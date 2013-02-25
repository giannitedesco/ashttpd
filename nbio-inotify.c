/*
 * This is the listener object it manages listening TCP sockets, for
 * each new connection that comes in we spawn off a new proxy object.
*/

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/inotify.h>

#include <compiler.h>
#include <list.h>
#include <nbio.h>
#include <nbio-inotify.h>
#include <os.h>

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

struct watch {
	int			w_d;
	void			*w_priv;
	const struct watch_ops	*w_ops;
};

struct _nbnotify {
	struct nbio		n_io;
	unsigned int		n_num_watch;
	struct watch		*n_watch;
};

static const char *mask2str(uint32_t mask)
{
	static char buf[256];
	unsigned int done, i;
	char *ptr = buf, *end = buf + sizeof(buf);
	static const struct {
		uint32_t val;
		const char *name;
	}flags[] = {
#define ENTRY(x) {.val = x, .name = #x}
		ENTRY(IN_ACCESS),
		ENTRY(IN_ATTRIB),
		ENTRY(IN_CLOSE_WRITE),
		ENTRY(IN_CLOSE_NOWRITE),
		ENTRY(IN_CREATE),
		ENTRY(IN_DELETE),
		ENTRY(IN_DELETE_SELF),
		ENTRY(IN_MODIFY),
		ENTRY(IN_MOVE_SELF),
		ENTRY(IN_MOVED_FROM),
		ENTRY(IN_MOVED_TO),
		ENTRY(IN_OPEN),
		ENTRY(IN_IGNORED),
		ENTRY(IN_ISDIR),
		ENTRY(IN_Q_OVERFLOW),
		ENTRY(IN_UNMOUNT),
	};

	for(done = i = 0; i < sizeof(flags)/sizeof(*flags); i++) {
		if ( !(mask & flags[i].val) )
			continue;
		ptr += snprintf(ptr, end - ptr,
				"%s%s",
				(done) ? ", " : "", flags[i].name);
		done = 1;
	}

	return buf;
}

static void dispatch(struct _nbnotify *n, const struct inotify_event *ev,
			const char *name)
{
	struct watch *w;
	char *nstr;
	int isdir;

	dprintf("wd: %d\n", ev->wd);
	dprintf("mask: %s\n", mask2str(ev->mask));
	dprintf("cookie: 0x%.8x\n", ev->cookie);
	dprintf("name: %.*s\n", (int)ev->len, ev->name);
	dprintf("\n");

	if ( ev->wd < 0 || (unsigned)ev->wd >= n->n_num_watch ) {
		fprintf(stderr, "spurious inotify event\n");
		return;
	}

	w = n->n_watch + ev->wd;
	if ( NULL == w->w_ops )
		return;

	isdir = !!(ev->mask & IN_ISDIR);
	if ( ev->len ) {
		nstr = malloc(ev->len + 1);
		if ( NULL == nstr ) {
			fprintf(stderr, "inotify dispatch: %s\n", os_err());
			return;
		}
		memcpy(nstr, name, ev->len);
		nstr[ev->len] = '\0';
	}else{
		nstr = NULL;
	}

	if ( ev->mask & IN_IGNORED ) {
		if ( ev->mask == IN_IGNORED ) {
			/* desc deleted */
		}
	}else if ( ev->mask & IN_CREATE ) {
		if ( w->w_ops->create )
			(*w->w_ops->create)(w->w_priv, nstr, isdir);
	}else if ( ev->mask & IN_DELETE ) {
		if ( w->w_ops->delete )
			(*w->w_ops->delete)(w->w_priv, nstr, isdir);
	}else if ( ev->mask & IN_MOVED_FROM ) {
		if ( w->w_ops->moved_from)
			(*w->w_ops->moved_from)(w->w_priv, nstr, isdir);
	}else if ( ev->mask & IN_MOVED_TO ) {
		if ( w->w_ops->moved_to)
			(*w->w_ops->moved_to)(w->w_priv, nstr, isdir);
	}else if ( ev->mask & IN_MOVE_SELF ) {
		if ( w->w_ops->move_self)
			(*w->w_ops->move_self)(w->w_priv);
	}else if ( ev->mask & (IN_DELETE_SELF|IN_UNMOUNT) ) {
		if ( w->w_ops->delete_self)
			(*w->w_ops->delete_self)(w->w_priv);
	}

	free(nstr);
	dprintf("\n");
}

static void efd_read(struct iothread *t, struct nbio *nbio)
{
	struct _nbnotify *n = (struct _nbnotify *)nbio;
	uint8_t buf[4096], *ptr, *end;
	const struct inotify_event *ev;
	ssize_t ret;

again:
	ret = read(n->n_io.fd, buf, sizeof(buf));
	if ( ret <= 0 )
		return;

	end = buf + ret;

	for(ptr = buf, ev = (struct inotify_event *)ptr;
			ptr < end;
			ptr += sizeof(*ev) + ev->len,
			ev = (struct inotify_event *)ptr) {
		dispatch(n, ev, ev->name);
	}

	goto again;
}

static void efd_dtor(struct iothread *t, struct nbio *nbio)
{
	struct _nbnotify *n = (struct _nbnotify *)nbio;
	unsigned int i;
	close(n->n_io.fd);
	for(i = 0; i < n->n_num_watch; i++) {
		struct watch *w = n->n_watch + i;
		if ( w->w_d < 0 )
			continue;
		if ( w->w_ops && w->w_ops->dtor )
			(*w->w_ops->dtor)(w->w_priv);
	}
	free(n->n_watch);
	free(n);
}

static const struct nbio_ops ops = {
	.read = efd_read,
	.dtor = efd_dtor,
};

nbnotify_t nbio_inotify_new(struct iothread *t)
{
	struct _nbnotify *n;

	n = calloc(1, sizeof(*n));
	if ( NULL == n ) {
		fprintf(stderr, "nbio_inotify_new: %s\n", os_err());
		return 0;
	}

	n->n_io.fd = inotify_init1(IN_NONBLOCK);
	if ( n->n_io.fd < 0 ) {
		fprintf(stderr, "inotify_init1: %s\n", os_err());
		free(n);
		return 0;
	}

	n->n_io.ops = &ops;
	nbio_add(t, &n->n_io, NBIO_READ);
	return n;
}

int nbio_inotify_watch_dir(nbnotify_t n, const char *dir,
					const struct watch_ops *ops,
					void *priv)
{
	struct watch *new;
	int wd;

	wd = inotify_add_watch(n->n_io.fd, dir,
				IN_CREATE|
				IN_DELETE|
				IN_DELETE_SELF|
				IN_MOVE_SELF|
				IN_MOVED_FROM|
				IN_MOVED_TO);
	if ( wd < 0 ) {
		fprintf(stderr, "inotify_add_watch: %s\n", os_err());
		return 0;
	}

	if ( n->n_num_watch < ((unsigned)wd + 1) ) {
		new = realloc(n->n_watch, sizeof(*new) * (wd + 1));
		if ( NULL == new ) {
			fprintf(stderr, "nbio_inotify_watch_dir: realloc: %s\n",
				os_err());
			inotify_rm_watch(n->n_io.fd, wd);
			return 0;
		}
		n->n_watch = new;
		n->n_watch[wd].w_d = -1;
		n->n_num_watch = wd + 1;
	}

	if ( n->n_watch[wd].w_d >= 0 ) {
		fprintf(stderr, "duplicate inotify watch\n");
		return 0;
	}

	n->n_watch[wd].w_d = wd;
	n->n_watch[wd].w_priv = priv;
	n->n_watch[wd].w_ops = ops;
	return 1;
}

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <ashttpd.h>
#include <ashttpd-conn.h>
#include <ashttpd-buf.h>
#include <nbio-inotify.h>
#include <normalize.h>
#include <hgang.h>
#include <critbit.h>

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

struct _vhosts {
	struct cb_tree vhosts;
	const char *dirname;
	webroot_t vdefault;
	nbnotify_t notify;
};

static void vhost_add(void *priv, const char *name, unsigned isdir)
{
	struct _vhosts *v = priv;
	char fn[strlen(v->dirname) + strlen(name) + 2];
	void **pptr;
	webroot_t w;

	if ( isdir )
		return;

	snprintf(fn, sizeof(fn), "%s/%s", v->dirname, name);
	printf("add vhost: %s -> %s\n", name, fn);

	w = webroot_open(fn);
	if ( NULL == w )
		return;

	if ( !strcmp(name, "__default__") ) {
		webroot_unref(v->vdefault);
		v->vdefault = w;
		return;
	}

	if ( !cb_insert(&v->vhosts, name, &pptr) )
		return;

	if ( *pptr ) {
		printf(" - closing old\n");
		webroot_unref(*pptr);
	}

	*pptr = w;
}

static void vhost_del(void *priv, const char *name, unsigned isdir)
{
	struct _vhosts *v = priv;
	webroot_t w;

	if ( isdir )
		return;
	printf("del vhost: %s\n", name);

	if ( !strcmp(name, "__default__") ) {
		webroot_unref(v->vdefault);
		v->vdefault = NULL;
		return;
	}

	if ( !cb_delete(&v->vhosts, name, (void **)&w) ) {
		printf(" - not found\n");
	}

	webroot_unref(w);
}

static void server_quit(void *priv)
{
	printf("quit now..\n");
}

static const struct watch_ops vhost_ops = {
	.create = vhost_add,
	.moved_to = vhost_add,
	.delete = vhost_del,
	.moved_from = vhost_del,
	.move_self = server_quit,
	.delete_self = server_quit,
};

static void dtor(void *priv)
{
	webroot_t w = priv;
	webroot_unref(w);
}

struct _vhosts *vhosts_new(struct iothread *t, const char *dirname)
{
	struct _vhosts *v = NULL;
	struct dirent *de;
	DIR *dir;

	v = calloc(1, sizeof(*v));
	if ( NULL == v )
		goto out;

	v->dirname = dirname;

	dir = opendir(dirname);
	if ( NULL == dir ) {
		fprintf(stderr, "opendir: %s: %s\n", dirname, os_err());
		goto out_free;
	}

	while ( (de = readdir(dir)) ) {
#if defined(_DIRENT_HAVE_D_TYPE)
		if ( de->d_type == DT_DIR )
			continue;
		/* if you put FIFO's, sockets, or dev nodes in here
		 * then what do you expect to happen?
		*/
#endif
		vhost_add(v, de->d_name, 0);
	}

	v->notify = nbio_inotify_new(t);
	if ( NULL == v->notify )
		goto out_closedir;

	if ( !nbio_inotify_watch_dir(v->notify, dirname, &vhost_ops, v) )
		goto out_denotify;

	/* sucess */
	goto out;

out_denotify:
	nbio_notify_free(t, v->notify);
out_closedir:
	cb_free(&v->vhosts, dtor);
	closedir(dir);
out_free:
	free(v);
	v = NULL;
out:
	return v;
}

webroot_t vhosts_lookup(vhosts_t v, const char *host)
{
	webroot_t w;

	if ( cb_contains(&v->vhosts, host, (void **)&w) ) {
		return w;
	}

	return v->vdefault;
}

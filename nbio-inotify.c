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

struct _nbnotify {
	struct nbio	n_io;
};

#include <ctype.h>
static void hex_dumpf(FILE *f, const uint8_t *tmp, size_t len, size_t llen)
{
	size_t i, j;
	size_t line;

	if ( NULL == f )
		return;
	if ( !llen )
		llen = 16;

	for(j = 0; j < len; j += line, tmp += line) {
		if ( j + llen > len ) {
			line = len - j;
		}else{
			line = llen;
		}

		fprintf(f, "%05zx : ", j);

		for(i = 0; i < line; i++) {
			if ( isprint(tmp[i]) ) {
				fprintf(f, "%c", tmp[i]);
			}else{
				fprintf(f, ".");
			}
		}

		for(; i < llen; i++)
			fprintf(f, " ");

		for(i = 0; i < line; i++)
			fprintf(f, " %02x", tmp[i]);

		fprintf(f, "\n");
	}
	fprintf(f, "\n");
}

static void efd_read(struct iothread *t, struct nbio *nbio)
{
	struct _nbnotify *n = (struct _nbnotify *)nbio;
	uint8_t buf[4096];
	ssize_t ret;
	ret = read(n->n_io.fd, buf, sizeof(buf));
	if ( ret <= 0 )
		return;
	hex_dumpf(stdout, buf, ret, 16);
}

static void efd_dtor(struct iothread *t, struct nbio *nbio)
{
	struct _nbnotify *n = (struct _nbnotify *)nbio;
	close(n->n_io.fd);
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

int nbio_inotify_watch_dir(nbnotify_t n, const char *dir)
{
	return inotify_add_watch(n->n_io.fd, dir,
				IN_CREATE|
				IN_DELETE|
				IN_DELETE_SELF|
				IN_MOVED_FROM|
				IN_MOVED_TO);
}

#include <ashttpd.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <errno.h>

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

static int io_sendfile_init(struct iothread *t, int webroot_fd)
{
	return 1;
}

static int io_sendfile_write(struct iothread *t, struct http_conn *h, int fd)
{
	ssize_t ret;
	off_t off = h->h_data_off;

	ret = sendfile(h->h_nbio.fd, fd, &off, h->h_data_len);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &h->h_nbio);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}

	h->h_data_off += (size_t)ret;
	h->h_data_len -= (size_t)ret;
	if ( 0 == h->h_data_len ) {
		nbio_set_wait(t, &h->h_nbio, NBIO_READ);
		h->h_state = HTTP_CONN_REQUEST;
	}

	return 1;
}

static int io_sendfile_prep(struct iothread *t, struct http_conn *h, int fd)
{
	return 1;
}

static void io_sendfile_abort(struct http_conn *h)
{
}

static void io_sendfile_fini(struct iothread *t)
{
}

struct http_fio fio_sendfile = {
	.label = "Synchronous Sendfile",
	.init = io_sendfile_init,
	.prep = io_sendfile_prep,
	.write = io_sendfile_write,
	.abort = io_sendfile_abort,
	.webroot_fd = generic_webroot_fd,
	.fini = io_sendfile_fini,
};

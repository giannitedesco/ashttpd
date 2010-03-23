#include <sys/socket.h>
#include <sys/sendfile.h>
#include <errno.h>

#include <ashttpd.h>
#include <ashttpd-conn.h>
#include <ashttpd-fio.h>
#include <ashttpd-buf.h>

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

static int io_sendfile_init(struct iothread *t, int webroot_fd)
{
	return os_sigpipe_ignore();
}

static int io_sendfile_write(struct iothread *t, http_conn_t h)
{
	ssize_t ret;
	size_t len;
	off_t off;
	int fd;

	len = http_conn_data(h, &fd, &off);

	/* let's try to be fair */
	/* TODO: does it even make a difference? */
	if ( len > 8192 )
		len = 8192;

	ret = sendfile(http_conn_socket(h), fd, &off, len);
	if ( ret < 0 && errno == EAGAIN ) {
		http_conn_inactive(t, h);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}

	if ( !http_conn_data_read(h, ret) )
		http_conn_data_complete(t, h);

	return 1;
}

static int io_sendfile_prep(struct iothread *t, http_conn_t h)
{
	return 1;
}

static void io_sendfile_abort(http_conn_t h)
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

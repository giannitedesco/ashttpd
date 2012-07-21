#include <sys/socket.h>
#include <libaio.h>
#include <errno.h>
#include <inttypes.h>
#include <ashttpd.h>
#include <ashttpd-conn.h>
#include <ashttpd-buf.h>
#include <ashttpd-fio.h>
#include <nbio-eventfd.h>
#include <hgang.h>

#if 1
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

#define AIO_QUEUE_SIZE		4096
static io_context_t aio_ctx;
static hgang_t aio_iocbs;
static struct nbio *efd;
static unsigned in_flight;

static int aio_submit(struct iothread *t, http_conn_t h)
{
	struct http_buf *data_buf;
	struct iocb *iocb;
	size_t data_len;
	off_t data_off;
	uint8_t *ptr;
	int ret, fd;
	size_t sz;

	data_len = http_conn_data(h, &fd, &data_off);
	assert(data_len);

	iocb = hgang_alloc(aio_iocbs);
	if ( NULL == iocb )
		return 0;

	data_buf = http_conn_get_priv(h, NULL);
	ptr = buf_write(data_buf, &sz);
	sz = (data_len < sz) ? data_len : sz;

	io_prep_pread(iocb, fd, ptr, sz, data_off);
	iocb->data = h;
	io_set_eventfd(iocb, efd->fd);

	ret = io_submit(aio_ctx, 1, &iocb);
	if ( ret <= 0 ) {
		errno = -ret;
		fprintf(stderr, "io_submit: %s\n", os_err());
		return 0;
	}

	dprintf("io_submit: pread: %zu bytes\n", sz);
	http_conn_to_waitq(t, h, NULL);
	in_flight++;
	return 1;
}

static void handle_completion(struct iothread *t, struct iocb *iocb,
				http_conn_t h, int ret)
{
	struct http_buf *data_buf;

	hgang_return(aio_iocbs, iocb);
	in_flight--;

	if ( ret <= 0 ) {
		errno = -ret;
		fprintf(stderr, "io_pread: %s\n", os_err());
		http_conn_abort(t, h);
		return;
	}

	data_buf = http_conn_get_priv(h, NULL);
	buf_done_write(data_buf, ret);
	http_conn_wake_one(t, h);
}

static void aio_event(struct iothread *t, void *priv, eventfd_t val)
{
	struct io_event ev[in_flight];
	struct timespec tmo;
	int ret, i;

	/* Spurious eventfd wakeup */
	if ( !in_flight )
		return;

	memset(&tmo, 0, sizeof(tmo));

	dprintf("aio_event ready, %"PRIu64"/%u in flight\n", val, in_flight);

	ret = io_getevents(aio_ctx, 1, in_flight, ev, &tmo);
	if ( ret < 0 ) {
		errno = -ret;
		fprintf(stderr, "io_getevents: %s\n", os_err());
		return;
	}

	for(i = 0; i < ret; i++)
		handle_completion(t, ev[i].obj, ev[i].data, ev[i].res);
}

static int io_async_init(struct iothread *t)
{
	int ret;

	memset(&aio_ctx, 0, sizeof(aio_ctx));
	ret = io_queue_init(AIO_QUEUE_SIZE, &aio_ctx);
	if ( ret < 0 ) {
		errno = -ret;
		fprintf(stderr, "io_queue_init: %s\n", os_err());
		return 0;
	}

	aio_iocbs = hgang_new(sizeof(struct iocb), 0);
	if ( NULL == aio_iocbs )
		return 0;

	efd = nbio_eventfd_new(0, aio_event, NULL);
	if ( NULL == efd )
		return 0;
	nbio_eventfd_add(t, efd);
	return 1;
}

static int io_async_write(struct iothread *t, http_conn_t h)
{
	struct http_buf *data_buf;
	int flags = MSG_NOSIGNAL;
	const uint8_t *ptr;
	size_t data_len;
	ssize_t ret;
	size_t sz;

	dprintf("\n");

	data_buf = http_conn_get_priv(h, NULL);
	ptr = buf_read(data_buf, &sz);
	data_len = http_conn_data(h, NULL, NULL);
	if ( data_len > sz )
		flags |= MSG_MORE;

	ret = send(http_conn_socket(h), ptr, sz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		http_conn_inactive(t, h);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}

	dprintf("Transmitted %zu\n", (size_t)ret);
	sz = buf_done_read(data_buf, ret);
	data_len = http_conn_data_read(h, ret);
	if ( sz ) {
		dprintf("Partial transmit: %zu bytes left\n", sz);
		return 1;
	}

	if ( data_len ) {
		dprintf("Submit more, %zu bytes left\n", data_len);
		buf_reset(data_buf);
		return aio_submit(t, h);
	}

	buf_free_data(data_buf);
	http_conn_set_priv(h, NULL, 0);
	http_conn_data_complete(t, h);
	dprintf("DONE\n");

	return 1;
}

static int io_async_prep(struct iothread *t, http_conn_t h)
{
	struct http_buf *data_buf;

	data_buf = buf_alloc_data();
	dprintf("allocated buffer\n");
	if ( NULL == data_buf ) {
		printf("OOM on data...\n");
		return 0;
	}

	http_conn_set_priv(h, data_buf, 0);
	if ( !aio_submit(t, h) ) {
		buf_free_data(data_buf);
		http_conn_set_priv(h, NULL, 0);
		return 0;
	}

	return 1;
}

static void io_async_abort(http_conn_t h)
{
	struct http_buf *data_buf;
	data_buf = http_conn_get_priv(h, NULL);
	buf_free_data(data_buf);
}

static void io_async_fini(struct iothread *t)
{
	/* fuck it */
}

struct http_fio fio_async = {
	.label = "File AIO",
	.prep = io_async_prep,
	.write = io_async_write,
	.abort = io_async_abort,
	.init = io_async_init,
	.fini = io_async_fini,
};

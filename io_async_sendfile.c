#include <sys/socket.h>
#include <errno.h>
#include <inttypes.h>
#include <libaio.h>

#include <ashttpd.h>
#include <ashttpd-conn.h>
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
	struct iocb *iocb;
	size_t data_len;
	off_t data_off;
	int ret, fd;

	data_len = http_conn_data(h, &fd, &data_off);
	assert(data_len);

	iocb = hgang_alloc(aio_iocbs);
	if ( NULL == iocb )
		return 0;

	io_prep_sendfile(iocb, fd, data_len, data_off, http_conn_socket(h));
	iocb->data = h;
	io_set_eventfd(iocb, efd->fd);

	ret = io_submit(aio_ctx, 1, &iocb);
	if ( ret <= 0 ) {
		errno = -ret;
		fprintf(stderr, "io_submit: %s\n", os_err());
		return 0;
	}

	dprintf("io_submit: sendfile: %zu bytes\n", data_len);
	in_flight++;
	return 1;
}

static void handle_completion(struct iothread *t, struct iocb *iocb,
				http_conn_t h, int ret)
{
	hgang_return(aio_iocbs, iocb);
	in_flight--;

	if ( ret > 0 ) {
		size_t data_len;
		data_len = http_conn_data_read(h, ret);
		if ( data_len ) {
			printf("re-submit from completion\n");
			if ( !aio_submit(t, h) )
				http_conn_abort(t, h);
		}else{
			dprintf("aio_sendfile: done\n");
			/* automatically removes from waitq */
			http_conn_data_complete(t, h);
		}
		return;
	}

	if ( ret == -EAGAIN ) {
		dprintf("aio_sendfile: failed EAGAIN\n");
		http_conn_wait_on(t, h, NBIO_WRITE);
		return;
	}else if (ret < 0 ) {
		errno = -ret;
		printf("aio_sendfile: %s\n", os_err());
	}
	http_conn_abort(t, h);
}

static void aio_event(struct iothread *t, void *priv, eventfd_t val)
{
	struct io_event ev[in_flight];
	struct timespec tmo;
	int ret, i;

	memset(&tmo, 0, sizeof(tmo));

	dprintf("aio_event ready, %"PRIu64"/%u in flight\n", val, in_flight);

	ret = io_getevents(aio_ctx, 1, in_flight, ev, &tmo);
	if ( ret < 0 ) {
		fprintf(stderr, "io_getevents: %s\n", os_err());
		return;
	}

	for(i = 0; i < ret; i++)
		handle_completion(t, ev[i].obj, ev[i].data, ev[i].res);
}

static int io_async_sendfile_init(struct iothread *t)
{
	memset(&aio_ctx, 0, sizeof(aio_ctx));
	if ( io_queue_init(AIO_QUEUE_SIZE, &aio_ctx) ) {
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

static int io_async_sendfile_write(struct iothread *t, http_conn_t h)
{
	dprintf("aio_sendfile: socket went writable\n");
	http_conn_to_waitq(t, h, NULL);
	return aio_submit(t, h);
}

static int io_async_sendfile_prep(struct iothread *t, http_conn_t h)
{
	return 1;
}

static void io_async_sendfile_abort(http_conn_t h)
{
}

static void io_async_sendfile_fini(struct iothread *t)
{
}

struct http_fio fio_async_sendfile = {
	.label = "Kernel AIO sendfile",
	.prep = io_async_sendfile_prep,
	.write = io_async_sendfile_write,
	.abort = io_async_sendfile_abort,
	.init = io_async_sendfile_init,
	.fini = io_async_sendfile_fini,
};

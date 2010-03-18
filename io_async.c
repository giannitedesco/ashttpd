#include <ashttpd.h>
#include <nbio-eventfd.h>
#include <assert.h>
#include "../libaio/src/libaio.h"

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

#define AIO_QUEUE_SIZE		4096
static io_context_t aio_ctx;
static hgang_t aio_iocbs;
static struct nbio *efd;
static unsigned in_flight;

static int aio_underflow(struct iothread *t, struct http_conn *h, int fd)
{
	struct iocb *iocb;
	uint8_t *ptr;
	size_t sz;

	if ( h->h_data_len == 0 )
		return 1;

	iocb = hgang_alloc0(aio_iocbs);
	if ( NULL == iocb )
		return 0;

	if ( NULL == h->h_async ) {
		h->h_async = buf_alloc_data();
		dprintf("allocated buffer\n");
		if ( NULL == h->h_async ) {
			hgang_return(aio_iocbs, iocb);
			printf("OOM on res...\n");
			return 0;
		}
	}

	ptr = buf_write(h->h_async, &sz);
	sz = (h->h_data_len < sz) ? h->h_data_len : sz;

	io_prep_pread(iocb, fd, ptr, sz, h->h_data_off);
	iocb->data = h;
	io_set_eventfd(iocb, efd->fd);

	if ( io_submit(aio_ctx, 1, &iocb) <= 0 ) {
		fprintf(stderr, "io_submit: %s\n", os_err());
		return 0;
	}

	dprintf("io_submit: pread: %u bytes\n", sz);
	in_flight++;
	return 1;
}

static void flip_buffers(struct http_conn *h)
{
	struct http_buf *tmp;

	dprintf("flipping buffers\n");
	tmp = h->h_dat;
	h->h_dat = h->h_async;
	h->h_async = tmp;
	if ( h->h_async )
		buf_reset(h->h_async);
}

static void handle_completion(struct iothread *t, struct iocb *iocb,
				struct http_conn *h, int ret)
{
	int fd = iocb->aio_fildes;

	assert(h->h_state == HTTP_CONN_DATA);

	hgang_return(aio_iocbs, iocb);
	in_flight--;

	if ( ret <= 0 ) {
		/* FIXME: signal error to io_async_write() */
		errno = -ret;
		fprintf(stderr, "aio_pread: %s\n", os_err());
		return;
	}

	buf_done_write(h->h_async, ret);
	h->h_data_off += (size_t)ret;
	h->h_data_len -= (size_t)ret;

	if ( NULL == h->h_dat ) {
		dprintf("submit from completion\n");
		flip_buffers(h);
		aio_underflow(t, h, fd);
		/* FIXME: Same herre */
	}

	nbio_set_wait(t, &h->h_nbio, NBIO_WRITE);
}

static void aio_event(struct iothread *t, void *priv, eventfd_t val)
{
	struct io_event ev[in_flight];
	struct timespec tmo;
	int ret, i;

	memset(&tmo, 0, sizeof(tmo));

	dprintf("aio_event ready, %llu/%u in flight\n", val, in_flight);

	ret = io_getevents(aio_ctx, 1, in_flight, ev, &tmo);
	if ( ret < 0 ) {
		fprintf(stderr, "io_getevents: %s\n", os_err());
		return;
	}

	for(i = 0; i < ret; i++)
		handle_completion(t, ev[i].obj, ev[i].data, ev[i].res);
}

int io_async_init(struct iothread *t)
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

int io_async_write(struct iothread *t, struct http_conn *h, int fd)
{
	int flags = MSG_NOSIGNAL;
	const uint8_t *ptr;
	ssize_t ret;
	size_t sz;

	dprintf("\n");

	if ( NULL == h->h_dat ) {
		dprintf("Async read not finished, sleeeeep....\n");
		nbio_set_wait(t, &h->h_nbio, 0);
		return 1;
	}

	/* FIXME: MSG_MORE semantics */
	ptr = buf_read(h->h_dat, &sz);
	ret = send(h->h_nbio.fd, ptr, sz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &h->h_nbio);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}

	dprintf("Transmitted %u\n", (size_t)ret);
	buf_done_read(h->h_dat, ret);
	buf_read(h->h_dat, &sz);

	if ( sz ) {
		dprintf("Partial transmit: %u bytes left\n", sz);
		return 1;
	}

	if ( NULL == h->h_async ) {
		if ( h->h_data_len == 0 )
			goto done;
		dprintf("Underflow??\n");
		flip_buffers(h);
		return aio_underflow(t, h, fd);
	}

	ptr = buf_read(h->h_async, &sz);
	if ( sz ) {
		dprintf("Fast flip %u bytes completed\n", sz);
		flip_buffers(h);
		return aio_underflow(t, h, fd);
	}else{
		dprintf("yeah hmm\n");
	}

done:
	buf_free_data(h->h_dat);
	h->h_dat = NULL;

	if ( h->h_data_len ) {
		dprintf("I/O pending, burned buffer %u bytes left\n",
			h->h_data_len);
	}else{
		buf_free_data(h->h_async);
		h->h_async = NULL;
		nbio_set_wait(t, &h->h_nbio, NBIO_READ);
		h->h_state = HTTP_CONN_REQUEST;
		h->h_req = buf_alloc_req();
		if ( NULL == h->h_req ) {
			printf("OOM on res after async data...\n");
			return 0;
		}
		dprintf("DONE\n");
	}

	return 1;
}

int io_async_prep(struct iothread *t, struct http_conn *h, int fd)
{
	return aio_underflow(t, h, fd);
}

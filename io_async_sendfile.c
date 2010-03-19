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

static int aio_submit(struct iothread *t, struct http_conn *h, int fd)
{
	struct iocb *iocb;
	int ret;

	assert(h->h_data_len);

	iocb = hgang_alloc0(aio_iocbs);
	if ( NULL == iocb )
		return 0;

	io_prep_sendfile(iocb, fd, h->h_data_len, h->h_data_off, h->h_nbio.fd);
	iocb->data = h;
	io_set_eventfd(iocb, efd->fd);

	ret = io_submit(aio_ctx, 1, &iocb);
	if ( ret <= 0 ) {
		errno = -ret;
		fprintf(stderr, "io_submit: %s\n", os_err());
		return 0;
	}

	dprintf("io_submit: sendfile: %u bytes\n", h->h_data_len);
	in_flight++;
	return 1;
}

static void handle_completion(struct iothread *t, struct iocb *iocb,
				struct http_conn *h, int ret)
{
	int fd;

	assert(h->h_state == HTTP_CONN_DATA);

	fd = iocb->aio_fildes;
	hgang_return(aio_iocbs, iocb);
	in_flight--;

	if ( ret > 0 ) {
		assert((size_t)ret <= h->h_data_len);
		h->h_data_off += (size_t)ret;
		h->h_data_len -= (size_t)ret;
		if ( h->h_data_len ) {
			printf("re-submit from completion\n");
			if ( !aio_submit(t, h, fd) )
				nbio_del(t, &h->h_nbio);
		}else{
			dprintf("aio_sendfile: done\n");
			h->h_state = HTTP_CONN_REQUEST;
			nbio_set_wait(t, &h->h_nbio, NBIO_READ);
			nbio_inactive(t, &h->h_nbio);
		}
		return;
	}

	if ( ret == -EAGAIN ) {
		dprintf("aio_sendfile: failed EAGAIN\n");
		nbio_set_wait(t, &h->h_nbio, NBIO_WRITE);
		nbio_inactive(t, &h->h_nbio);
		return;
	}else if (ret < 0 ) {
		errno = -ret;
		printf("aio_sendfile: %s\n", os_err());
	}
	nbio_del(t, &h->h_nbio);
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

int io_async_sendfile_init(struct iothread *t)
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

int io_async_sendfile_write(struct iothread *t, struct http_conn *h, int fd)
{
	dprintf("aio_sendfile: socket went writable\n");
	nbio_set_wait(t, &h->h_nbio, 0);
	return aio_submit(t, h, fd);
}

int io_async_sendfile_prep(struct iothread *t, struct http_conn *h, int fd)
{
	return 1;
}

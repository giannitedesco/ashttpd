#include <ashttpd.h>
#include <sys/socket.h>
#include <errno.h>

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

static int io_sync_init(struct iothread *t)
{
	return 1;
}

static int io_sync_write(struct iothread *t, struct http_conn *h, int fd)
{
	int flags = MSG_NOSIGNAL;
	const uint8_t *rptr;
	size_t wsz, rsz;
	uint8_t *wptr;
	ssize_t ret;

	/* Top up buffer if necessary */
	wptr = buf_write(h->h_dat, &wsz);
	if ( wsz ) {
		size_t sz;
		int eof = 0;
		
		sz = ( wsz > h->h_data_len ) ? h->h_data_len : wsz;

		if ( !fd_pread(fd, h->h_data_off,
			wptr, &sz, &eof) || eof ) {
			return 0;
		}

		dprintf("Read %u bytes in to buffer\n", sz);
		buf_done_write(h->h_dat, sz);
		h->h_data_off += sz;
		h->h_data_len -= sz;
	}

	if ( h->h_data_len )
		flags |= MSG_MORE;

	rptr = buf_read(h->h_dat, &rsz);
	ret = send(h->h_nbio.fd, rptr, rsz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &h->h_nbio);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}

	buf_done_read(h->h_dat, ret);
	buf_read(h->h_dat, &rsz);
	dprintf("Transmitted %u/%u bytes, %u left in file\n",
		(size_t)ret, rsz, h->h_data_len);

	if ( rsz + h->h_data_len == 0 ) {
		buf_free_data(h->h_dat);
		h->h_dat = NULL;

		nbio_set_wait(t, &h->h_nbio, NBIO_READ);
		h->h_state = HTTP_CONN_REQUEST;
		dprintf("DONE\n");
	}else{
		buf_write(h->h_dat, &wsz);
		if ( h->h_data_len > wsz ) {
			if ( wsz )
				dprintf("resetting %u byte data buffer\n", wsz);
			buf_reset(h->h_dat);
		}
	}
	return 1;
}

static int io_sync_prep(struct iothread *t, struct http_conn *h, int fd)
{
	h->h_dat = buf_alloc_data();
	if ( NULL == h->h_dat ) {
		printf("OOM on res...\n");
		return 0;
	}

	return 1;
}

static void io_sync_fini(struct iothread *t)
{
}

struct http_fio fio_sync = {
	.label = "Traditional synchronous",
	.init = io_sync_init,
	.prep = io_sync_prep,
	.write = io_sync_write,
	.webroot_fd = generic_webroot_fd,
	.fini = io_sync_fini,
};

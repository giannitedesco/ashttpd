#include <sys/socket.h>
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

static int io_sync_init(struct iothread *t)
{
	return 1;
}

static int io_sync_write(struct iothread *t, http_conn_t h)
{
	struct http_buf *data_buf;
	int flags = MSG_NOSIGNAL;
	size_t data_len;
	off_t data_off;
	const uint8_t *rptr;
	size_t wsz, rsz;
	uint8_t *wptr;
	ssize_t ret;
	int fd;

	data_buf = http_conn_get_priv(h, NULL);
	data_len = http_conn_data(h, &fd, &data_off);

	/* Top up buffer if necessary */
	wptr = buf_write(data_buf, &wsz);
	if ( wsz ) {
		size_t sz;
		int eof = 0;

		sz = ( wsz > data_len ) ? data_len : wsz;

		if ( !fd_pread(fd, data_off, wptr, &sz, &eof) || eof ) {
			return 0;
		}

		dprintf("Read %u bytes in to buffer\n", sz);
		buf_done_write(data_buf, sz);
		http_conn_data_read(h, sz);
		data_len -= sz;
	}

	if ( data_len )
		flags |= MSG_MORE;

	rptr = buf_read(data_buf, &rsz);
	ret = send(http_conn_socket(h), rptr, rsz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		http_conn_inactive(t, h);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}

	buf_done_read(data_buf, ret);
	buf_read(data_buf, &rsz);
	dprintf("Transmitted %u/%u bytes, %u left in file\n",
		(size_t)ret, rsz, data_len);

	if ( rsz + data_len == 0 ) {
		http_conn_set_priv(h, NULL, 0);
		buf_free_data(data_buf);
		http_conn_data_complete(t, h);
	}else{
		buf_write(data_buf, &wsz);
		if ( data_len > wsz ) {
			if ( wsz )
				dprintf("resetting %u byte data buffer\n", wsz);
			buf_reset(data_buf);
		}
	}

	return 1;
}

static int io_sync_prep(struct iothread *t, http_conn_t h)
{
	struct http_buf *data_buf;

	data_buf = buf_alloc_data();
	if ( NULL == data_buf ) {
		printf("OOM on data...\n");
		return 0;
	}

	http_conn_set_priv(h, data_buf, 0);
	return 1;
}

static void io_sync_abort(http_conn_t h)
{
	struct http_buf *data_buf;
	data_buf = http_conn_get_priv(h, NULL);
	buf_free_data(data_buf);
}

static void io_sync_fini(struct iothread *t)
{
}

struct http_fio fio_sync = {
	.label = "Traditional synchronous",
	.init = io_sync_init,
	.prep = io_sync_prep,
	.write = io_sync_write,
	.abort = io_sync_abort,
	.fini = io_sync_fini,
};

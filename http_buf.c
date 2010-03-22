#include <fcntl.h>
#include <unistd.h>

#include <ashttpd.h>
#include <hgang.h>
#include <ashttpd-buf.h>

static hgang_t h_req;
static hgang_t h_res;
static hgang_t h_dat;
static hgang_t h_buf;

static void __attribute__((constructor)) http_conn_ctor(void)
{
	h_buf = hgang_new(sizeof(struct http_buf), 256);
	h_res = hgang_new(HTTP_MAX_REQ, 16);
	h_req = hgang_new(HTTP_MAX_RESP, 8);
	h_dat = hgang_new(HTTP_DATA_BUFFER, 32);
}

static struct http_buf *do_alloc(hgang_t alloc)
{
	struct http_buf *b;
	size_t sz;

	b = hgang_alloc(h_buf);
	if ( NULL == b )
		return NULL;

	if ( NULL == alloc )
		return b;

	b->b_base = hgang_alloc(alloc);
	if ( NULL == b->b_base ) {
		hgang_return(h_buf, b);
		return NULL;
	}

	sz = hgang_object_size(alloc);
	b->b_end = b->b_base + sz;
	b->b_read = b->b_base;
	b->b_write = b->b_base;
	return b;
}

static void do_free(hgang_t alloc, struct http_buf *b)
{
	if ( b ) {
		if ( alloc )
			hgang_return(alloc, b->b_base);
		hgang_return(h_buf, b);
	}
}

struct http_buf *buf_alloc_naked(void)
{
	return do_alloc(NULL);
}

void buf_free_naked(struct http_buf *b)
{
	return do_free(NULL, b);
}

struct http_buf *buf_alloc_req(void)
{
	return do_alloc(h_req);
}

void buf_free_req(struct http_buf *b)
{
	do_free(h_req, b);
}

struct http_buf *buf_alloc_res(void)
{
	return do_alloc(h_res);
}

void buf_free_res(struct http_buf *b)
{
	do_free(h_res, b);
}

struct http_buf *buf_alloc_data(void)
{
	return do_alloc(h_res);
}

void buf_free_data(struct http_buf *b)
{
	do_free(h_dat, b);
}

const uint8_t *buf_read(struct http_buf *b, size_t *sz)
{
	assert(b->b_write >= b->b_read);
	*sz = b->b_write - b->b_read;
	return (b->b_read) ? b->b_read : NULL;
}

uint8_t *buf_write(struct http_buf *b, size_t *sz)
{
	assert(b->b_write <= b->b_end);
	*sz = b->b_end - b->b_write;
	return (b->b_write < b->b_end) ? b->b_write : NULL;
}

size_t buf_done_read(struct http_buf *b, size_t sz)
{
	assert(b->b_read + sz <= b->b_write);
	assert(b->b_read + sz <= b->b_end);
	b->b_read += sz;
	return b->b_write - b->b_read;
}

size_t buf_done_write(struct http_buf *b, size_t sz)
{
	assert(b->b_write + sz <= b->b_end);
	b->b_write += sz;
	return b->b_end - b->b_write;
}

void buf_reset(struct http_buf *b)
{
	size_t res;

	assert(b->b_read <= b->b_write);
	assert(b->b_read <= b->b_end);
	if ( b->b_read == b->b_write ) {
		b->b_read = b->b_base;
		b->b_write = b->b_base;
		return;
	}

	res = b->b_write - b->b_read;
	memmove(b->b_base, b->b_read, res);
	b->b_read = b->b_base;
	b->b_write = b->b_base + res;
}

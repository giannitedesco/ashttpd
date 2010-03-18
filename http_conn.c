#include <ashttpd.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include "webroot.h"

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

static unsigned int concurrency;

static hgang_t conns;
static int rt_skip[BM_SKIP_LEN];
static const uint8_t http_req_terminator[4] = "\r\n\r\n";
static int webroot_fd = -1;

static void __attribute__((constructor)) http_conn_ctor(void)
{
	conns = hgang_new(sizeof(struct http_conn), 0);
	bm_skip(http_req_terminator, sizeof(http_req_terminator), rt_skip);
	webroot_fd = open("webroot.objdb", O_RDONLY);

	/* TODO: preallocate fake buffers for out-of-resources */
}

static const char * const resp400 =
	"HTTP/1.1 400 Bad Request\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 77\r\n"
	"\r\n"
	"<html><head><title>Fuck Off</title></head>"
	"<body><h1>Bad Request</body></html>";

static int http_write_hdr(struct iothread *t, struct http_conn *h)
{
	const uint8_t *ptr;
	size_t sz;
	ssize_t ret;
	int flags = MSG_NOSIGNAL;

	if ( h->h_data_len )
		flags |= MSG_MORE;

	ptr = buf_read(h->h_res, &sz);

	ret = send(h->h_nbio.fd, ptr, sz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &h->h_nbio);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}

	buf_done_read(h->h_res, ret);

	ptr = buf_read(h->h_res, &sz);
	if ( sz == 0 ) {
		buf_free_res(h->h_res);
		h->h_res = NULL;

		if ( h->h_data_len ) {
			dprintf("Header done, %u bytes of data\n",
				h->h_data_len);
			h->h_state = HTTP_CONN_DATA;
		}else{
			nbio_set_wait(t, &h->h_nbio, NBIO_READ);
			h->h_state = HTTP_CONN_REQUEST;
			assert(NULL == h->h_dat);
			h->h_req = buf_alloc_req();
			if ( NULL == h->h_req ) {
				printf("OOM on res after header...\n");
				return 0;
			}
		}
	}

	return 1;
}

static void http_write(struct iothread *t, struct nbio *n)
{
	struct http_conn *h;
	int ret = 0;

	h = (struct http_conn *)n;

	switch(h->h_state) {
	case HTTP_CONN_HEADER:
		ret = http_write_hdr(t, h);
		break;
	case HTTP_CONN_DATA:
		ret = _io_write(t, h, webroot_fd);
		break;
	default:
		dprintf("uh %u\n", h->h_state);
		abort();
	}

	if ( !ret )
		nbio_del(t, &h->h_nbio);
}

static int response_400(struct iothread *t, struct http_conn *h)
{
	uint8_t *ptr;
	size_t sz;

	ptr = buf_write(h->h_res, &sz);
	assert(NULL != ptr && sz >= strlen(resp400));

	memcpy(ptr, resp400, strlen(resp400));
	buf_done_write(h->h_res, strlen(resp400));
	h->h_data_len = 0;
	return 1;
}

static const struct webroot_name *webroot_find(struct ro_vec *uri)
{
	const struct webroot_name *haystack;
	unsigned int n;

	for(n = sizeof(webroot_namedb)/sizeof(*webroot_namedb),
			haystack = webroot_namedb; n; ) {
	 	unsigned int i;
		int cmp;

		i = n / 2U;

		cmp = vcmp_fast(uri, &haystack[i].name);
		if ( cmp < 0 ) {
			n = i;
		}else if ( cmp > 0 ) {
			haystack = haystack + (i + 1U);
			n = n - (i + 1U);
		}else
			return haystack + i;
	}

	return NULL;
}

static int handle_get(struct iothread *t, struct http_conn *h,
			struct http_request *r)
{
	const struct webroot_name *n;
	unsigned int code, mime_type;
	struct ro_vec search_uri = r->uri;
	uint8_t *ptr;
	size_t sz;
	int len;

	if ( search_uri.v_len > 1 &&
		search_uri.v_ptr[search_uri.v_len - 1] == '/' )
		search_uri.v_len--;

	//printf("get %.*s\n", r->uri.v_len, r->uri.v_ptr);
	n = webroot_find(&search_uri);
	if ( NULL == n ) {
		h->h_data_off = obj404_f_ofs;
		h->h_data_len = obj404_f_len;
		mime_type = obj404_mime_type;
		code = 404;
	}else{
		h->h_data_off = n->f_ofs;
		h->h_data_len = n->f_len;
		mime_type = n->mime_type;
		code = 200;
	}

	if ( h->h_data_len ) {
		if ( !_io_prep(t, h, webroot_fd) )
			return 0;
	}

	ptr = buf_write(h->h_res, &sz);
	len = snprintf((char *)ptr, sz,
			"HTTP/1.1 %u %s\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %u\r\n"
			"Connection: keep-alive\r\n"
			"\r\n",
			code,
			(code == 200) ? "OK": "Not Found",
			mime_types[mime_type],
			h->h_data_len);
	if ( len < 0 )
		len = 0;
	if ( (size_t)len == sz ) {
		printf("Truncated header...\n");
		return 0;
	}

	buf_done_write(h->h_res, len);
	return 1;
}

static void handle_request(struct iothread *t, struct http_conn *h)
{
	struct http_request r;
	size_t hlen, sz;
	const uint8_t *ptr;
	int ret;

	assert(h->h_state == HTTP_CONN_REQUEST);

	memset(&r, 0, sizeof(r));
	ptr = buf_read(h->h_req, &sz);
	hlen = http_req(&r, ptr, sz);

	nbio_set_wait(t, &h->h_nbio, NBIO_WRITE);
	h->h_state = HTTP_CONN_HEADER;
	h->h_res = buf_alloc_res();

	if ( NULL == h->h_res ) {
		printf("OOM on res...\n");
		nbio_del(t, &h->h_nbio);
		return;
	}else if ( vstrcmp_fast(&r.method, "GET") ) {
		/* FIXME: bad request */
		ret = response_400(t, h);
	}else{
		ret = handle_get(t, h, &r);
	}

	if ( !ret ) {
		nbio_del(t, &h->h_nbio);
		return;
	}

	dprintf("%u/%u bytes were request\n", hlen, sz);
	buf_done_read(h->h_req, hlen);

	buf_read(h->h_req, &sz);
	if ( sz ) {
		buf_reset(h->h_req);
		return;
	}else{
		buf_free_req(h->h_req);
		h->h_req = NULL;
	}
}

static void http_read(struct iothread *t, struct nbio *nbio)
{
	const uint8_t *hs, *n;
	struct http_conn *h;
	uint8_t *ptr;
	ssize_t ret;
	size_t sz;

	h = (struct http_conn *)nbio;

	assert(h->h_state == HTTP_CONN_REQUEST);

	ptr = buf_write(h->h_req, &sz);
	if ( 0 == sz ) {
		printf("OOM on req...\n");
		nbio_del(t, nbio);
		return;
	}

	ret = recv(h->h_nbio.fd, ptr, sz, 0);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, nbio);
		return;
	}else if ( ret <= 0 ) {
		nbio_del(t, nbio);
		return;
	}

	dprintf("Received %u bytes: %.*s\n",
		ret, ret, ptr);
	buf_done_write(h->h_req, ret);

	/* FIXME: can handle LF/CRLF/mixed more efficiently than this */
	dprintf("Searching for terminating %u bytes\n",
		sizeof(http_req_terminator));
	hs = buf_read(h->h_req, &sz);
	n = bm_find(http_req_terminator, sizeof(http_req_terminator),
		hs, sz, rt_skip);
	if ( NULL == n ) {
		dprintf("Nah, waiting for more data\n");
		return;
	}

	handle_request(t, h);
}

static void http_dtor(struct iothread *t, struct nbio *n)
{
	struct http_conn *h;
	h = (struct http_conn *)n;
	dprintf("Connection killed\n");
	buf_free_req(h->h_req);
	buf_free_res(h->h_res);
	buf_free_data(h->h_dat);
	fd_close(h->h_nbio.fd);
	--concurrency;
}

static const struct nbio_ops http_ops = {
	.read = http_read,
	.write = http_write,
	.dtor = http_dtor,
};

void http_conn(struct iothread *t, int s, void *priv)
{
	struct http_conn *h;

	h = hgang_alloc0(conns);
	if ( NULL == h ) {
		fprintf(stderr, "hgang_alloc: %s\n", os_err());
		fd_close(s);
		return;
	}

	h->h_state = HTTP_CONN_REQUEST;
	h->h_req = buf_alloc_req();
	if ( NULL == h->h_req ) {
		printf("OOM on res on accept...\n");
		fd_close(s);
		return;
	}

	h->h_nbio.fd = s;
	h->h_nbio.ops = &http_ops;
	nbio_add(t, &h->h_nbio, NBIO_READ);
	concurrency++;
	if ( (concurrency % 1000) == 0 )
		printf("concurrency %u\n", concurrency);
	return;
}

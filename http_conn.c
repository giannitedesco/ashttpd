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

static hgang_t conns;
static hgang_t buffs;
static hgang_t hbufs;
static int rt_skip[BM_SKIP_LEN];
static const uint8_t http_req_terminator[4] = "\r\n\r\n";
static int webroot_fd;

static void __attribute__((constructor)) http_conn_ctor(void)
{
	conns = hgang_new(sizeof(struct http_conn), 16);
	buffs = hgang_new(HTTP_MAX_REQ, 8);
	hbufs = hgang_new(HTTP_MAX_RESP, 8);
	bm_skip(http_req_terminator, sizeof(http_req_terminator), rt_skip);
	webroot_fd = open("webroot.objdb", O_RDONLY);
}

static const char * const resp400 =
	"HTTP/1.1 400 Bad Request\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 77\r\n"
	"\r\n"
	"<html><head><title>Fuck Off</title></head>"
	"<body><h1>Bad Request</body></html>";

static void http_write_hdr(struct iothread *t, struct http_conn *h)
{
	ssize_t ret;
	int flags = MSG_NOSIGNAL;

	if ( h->h_data_len )
		flags |= MSG_MORE;

	ret = send(h->h_nbio.fd, h->h_res_ptr,
			h->h_res_end - h->h_res_ptr, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &h->h_nbio);
		return;
	}else if ( ret <= 0 ) {
		nbio_del(t, &h->h_nbio);
		return;
	}

	h->h_res_ptr += (size_t)ret;

	if ( h->h_res_ptr == h->h_res_end ) {
		if ( h->h_data_len ) {
			dprintf("Header done, %u bytes of data\n",
				h->h_data_len);
			h->h_state = HTTP_CONN_DATA;
		}else{
			nbio_set_wait(t, &h->h_nbio, NBIO_READ);
			h->h_state = HTTP_CONN_REQUEST;
		}
	}
}

static void http_write_data_sync(struct iothread *t, struct http_conn *h)
{
	int flags = MSG_NOSIGNAL;
	char buf[8192];
	ssize_t ret;
	size_t sz;
	int eof = 0;

	sz = (h->h_data_len > sizeof(buf)) ? sizeof(buf) : h->h_data_len;
	if ( !fd_pread(webroot_fd, h->h_data_off, buf, &sz, &eof) || eof ) {
		nbio_del(t, &h->h_nbio);
		return;
	}

	dprintf("Read %u bytes in to buffer\n", sz);

	if ( sz == h->h_data_len )
		flags |= MSG_MORE;

	ret = send(h->h_nbio.fd, buf, sz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &h->h_nbio);
		return;
	}else if ( ret <= 0 ) {
		nbio_del(t, &h->h_nbio);
		return;
	}

	h->h_data_off += (size_t)ret;
	h->h_data_len -= (size_t)ret;
	dprintf("Transmitted %u/%u bytes\n", ret, sz);

	if ( h->h_data_len == 0 ) {
		nbio_set_wait(t, &h->h_nbio, NBIO_READ);
		h->h_state = HTTP_CONN_REQUEST;
		dprintf("DONE\n");
	}
}

static void http_write(struct iothread *t, struct nbio *n)
{
	struct http_conn *h;
	h = (struct http_conn *)n;

	switch(h->h_state) {
	case HTTP_CONN_HEADER:
		http_write_hdr(t, h);
		break;
	case HTTP_CONN_DATA:
		http_write_data_sync(t, h);
		break;
	default:
		dprintf("uh %u\n", h->h_state);
		abort();
	}
}

static void response_400(struct iothread *t, struct http_conn *h)
{
	memcpy(h->h_res, resp400, strlen(resp400));
	h->h_res_end = h->h_res + strlen(resp400);
	h->h_data_len = 0;
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

static void handle_get(struct iothread *t, struct http_conn *h,
			struct http_request *r)
{
	const struct webroot_name *n;
	unsigned int code, mime_type;
	struct ro_vec search_uri = r->uri;
	int len;

	if ( search_uri.v_len > 1 &&
		search_uri.v_ptr[search_uri.v_len - 1] == '/' )
		search_uri.v_len--;

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

	len = snprintf((char *)h->h_res, HTTP_MAX_RESP,
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
	h->h_res_end = h->h_res + len;
}

static void handle_request(struct iothread *t, struct http_conn *h)
{
	struct http_request r;
	size_t hlen, blen;

	memset(&r, 0, sizeof(r));
	blen = h->h_req_ptr - h->h_req;

	hlen = http_req(&r, h->h_req, blen);
	dprintf("%u/%u bytes were request\n", hlen, blen);
	h->h_req_ptr = h->h_req + hlen;
	if ( h->h_req_ptr != h->h_req + blen ) {
		dprintf("memmove %u: %.*s",
			blen - (h->h_req_ptr - h->h_req),
			blen - (h->h_req_ptr - h->h_req),
			h->h_req_ptr);
		memmove(h->h_req, h->h_req_ptr,
			blen - (h->h_req_ptr - h->h_req));
	}
	h->h_req_ptr = h->h_req;

	nbio_set_wait(t, &h->h_nbio, NBIO_WRITE);
	h->h_state = HTTP_CONN_HEADER;
	h->h_res_ptr = h->h_res;

	if ( vstrcmp_fast(&r.method, "GET") ) {
		response_400(t, h);
	}else{
		handle_get(t, h, &r);
	}
}

static void http_read(struct iothread *t, struct nbio *n)
{
	struct http_conn *h;
	size_t buflen;
	ssize_t ret;
	const uint8_t *term;

	h = (struct http_conn *)n;

	assert(h->h_state == HTTP_CONN_REQUEST);

	buflen = h->h_req_end - h->h_req_ptr;

	ret = recv(h->h_nbio.fd, h->h_req_ptr, buflen, 0);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, n);
		return;
	}else if ( ret <= 0 ) {
		nbio_del(t, n);
		return;
	}

	dprintf("Received %u bytes: %.*s\n",
		ret, ret, h->h_req_ptr);
	h->h_req_ptr += (size_t)ret;

	dprintf("Searching for terminating %u bytes\n",
		sizeof(http_req_terminator));
	term = bm_find(http_req_terminator, sizeof(http_req_terminator),
		h->h_req, h->h_req_ptr - h->h_req,
		rt_skip);
	if ( NULL == term ) {
		if ( unlikely(h->h_req_ptr == h->h_req_end) ) {
			dprintf("Nah, buffer full, resetting\n");
			h->h_req_ptr = h->h_req;
		}else{
			dprintf("Nah, waiting for more data\n");
			return;
		}
	}

	handle_request(t, h);
}

static void http_dtor(struct iothread *t, struct nbio *n)
{
	struct http_conn *h;
	h = (struct http_conn *)n;
	dprintf("Connection killed\n");
	hgang_return(buffs, h->h_req);
	hgang_return(hbufs, h->h_res);
	hgang_return(conns, h);
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
		goto err;
	}

	h->h_req = hgang_alloc(buffs);
	if ( NULL == h->h_req ) {
		fprintf(stderr, "hgang_alloc: %s\n", os_err());
		goto err_free_conn;
	}

	h->h_req_ptr = h->h_req;
	h->h_req_end = h->h_req + HTTP_MAX_REQ;

	h->h_res = hgang_alloc(hbufs);
	if ( NULL == h->h_res ) {
		fprintf(stderr, "hgang_alloc: %s\n", os_err());
		goto err_free_buf;
	}

	h->h_res_ptr = h->h_res;
	h->h_res_end = h->h_res + HTTP_MAX_REQ;

	h->h_nbio.fd = s;
	h->h_nbio.ops = &http_ops;
	nbio_add(t, &h->h_nbio, NBIO_READ);

	h->h_state = HTTP_CONN_REQUEST;
	return;
err_free_buf:
	hgang_return(buffs, h->h_req);
err_free_conn:
	hgang_return(conns, h);
err:
	fd_close(s);
}

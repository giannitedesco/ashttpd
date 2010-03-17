#include <ashttpd.h>

static hgang_t conns;
static hgang_t buffs;
static int rt_skip[BM_SKIP_LEN];
static const uint8_t http_req_terminator[4] = "\r\n\r\n";

static void __attribute__((constructor)) http_conn_ctor(void)
{
	conns = hgang_new(sizeof(struct http_conn), 16);
	buffs = hgang_new(HTTP_MAX_REQ, 8);
	bm_skip(http_req_terminator, sizeof(http_req_terminator), rt_skip);
}

static void http_write(struct iothread *t, struct nbio *n)
{
	struct http_conn *h;
	ssize_t ret;

	h = (struct http_conn *)n;

	ret = send(h->h_nbio.fd, "HELLO\r\n", 7, MSG_NOSIGNAL);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, n);
		return;
	}else if ( ret <= 0 ) {
		nbio_del(t, n);
		return;
	}

	nbio_set_wait(t, n, NBIO_READ);
}

static void handle_request(struct iothread *t, struct http_conn *h)
{
	struct http_request r;
	size_t hlen, blen;

	memset(&r, 0, sizeof(r));
	blen = h->h_buf_ptr - h->h_buf;

	hlen = http_req(&r, h->h_buf, blen);
	printf("%u/%u bytes were request\n", hlen, blen);
	h->h_buf_ptr = h->h_buf + hlen;

	nbio_set_wait(t, &h->h_nbio, NBIO_WRITE);
}

static void http_read(struct iothread *t, struct nbio *n)
{
	struct http_conn *h;
	size_t buflen;
	ssize_t ret;
	const uint8_t *term;

	h = (struct http_conn *)n;

	buflen = h->h_buf_end - h->h_buf_ptr;

	ret = recv(h->h_nbio.fd, h->h_buf_ptr, buflen, 0);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, n);
		return;
	}else if ( ret <= 0 ) {
		nbio_del(t, n);
		return;
	}

	printf("Received %u bytes: %.*s\n",
		ret, ret, h->h_buf_ptr);
	h->h_buf_ptr += (size_t)ret;

	printf("Searching for terminating %u bytes\n",
		sizeof(http_req_terminator));
	term = bm_find(http_req_terminator, sizeof(http_req_terminator),
		h->h_buf, h->h_buf_ptr - h->h_buf,
		rt_skip);
	if ( NULL == term ) {
		if ( unlikely(h->h_buf_ptr == h->h_buf_end) ) {
			printf("Nah, buffer full, resetting\n");
			h->h_buf_ptr = h->h_buf;
		}else{
			printf("Nah, waiting for more data\n");
			return;
		}
	}

	handle_request(t, h);
}

static void http_dtor(struct iothread *t, struct nbio *n)
{
	struct http_conn *h;
	h = (struct http_conn *)n;
	printf("Connection killed\n");
	hgang_return(buffs, h->h_buf);
	hgang_return(conns, h);
}

static const struct nbio_ops http_ops = {
	.read = http_read,
	.write = http_write,
	.dtor = http_dtor,
};

void http_conn(struct iothread *t, int s, void *priv)
{
	struct http_conn *conn;

	conn = hgang_alloc0(conns);
	if ( NULL == conn ) {
		fprintf(stderr, "hgang_alloc: %s\n", os_err());
		goto err;
	}

	conn->h_buf = hgang_alloc(buffs);
	if ( NULL == conn->h_buf ) {
		fprintf(stderr, "hgang_alloc: %s\n", os_err());
		goto err_free_conn;
	}

	conn->h_buf_ptr = conn->h_buf;
	conn->h_buf_end = conn->h_buf + HTTP_MAX_REQ;

	conn->h_nbio.fd = s;
	conn->h_nbio.ops = &http_ops;
	nbio_add(t, &conn->h_nbio, NBIO_READ);
	return;
err_free_conn:
	hgang_return(conns, conn);
err:
	os_fd_close(s);
}

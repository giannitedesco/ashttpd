#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <ashttpd.h>
#include <nbio-listener.h>
#include <ashttpd-conn.h>
#include <ashttpd-buf.h>
#include <ashttpd-fio.h>
#include <http-parse.h>
#include <http-req.h>
#include <hgang.h>

/* State machine for incremental HTTP request parse */
#define RSTATE_INITIAL		0
#define RSTATE_CR		1
#define RSTATE_LF		2
#define RSTATE_CRLF		3
#define RSTATE_LFCR		4
#define RSTATE_CRLFCR		5
#define RSTATE_LFLF		6
#define RSTATE_CRLFLF		7
#define RSTATE_LFCRLF		8
#define RSTATE_CRLFCRLF		9

#define RSTATE_NR_NONTERMINAL	RSTATE_LFLF
#define RSTATE_TERMINAL(x)	((x) >= RSTATE_LFLF)

#define HTTP_CONN_REQUEST	0
/* FIXME: gobble any POST data */
#define HTTP_CONN_HEADER	1
#define HTTP_CONN_DATA		2
#define HTTP_CONN_DEAD		3
struct _http_conn {
	struct nbio	h_nbio;
	unsigned char	h_state;
	unsigned char	h_rstate;
	unsigned short	h_io_state;

	const uint8_t	*h_rptr;
	struct http_buf	*h_req;
	struct http_buf	*h_res;

	void 		*h_io_priv;
	off_t		h_data_off;
	size_t		h_data_len;

	unsigned int	h_conn_close;
};

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

static struct http_fio *fio_current;
static unsigned int concurrency;
static hgang_t conns;
static int webroot_fd = -1;

static const char * const resp400 =
	"HTTP/1.1 400 Bad Request\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 77\r\n"
	"\r\n"
	"<html><head><title>Fuck Off</title></head>"
	"<body><h1>Bad Request</body></html>";

#define _io_init	(*fio_current->init)
#define _io_prep	(*fio_current->prep)
#define _io_write	(*fio_current->write)
#define _io_webroot_fd	(*fio_current->webroot_fd)
#define _io_abort	(*fio_current->abort)
#define _io_fini	(*fio_current->fini)

static LIST_HEAD(oomq);

static void wake_listeners(struct iothread *t)
{
	struct nbio *io, *tmp;

	dprintf("http_listener: waking all on wait queue\n");
	list_for_each_entry_safe(io, tmp, &oomq, list)
		listener_wake(t, io);
}

static void http_oom(struct iothread *t, struct nbio *listener)
{
	dprintf("http_listener: no more resources\n");
	nbio_to_waitq(t, listener, &oomq);
}

static void http_kill(struct iothread *t, struct _http_conn *h)
{
	/* ma have got here already due to data complete in a non
	 * nbio context (eg. AIO) on a non-persistent HTTP connection
	 */
	if ( h->h_nbio.fd == -1 )
		return;

	dprintf("Connection killed\n");
	fd_close(h->h_nbio.fd);
	h->h_nbio.fd = -1;
	assert(concurrency);
	--concurrency;
	if ( !list_empty(&oomq) )
		wake_listeners(t);

	switch(h->h_state) {
	case HTTP_CONN_REQUEST:
		buf_free_req(h->h_req);
		assert(h->h_res == NULL);
		break;
	case HTTP_CONN_HEADER:
		assert(h->h_req == NULL);
		buf_free_res(h->h_res);
		/* fall through */
	case HTTP_CONN_DATA:
		_io_abort(h);
		break;
	default:
		abort();
	}

	h->h_state = HTTP_CONN_DEAD;
	nbio_del(t, &h->h_nbio);
}

size_t http_conn_data(http_conn_t h, int *fd, off_t *off)
{
	/* AIO read may run in parallel with transmission of header */
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);

	if ( fd )
		*fd = webroot_fd;
	if ( off )
		*off = h->h_data_off;
	return h->h_data_len;
}

size_t http_conn_data_read(http_conn_t h, size_t len)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	assert(len <= h->h_data_len);
	h->h_data_len -= len;
	h->h_data_off += len;
	return h->h_data_len;
}

void http_conn_data_complete(struct iothread *t, http_conn_t h)
{
	assert(h->h_state == HTTP_CONN_DATA);
	assert(0 == h->h_data_len);
	h->h_state = HTTP_CONN_REQUEST;
	nbio_set_wait(t, &h->h_nbio, NBIO_READ);
	if ( h->h_conn_close )
		http_kill(t, h);
}

void http_conn_abort(struct iothread *t, http_conn_t h)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	http_kill(t, h);
}

void *http_conn_get_priv(http_conn_t h, unsigned short *state)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	if ( state )
		*state = h->h_io_state;
	return h->h_io_priv;
}

void http_conn_set_priv(http_conn_t h, void *priv, unsigned short state)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	h->h_io_state = state;
	h->h_io_priv = priv;
}

int http_conn_socket(http_conn_t h)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	return h->h_nbio.fd;
}

void http_conn_inactive(struct iothread *t, http_conn_t h)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	nbio_inactive(t, &h->h_nbio);
}

void http_conn_wait_on(struct iothread *t, http_conn_t h, unsigned short w)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	nbio_wait_on(t, &h->h_nbio, w);
}

void http_conn_to_waitq(struct iothread *t, http_conn_t h, struct list_head *wq)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	nbio_to_waitq(t, &h->h_nbio, wq);
}

void http_conn_wake_one(struct iothread *t, http_conn_t h)
{
	assert(h->h_state == HTTP_CONN_DATA || h->h_state == HTTP_CONN_HEADER);
	nbio_wake(t, &h->h_nbio, NBIO_WRITE);
}

void http_conn_wake(struct iothread *t, struct list_head *waitq,
			void(*wake_func)(struct iothread *, http_conn_t))
{
	struct _http_conn *h, *tmp;
	list_for_each_entry_safe(h, tmp, waitq, h_nbio.list) {
		assert(h->h_state == HTTP_CONN_DATA ||
			h->h_state == HTTP_CONN_HEADER);
		/* Do the nbio wake first incase caller decides to
		 * delete conn */
		nbio_wake(t, &h->h_nbio, NBIO_WRITE);
		(*wake_func)(t, h);
	}
}

static int http_write_hdr(struct iothread *t, struct _http_conn *h)
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
			if ( h->h_conn_close )
				return 0;
			nbio_set_wait(t, &h->h_nbio, NBIO_READ);
			h->h_state = HTTP_CONN_REQUEST;
		}
	}

	return 1;
}

static void http_write(struct iothread *t, struct nbio *n)
{
	struct _http_conn *h;
	int ret = 0;

	h = (struct _http_conn *)n;

	switch(h->h_state) {
	case HTTP_CONN_HEADER:
		ret = http_write_hdr(t, h);
		break;
	case HTTP_CONN_DATA:
		ret = _io_write(t, h);
		break;
	default:
		dprintf("uh %u\n", h->h_state);
		abort();
	}

	if ( !ret )
		http_kill(t, h);
}

/* FIXME: persistent connection handling */
static int response_400(struct iothread *t, struct _http_conn *h)
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

static int handle_get(struct iothread *t, struct _http_conn *h,
			struct http_request *r, int head)
{
	const struct webroot_name *n;
	unsigned int code, mime_type;
	struct ro_vec search_uri = r->uri;
	char *conn;
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

	if ( h->h_data_len && !head ) {
		if ( !_io_prep(t, h) ) {
			/* FIXME: insufficient resources http code */
			return 0;
		}
	}

	if ( h->h_conn_close ) {
		conn = "Close";
	}else{
		conn = "Keep-Alive";
	}

	ptr = buf_write(h->h_res, &sz);
	len = snprintf((char *)ptr, sz,
			"HTTP/1.1 %u %s\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %u\r\n"
			"Connection: %s\r\n"
			"Server: ashttpd, experimental l33tness\r\n"
			"\r\n",
			code,
			(code == 200) ? "OK": "Not Found",
			webroot_mime_type(mime_type),
			h->h_data_len,
			conn);
	if ( len < 0 )
		len = 0;
	if ( (size_t)len == sz ) {
		printf("Truncated header...\n");
		return 0;
	}

	buf_done_write(h->h_res, len);
	if ( head )
		h->h_data_len = 0;
	return 1;
}

static void handle_request(struct iothread *t, struct _http_conn *h)
{
	struct http_request r;
	size_t hlen, sz;
	const uint8_t *ptr;
	int ret;

	assert(h->h_state == HTTP_CONN_REQUEST);

	memset(&r, 0, sizeof(r));
	ptr = buf_read(h->h_req, &sz);
	hlen = http_req(&r, ptr, sz);
	if ( 0 == hlen ) {
		/* FIXME: error 400 */
		http_kill(t, h);
		return;
	}

	h->h_res = buf_alloc_res();
	if ( NULL == h->h_res ) {
		printf("OOM on res...\n");
		http_kill(t, h);
		return;
	}

	nbio_set_wait(t, &h->h_nbio, NBIO_WRITE);
	h->h_state = HTTP_CONN_HEADER;

	if ( r.proto_vers >= HTTP_VER_1_1 ) {
		static const struct ro_vec close_token = {
			.v_ptr = (uint8_t *)"Close",
			.v_len = 5,
		};
		if ( !vcasecmp_fast(&r.connection, &close_token) ) {
			h->h_conn_close = 1;
		}else{
			h->h_conn_close = 0;
		}
	}else{
		/* For HTTP/1.0 close connection regardless of connection
		 * header token. Due to buggy proxies which may pass on
		 * connection: Keep-Alive token without understanding it
		 * resulting in a hung connection
		 */
		h->h_conn_close = 1;
	}

	if ( !vstrcmp_fast(&r.method, "GET") ) {
		/* FIXME: bad request */
		ret = handle_get(t, h, &r, 0);
	}else if ( !vstrcmp_fast(&r.method, "HEAD") ) {
		ret = handle_get(t, h, &r, 1);
	}else{
		ret = response_400(t, h);
	}

	if ( !ret ) {
		http_kill(t, h);
		return;
	}

	dprintf("%u/%u bytes were request\n", hlen, sz);
	buf_done_read(h->h_req, hlen);

	buf_read(h->h_req, &sz);
	if ( sz ) {
		buf_reset(h->h_req);
		h->h_rptr = h->h_req->b_base;
		h->h_rstate = RSTATE_INITIAL;
		return;
	}else{
		buf_free_req(h->h_req);
		h->h_req = NULL;
	}
}

static int http_parse_incremental(struct _http_conn *h)
{
	static const uint8_t cr_map[RSTATE_NR_NONTERMINAL] = {
			[RSTATE_INITIAL] = RSTATE_CR,
			[RSTATE_CR] = RSTATE_CR,
			[RSTATE_LF] = RSTATE_LFCR,
			[RSTATE_CRLF] = RSTATE_CRLFCR,
			[RSTATE_LFCR] = RSTATE_CR,
			[RSTATE_CRLFCR] = RSTATE_CR};
	static const uint8_t lf_map[RSTATE_NR_NONTERMINAL] = {
			[RSTATE_INITIAL] = RSTATE_LF,
			[RSTATE_CR] = RSTATE_CRLF,
			[RSTATE_LF] = RSTATE_LFLF,
			[RSTATE_CRLF] = RSTATE_CRLFLF,
			[RSTATE_LFCR] = RSTATE_LFCRLF,
			[RSTATE_CRLFCR] = RSTATE_CRLFCRLF};

	for(; h->h_rptr < h->h_req->b_write; h->h_rptr++) {
		switch(h->h_rptr[0]) {
		case '\r':
			assert(h->h_rstate < RSTATE_NR_NONTERMINAL);
			h->h_rstate = cr_map[h->h_rstate];
			break;
		case '\n':
			assert(h->h_rstate < RSTATE_NR_NONTERMINAL);
			h->h_rstate = lf_map[h->h_rstate];
			break;
		default:
			h->h_rstate = RSTATE_INITIAL;
			continue;
		}
		if ( RSTATE_TERMINAL(h->h_rstate) )
			return 1;
	}
	return 0;
}

static void http_read(struct iothread *t, struct nbio *nbio)
{
	struct _http_conn *h;
	uint8_t *ptr;
	ssize_t ret;
	size_t sz;

	h = (struct _http_conn *)nbio;

	assert(h->h_state == HTTP_CONN_REQUEST);

	if ( NULL == h->h_req ) {
		h->h_req = buf_alloc_req();
		if ( NULL == h->h_req ) {
			printf("OOM on res after header...\n");
			http_kill(t, h);
			return;
		}
		h->h_rptr = h->h_req->b_base;
		h->h_rstate = RSTATE_INITIAL;
	}

	ptr = buf_write(h->h_req, &sz);
	if ( 0 == sz ) {
		printf("OOM on req...\n");
		http_kill(t, h);
		return;
	}

	ret = recv(h->h_nbio.fd, ptr, sz, 0);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, nbio);
		return;
	}else if ( ret <= 0 ) {
		http_kill(t, h);
		return;
	}

	dprintf("Received %u bytes: %.*s\n",
		ret, ret, ptr);
	buf_done_write(h->h_req, ret);

	if ( !http_parse_incremental(h) ) {
		dprintf("no request yet, waiting for more data\n");
		return;
	}

	handle_request(t, h);
}

static void http_dtor(struct iothread *t, struct nbio *n)
{
	struct _http_conn *h = (struct _http_conn *)n;
	assert(h->h_state = HTTP_CONN_DEAD);
}

static const struct nbio_ops http_ops = {
	.read = http_read,
	.write = http_write,
	.dtor = http_dtor,
};

static void http_conn(struct iothread *t, int s, void *priv)
{
	struct _http_conn *h;

	h = hgang_alloc0(conns);
	if ( NULL == h ) {
		fprintf(stderr, "hgang_alloc: %s\n", os_err());
		fd_close(s);
		return;
	}

	h->h_state = HTTP_CONN_REQUEST;

	h->h_nbio.fd = s;
	h->h_nbio.ops = &http_ops;
	nbio_add(t, &h->h_nbio, NBIO_READ);
	concurrency++;
	if ( (concurrency % 1000) == 0 )
		printf("concurrency %u\n", concurrency);
	return;
}

static struct http_fio *io_model(const char *name)
{
	unsigned int i;
	static const struct {
		char * const n;
		struct http_fio *fio;
	}ionames[]={
		/* traditional pread based */
		{"sync", &fio_sync},

		/* sendfile: regular synchronous version */
		{"sendfile", &fio_sendfile},

		/* Kernel AIO on regular file, not currently
		 * supported in linux 2.6 so falls back to synchronous
		 */
		{"aio", &fio_async},
		{"async", &fio_async},

		/* Kernel AIO on O_DIRECT file descriptor, re-implementing
		 * page cache in userspace fucking alice in wonderland
		 */
		{"dasync", &fio_dasync},
		{"direct", &fio_dasync},
		{"dio", &fio_dasync},

		/* Kernel based AIO / sendfile utilizing kernel pipe
		 * buffers and splicing... experimental
		 */
		{"aio-sendfile", &fio_async_sendfile},
		{"async-sendfile", &fio_async_sendfile},
	};

	if ( NULL == name )
		return &fio_sync;

	for(i = 0; i < sizeof(ionames)/sizeof(*ionames); i++) {
		if ( !strcmp(name, ionames[i].n) )
			return ionames[i].fio;
	}

	printf("%s not found!\n", name);
	return &fio_sync;
}

int main(int argc, char **argv)
{
	const char * const webroot_fn = "webroot.objdb";
	struct iothread iothread;

	fio_current = io_model((argc > 1) ? argv[1] : NULL);
	printf("data: %s model\n", fio_current->label);

	webroot_fd = _io_webroot_fd(webroot_fn);
	if ( webroot_fd < 0 ) {
		fprintf(stderr, "webroot: %s: %s\n", webroot_fn, os_err());
		return EXIT_FAILURE;
	}

	conns = hgang_new(sizeof(struct _http_conn), 0);
	if ( NULL == conns ) {
		fprintf(stderr, "conns: %s\n", os_err());
		return EXIT_FAILURE;
	}

	if ( !nbio_init(&iothread, NULL) )
		return EXIT_FAILURE;

	listener_inet(&iothread, SOCK_STREAM, IPPROTO_TCP,
			0, 80, http_conn, NULL, http_oom);
	listener_inet(&iothread, SOCK_STREAM, IPPROTO_TCP,
			0, 1234, http_conn, NULL, http_oom);

	if ( !_io_init(&iothread, webroot_fd) ) {
		return EXIT_FAILURE;
	}

	do {
		nbio_pump(&iothread, -1);
	}while ( !list_empty(&iothread.active) );

	nbio_fini(&iothread);

	return EXIT_SUCCESS;
}

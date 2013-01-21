#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <ashttpd.h>
#include <ashttpd-conn.h>
#include <ashttpd-buf.h>
#include <ashttpd-fio.h>
#include <http-parse.h>
#include <http-req.h>
#include <normalize.h>
#include <hgang.h>

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

	struct http_listener *h_owner;

	const uint8_t	*h_rptr;
	struct http_buf	*h_req;
	struct http_buf	*h_res;

	void		*h_io_priv;
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

static const char * const resp400 =
	"HTTP/1.1 400 Bad Request\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 82\r\n"
	"\r\n"
	"<html><head><title>Fuck Off</title></head>"
	"<body><h1>Bad Request</h1></body></html>";
static const char * const resp301 =
	"HTTP/1.1 301 Moved Permanently\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 87\r\n"
	"Location: http://%.*s%.*s\r\n"
	"\r\n"
	"<html><head><title>Object Moved</title></head>"
	"<body><h1>Object Moved</h1></body></html>";
static const char * const resp403 =
	"HTTP/1.1 403 Forbidden\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 84\r\n"
	"\r\n"
	"<html><head><title>Fobidden</title></head>"
	"<body><h1>Access denied</h1></body></html>";
static const char * const resp404 =
	"HTTP/1.1 404 Object Not Found\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 83\r\n"
	"\r\n"
	"<html><head><title>Fuck Off</title></head>"
	"<body><h1>y u no find?</h1></body></html>";


#define _io_init	(*fio_current->init)
#define _io_prep	(*fio_current->prep)
#define _io_write	(*fio_current->write)
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
	/* may have got here already due to data complete in a non
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

	buf_free_req(h->h_req);

	switch(h->h_state) {
	case HTTP_CONN_REQUEST:
		assert(h->h_res == NULL);
		break;
	case HTTP_CONN_HEADER:
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
		*fd = webroot_get_fd(h->h_owner->l_webroot);
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
	nbio_inactive(t, &h->h_nbio, NBIO_WRITE);
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
		nbio_inactive(t, &h->h_nbio, NBIO_WRITE);
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
			dprintf("Header done, %zu bytes of data\n",
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
static int response_403(struct iothread *t, struct _http_conn *h)
{
	uint8_t *ptr;
	size_t sz;

	ptr = buf_write(h->h_res, &sz);
	assert(NULL != ptr && sz >= strlen(resp403));

	memcpy(ptr, resp403, strlen(resp403));
	buf_done_write(h->h_res, strlen(resp403));
	h->h_data_len = 0;
	return 1;
}

/* FIXME: persistent connection handling */
static int response_404(struct iothread *t, struct _http_conn *h)
{
	uint8_t *ptr;
	size_t sz;

	ptr = buf_write(h->h_res, &sz);
	assert(NULL != ptr && sz >= strlen(resp404));

	memcpy(ptr, resp404, strlen(resp404));
	buf_done_write(h->h_res, strlen(resp404));
	h->h_data_len = 0;
	return 1;
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

/* FIXME: persistent connection handling */
static int response_301(struct iothread *t, struct _http_conn *h,
			const struct ro_vec *host_hdr,
			const struct ro_vec *loc)
{
	struct ro_vec host;
	uint8_t *ptr;
	size_t sz;
	int n;

	/* FIXME: if host header not present, then figure it out */
	host.v_ptr = host_hdr->v_ptr;
	host.v_len = host_hdr->v_len;

	ptr = buf_write(h->h_res, &sz);
	assert(NULL != ptr && sz >= strlen(resp301));

	memcpy(ptr, resp301, strlen(resp301));
	n = snprintf((char *)ptr, sz, resp301,
			(int)host.v_len, host.v_ptr,
			(int)loc->v_len, loc->v_ptr);
	buf_done_write(h->h_res, n);
	h->h_data_len = 0;
	return 1;
}

static int handle_get(struct iothread *t, struct _http_conn *h,
			struct http_request *r, int head)
{
	struct webroot_name n;
	struct ro_vec search_uri = r->uri;
	struct tm tm;
	char etag[41];
	char mtime[128];
	time_t mt;
	struct nads nads;
	uint8_t *ptr;
	size_t sz;
	int len, hit = 0;

	if ( search_uri.v_len > 1 &&
		search_uri.v_ptr[search_uri.v_len - 1] == '/' )
		search_uri.v_len--;

	nads.buf = (char *)r->uri.v_ptr;
	nads.buf_len = r->uri.v_len;
	nads_normalize(&nads);

	dprintf("GET %.*s -> '%s' '%s'\n",
		(int)r->uri.v_len, r->uri.v_ptr,
		nads.uri, nads.query);

	search_uri.v_ptr = (uint8_t *)nads.uri;
	search_uri.v_len = strlen(nads.uri);

	if ( !webroot_find(h->h_owner->l_webroot, &search_uri, &n) ) {
#if 0
		h->h_data_off = obj404_f_ofs;
		h->h_data_len = obj404_f_len;
		mime_type = obj404_mime_type;
#else
		return response_404(t, h);
#endif
	}else{
		switch(n.code) {
		case HTTP_MOVED_PERMANENTLY:
			return response_301(t, h, &r->host, &n.u.moved);
		case HTTP_FORBIDDEN:
			return response_403(t, h);
		case HTTP_FOUND:
			h->h_data_off = n.u.data.f_ofs;
			h->h_data_len = n.u.data.f_len;
			break;
		}
	}

	snprintf(etag, sizeof(etag), "%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x"
			"%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x",
			n.u.data.f_etag[0], n.u.data.f_etag[1],
			n.u.data.f_etag[2], n.u.data.f_etag[3],
			n.u.data.f_etag[4], n.u.data.f_etag[5],
			n.u.data.f_etag[6], n.u.data.f_etag[7],
			n.u.data.f_etag[8], n.u.data.f_etag[9],
			n.u.data.f_etag[10], n.u.data.f_etag[11],
			n.u.data.f_etag[12], n.u.data.f_etag[13],
			n.u.data.f_etag[14], n.u.data.f_etag[15],
			n.u.data.f_etag[16], n.u.data.f_etag[17],
			n.u.data.f_etag[18], n.u.data.f_etag[19]);

	if ( r->etag.v_len == 40 ) {
		dprintf("conditional query: %.*s vs %s: ",
			(int)r->etag.v_len, r->etag.v_ptr, etag);
		if ( !vstrcmp(&r->etag, etag) ) {
			hit = 1;
			dprintf("hit\n");
		}else{
			dprintf("miss\n");
		}
	}

	if ( hit ) {
		head = 1;
		n.code = HTTP_NOT_MODIFIED;
	}else{
	}

	if ( h->h_data_len && !head ) {
		if ( !_io_prep(t, h) ) {
			/* FIXME: insufficient resources http code */
			return 0;
		}
	}

	mt = n.u.data.f_mtime;
	gmtime_r(&mt, &tm);
	strftime(mtime, sizeof(mtime), "%a, %d %b %Y %H:%M:%S GMT", &tm);

	ptr = buf_write(h->h_res, &sz);
	len = snprintf((char *)ptr, sz,
			"HTTP/1.1 %u %s\r\n"
			"Content-Type: %.*s\r\n"
			"Content-Length: %zu\r\n"
			"Connection: %s\r\n"
			"ETag: %s\r\n"
			"Last-Modified: %s\r\n"
			"Server: ashttpd, experimental l33tness\r\n"
			"\r\n",
			n.code,
			(n.code == HTTP_FOUND) ? "OK": "Not Found",
			(int)n.mime_type.v_len,
			n.mime_type.v_ptr,
			h->h_data_len,
			(h->h_conn_close) ? "Close" : "Keep-Alive",
			etag,
			mtime);
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

	ptr = buf_read(h->h_req, &sz);
	memset(&r, 0, sizeof(r));
	hlen = http_req(&r, ptr, sz);
	//printf("%.*s\n", (int)sz, ptr);
	if ( 0 == hlen ) {
		/* FIXME: error 400 */
		http_kill(t, h);
		return;
	}

	if ( r.content_len ) {
		printf("Argh, Content-Length set on request\n");
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

	h->h_conn_close = r.conn_close;

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

	dprintf("%zu/%zu bytes were request\n", hlen, sz);
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
		nbio_inactive(t, nbio, NBIO_READ);
		return;
	}else if ( ret <= 0 ) {
		http_kill(t, h);
		return;
	}

	dprintf("Received %zu bytes: %.*s\n",
		ret, (int)ret, ptr);
	buf_done_write(h->h_req, ret);

	if ( !http_parse_incremental(&h->h_rstate,
					&h->h_rptr, h->h_req->b_write) ) {
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
	struct http_listener *hl = priv;
	struct _http_conn *h;

	h = hgang_alloc0(conns);
	if ( NULL == h ) {
		fprintf(stderr, "hgang_alloc: %s\n", os_err());
		fd_close(s);
		return;
	}

	h->h_owner = hl;
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
#if 0
		{"dasync", &fio_dasync},
		{"direct", &fio_dasync},
		{"dio", &fio_dasync},
#endif

		/* Kernel based AIO / sendfile utilizing kernel pipe
		 * buffers and splicing... experimental
		 */
#if HAVE_AIO_SENDFILE
		{"aio-sendfile", &fio_async_sendfile},
		{"async-sendfile", &fio_async_sendfile},
#endif
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

static struct http_listener *http_listen(struct iothread *t,
					uint32_t addr, uint16_t port, webroot_t root)
{
	struct http_listener *hl;

	hl = calloc(1, sizeof(*hl));
	if ( NULL == hl )
		goto out;

	hl->l_listen = listener_inet(t, SOCK_STREAM, IPPROTO_TCP,
					addr, port, http_conn, hl, http_oom);
	if ( NULL == hl->l_listen )
		goto out_free;

	hl->l_webroot = root;

	goto out; /* success */

out_free:
	free(hl);
	hl = NULL;
out:
	return hl;
}

int main(int argc, char **argv)
{
	static webroot_t webroot;
	const char * webroot_fn;
	struct iothread iothread;

	webroot_fn = (argc > 1) ? argv[1] : "webroot.objdb";
	fio_current = io_model((argc > 2) ? argv[2] : NULL);

	printf("data: %s model\n", fio_current->label);
	printf("webroot: %s\n", webroot_fn);

	webroot = webroot_open(webroot_fn);
	if ( NULL == webroot ) {
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

	http_listen(&iothread, 0, 80, webroot);
	http_listen(&iothread, 0, 1234, webroot);

	if ( !_io_init(&iothread) ) {
		return EXIT_FAILURE;
	}

	do {
		nbio_pump(&iothread, -1);
	}while ( !list_empty(&iothread.active) );

	nbio_fini(&iothread);
	webroot_close(webroot);

	return EXIT_SUCCESS;
}

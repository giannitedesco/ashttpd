#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
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
#include <nbio-inotify.h>
#include <normalize.h>
#include <hgang.h>
#include <signal.h>

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
	webroot_t	h_webroot;

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

struct http_fio *fio_current;
static unsigned int concurrency;
static hgang_t conns;

static const char * const resp400 =
	"HTTP/1.1 400 Bad Request\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 82\r\n"
	"Connection: Close\r\n"
	"\r\n"
	"<html><head><title>Fuck Off</title></head>"
	"<body><h1>Bad Request</h1></body></html>";
static const char * const resp500 =
	"HTTP/1.1 500 Internal Server Error\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 79\r\n"
	"Connection: Close\r\n"
	"\r\n"
	"<html><head><title>Fail Whale</title></head>"
	"<body><h1>Sorry!</h1></body></html>";
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
static const char * const resp501 =
	"HTTP/1.1 501 Method Not Implemented\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 87\r\n"
	"\r\n"
	"<html><head><title>Fuck...</title></head>"
	"<body><h1>y u no implement?</h1></body></html>";


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

void http_oom(struct iothread *t, struct nbio *listener)
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
	close(h->h_nbio.fd);
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
		*fd = webroot_get_fd(h->h_webroot);
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
	webroot_unref(h->h_webroot);
	h->h_webroot = NULL;
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
static int response_501(struct iothread *t, struct _http_conn *h)
{
	uint8_t *ptr;
	size_t sz;

	ptr = buf_write(h->h_res, &sz);
	assert(NULL != ptr && sz >= strlen(resp501));

	memcpy(ptr, resp501, strlen(resp501));
	buf_done_write(h->h_res, strlen(resp501));
	h->h_data_len = 0;
	h->h_conn_close = 1;
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

static int response_400(struct iothread *t, struct _http_conn *h)
{
	uint8_t *ptr;
	size_t sz;

	ptr = buf_write(h->h_res, &sz);
	assert(NULL != ptr && sz >= strlen(resp400));

	memcpy(ptr, resp400, strlen(resp400));
	buf_done_write(h->h_res, strlen(resp400));
	h->h_data_len = 0;
	h->h_conn_close = 1;
	return 1;
}

static int response_500(struct iothread *t, struct _http_conn *h)
{
	uint8_t *ptr;
	size_t sz;

	ptr = buf_write(h->h_res, &sz);
	assert(NULL != ptr && sz >= strlen(resp500));

	memcpy(ptr, resp500, strlen(resp500));
	buf_done_write(h->h_res, strlen(resp500));
	h->h_data_len = 0;
	h->h_conn_close = 1;
	return 1;
}

static void print_etag(char buf[ETAG_SZ * 2 + 1], const uint8_t etag[ETAG_SZ])
{
	unsigned int i;
	char *ptr;

	for(i = 0, ptr = buf; i < ETAG_SZ; i++, ptr += 2) {
		static const char hex[] = "01234567890abcdef";
		uint8_t hi = etag[i] >> 4;
		uint8_t lo = etag[i] & 0xf;
		ptr[0] = hex[hi];
		ptr[1] = hex[lo];
	}

	*ptr = '\0';
}

#define HTTP_TIME_BUF 44
static int print_time(char buf[HTTP_TIME_BUF + 1], struct tm *tm)
{
	static const char * const dayofweek[] = {
		"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
	};
	static const char * const monthofyear[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	//strftime(mtime, sizeof(mtime), "%a, %d %b %Y %H:%M:%S GMT", &tm);
	return snprintf(buf, HTTP_TIME_BUF + 1,
		"%s, %02d %s %4d %02d:%02d:%02d GMT",
		dayofweek[tm->tm_wday],
		tm->tm_mday,
		monthofyear[tm->tm_mon - 1],
		tm->tm_year + 1900,
		tm->tm_hour,
		tm->tm_min,
		tm->tm_sec);
}

struct resp {
	uint8_t *r_ptr;
	uint8_t *r_end;
	size_t r_len;
};

static void resp_begin(struct _http_conn *h, struct resp *r)
{
	size_t sz;
	r->r_ptr = buf_write(h->h_res, &sz);
	r->r_end = r->r_ptr + sz;
	r->r_len = 0;
}

#define resp_static_string(r, s) resp_string(r, (uint8_t *)s, strlen(s))
static void resp_string(struct resp *r, const uint8_t *s, size_t len)
{
	assert(r->r_ptr + len <= r->r_end);
	memcpy(r->r_ptr, s, len);
	r->r_ptr += len;
	r->r_len += len;
}

static void resp_u64(struct resp *r, uint64_t u)
{
	int ret;
	ret = snprintf((char *)r->r_ptr, r->r_end - r->r_ptr, "%"PRIu64, u);
	assert(r->r_ptr + ret <= r->r_end);
	r->r_ptr += ret;
	r->r_len += ret;
}

static void resp_etag(struct resp *r, const uint8_t etag[ETAG_SZ])
{
	assert(r->r_ptr + ETAG_SZ * 2 + 1 <= r->r_end);
	print_etag((char *)r->r_ptr, etag);
	r->r_ptr += ETAG_SZ * 2;
	r->r_len += ETAG_SZ * 2;
}

static void resp_time(struct resp *r, struct tm *tm)
{
	int len;
	assert(r->r_ptr + HTTP_TIME_BUF <= r->r_end);
	len = print_time((char *)r->r_ptr, tm);
	len = (len < 0) ? 0 : len;
	r->r_ptr += len;
	r->r_len += len;
}

static void resp_http_code(struct resp *r, unsigned code)
{
	assert(r->r_ptr + 3 <= r->r_end);
	r->r_ptr[0] = '0' + ((code / 100) % 10);
	r->r_ptr[1] = '0' + ((code / 10) % 10);
	r->r_ptr[2] = '0' + (code % 10);
	r->r_ptr += 3;
	r->r_len += 3;
}

uint64_t reqs;
static int handle_get(struct iothread *t, struct _http_conn *h,
			struct http_request *r, int head)
{
	struct webroot_name n;
	struct ro_vec search_uri = r->uri;
	struct resp res;
	struct tm tm;
	char etag[41];
	char hbuf[r->host.v_len + 1];
	time_t mt;
	struct nads nads;
	webroot_t root;
	int hit = 0;

	nads.buf = (char *)r->uri.v_ptr;
	nads.buf_len = r->uri.v_len;
	nads_normalize(&nads);

	snprintf(hbuf, sizeof(hbuf), "%.*s", (int)r->host.v_len, r->host.v_ptr);

	dprintf("GET %.*s -> '%s' '%s' (host %s)\n",
		(int)r->uri.v_len, r->uri.v_ptr,
		nads.uri, nads.query, hbuf);

	search_uri.v_ptr = (uint8_t *)nads.uri;
	search_uri.v_len = strlen(nads.uri);

	root = vhosts_lookup(h->h_owner->l_vhosts, hbuf);
	if ( NULL == root ) {
		return response_403(t, h);
	}

	if ( !webroot_find(root, &search_uri, &n) ) {
#if 0
		h->h_data_off = obj404_f_ofs;
		h->h_data_len = obj404_f_len;
		mime_type = obj404_mime_type;
#else
		dprintf("404\n");
		return response_404(t, h);
#endif
	}else{
		switch(n.code) {
		case HTTP_MOVED_PERMANENTLY:
			dprintf("301 -> %.*s\n",
				(int)n.u.moved.v_len, n.u.moved.v_ptr);
			return response_301(t, h, &r->host, &n.u.moved);
		case HTTP_FORBIDDEN:
			return response_403(t, h);
		case HTTP_FOUND:
			h->h_data_off = n.u.data.f_ofs;
			h->h_data_len = n.u.data.f_len;
			break;
		}
	}

	print_etag(etag, n.u.data.f_etag);

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
			response_500(t, h);
			return 1;
		}
	}

	resp_begin(h, &res);
	resp_static_string(&res, "HTTP/1.1 ");
	resp_http_code(&res, n.code);
	switch(n.code) {
	case 200:
		resp_static_string(&res, " OK");
		break;
	case HTTP_NOT_MODIFIED:
		resp_static_string(&res, " Not Modified");
		break;
	case 404:
	default:
		resp_static_string(&res, " Not Found");
		break;
	}

	resp_static_string(&res, "\r\nContent-Type: ");
	resp_string(&res, n.mime_type.v_ptr, n.mime_type.v_len);

	resp_static_string(&res, "\r\nContent-Length: ");
	resp_u64(&res, h->h_data_len);

	resp_static_string(&res, "\r\nConnection: ");
	if ( h->h_conn_close ) {
		resp_static_string(&res, "Close");
	}else{
		resp_static_string(&res, "Keep-Alive");
	}

	resp_static_string(&res, "\r\nETag: ");
	resp_etag(&res, n.u.data.f_etag);

	resp_static_string(&res, "\r\nLast-Modified: ");
	mt = n.u.data.f_mtime;
	gmtime_r(&mt, &tm);
	resp_time(&res, &tm);

	resp_static_string(&res, "\r\nServer: ashttpd\r\n\r\n");

	reqs++;
	buf_done_write(h->h_res, res.r_len);
	dprintf("%.*s\n", (int)res.r_len, h->h_res->b_base);
	if ( head )
		h->h_data_len = 0;
	else {
		h->h_webroot = root;
		webroot_ref(h->h_webroot);
	}
	return 1;
}

static void handle_request(struct iothread *t, struct _http_conn *h)
{
	struct http_request r;
	size_t hlen, sz;
	const uint8_t *ptr;
	int ret;

	assert(h->h_state == HTTP_CONN_REQUEST);

	/* first try allocate buffer to respond,
	 * we always need to respond, so do this
	 * first and kill the conn if we fail
	*/
	h->h_res = buf_alloc_res();
	if ( NULL == h->h_res ) {
		printf("OOM on res...\n");
		http_kill(t, h);
		return;
	}

	/* Respond or die! */
	h->h_state = HTTP_CONN_HEADER;
	nbio_set_wait(t, &h->h_nbio, NBIO_WRITE);

	/* Parse the request */
	ptr = buf_read(h->h_req, &sz);
	memset(&r, 0, sizeof(r));
	hlen = http_req(&r, ptr, sz);
	dprintf("%zu/%zu bytes were request\n", hlen, sz);
	dprintf("%.*s\n", (int)sz, ptr);
	if ( 0 == hlen ) {
		response_400(t, h);
		return;
	}

	if ( r.content_len ) {
		printf("Argh, Content-Length set on request\n");
		http_kill(t, h);
		return;
	}

	/* For requests with no host header (HTTP/1.0, or malformed HTTP/1.1
	 * we use getsockname to figure out what our host is.
	 */
	if ( !r.host.v_len ) {
		struct sockaddr_in in;
		socklen_t len = sizeof(in);
		char buf[64];

		getsockname(h->h_nbio.fd, (struct sockaddr *)&in, &len);

		r.host.v_len = snprintf((char *)buf, 64, "%s:%d",
				inet_ntoa((struct in_addr){in.sin_addr.s_addr}),
				ntohs(in.sin_port));
		r.host.v_ptr = (uint8_t *)buf;
	}

	h->h_conn_close = r.conn_close;

	if ( !vstrcmp_fast(&r.method, "GET") ) {
		ret = handle_get(t, h, &r, 0);
	}else if ( !vstrcmp_fast(&r.method, "HEAD") ) {
		ret = handle_get(t, h, &r, 1);
	}else{
		ret = response_501(t, h);
	}

	if ( !ret ) {
		http_kill(t, h);
		return;
	}

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
	webroot_unref(h->h_webroot);
	hgang_return(conns, n);
}

static const struct nbio_ops http_ops = {
	.read = http_read,
	.write = http_write,
	.dtor = http_dtor,
};

void http_conn(struct iothread *t, int s, void *priv)
{
	struct http_listener *hl = priv;
	struct _http_conn *h;

	h = hgang_alloc0(conns);
	if ( NULL == h ) {
		fprintf(stderr, "hgang_alloc: %s\n", os_err());
		close(s);
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

__attribute__((noreturn)) static void sigint(int sig)
{
	printf("\n%"PRIu64" reqs handled\n", reqs);
	exit(1);
}
int http_proto_init(struct iothread *t)
{
	signal(SIGINT, sigint);
	conns = hgang_new(sizeof(struct _http_conn), 0);
	if ( NULL == conns ) {
		fprintf(stderr, "conns: %s\n", os_err());
		return 0;
	}

	if ( !_io_init(t) ) {
		return 0;
	}

	return 1;
}

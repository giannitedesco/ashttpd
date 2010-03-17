#include <ashttpd.h>
#include <limits.h>
#include <ctype.h>

/* HTTP Decode Control Block */
struct http_hcb {
	const char *label;
	void (*fn)(struct http_hcb *, struct ro_vec *);
	union {
		struct ro_vec *vec;
		uint16_t *u16;
		int *val;
	}u;
};

/* Parse and HTTP version string (eg: "HTTP/1.0") */
static http_ver_t http_proto_version(struct ro_vec *str)
{
	const uint8_t *s = str->v_ptr;
	int maj, min;

	if ( str->v_ptr == NULL )
		return HTTP_VER_UNKNOWN;

	if ( str->v_len != 8 ) /* sizeof("HTTP/X.Y") */
		return HTTP_VER_UNKNOWN;

	if ( memcmp(s, "HTTP/", 5) )
		return HTTP_VER_UNKNOWN;

	if ( s[6] != '.' )
		return HTTP_VER_UNKNOWN;

	if ( !isdigit(s[5]) || !isdigit(s[7]) )
		return HTTP_VER_UNKNOWN;

	maj = s[5] - '0';
	min = s[7] - '0';

	return (min << 4) | maj;
}

static void htype_string(struct http_hcb *h, struct ro_vec *v)
{
	if ( !h->u.vec )
		return;

	h->u.vec->v_ptr = v->v_ptr;
	h->u.vec->v_len = v->v_len;
}

static void htype_present(struct http_hcb *h, struct ro_vec *v)
{
	*h->u.val = 1;
}

static void htype_int(struct http_hcb *h, struct ro_vec *v)
{
	unsigned int val;
	size_t len;

	len = vtouint(v, &val);
	if ( !len || val > INT_MAX ) {
		*h->u.val = -1;
	}else{
		*h->u.val = val;
	}
}

/* Same as int but ensure input is only 3 digits */
#if 0
static void htype_code(struct http_hcb *h, struct ro_vec *v)
{
	unsigned int val;
	size_t len;

	len = vtouint(v, &val);
	if ( len != 3 || val > 999 )
		*h->u.u16 = 0xffff;
	else
		*h->u.u16 = val;
}
#endif

/* Check if this header is one we want to store */
static inline void dispatch_hdr(struct http_hcb *dcb,
				size_t num_dcb,
				struct ro_vec *k,
				struct ro_vec *v)
{
	unsigned int n;
	struct http_hcb *d;

	for(n = num_dcb, d = dcb; n; ) {
		unsigned int i;
		int ret;

		i = (n / 2);
		ret = vstrcmp(k, d[i].label);
		if ( ret < 0 ) {
			n = i;
		}else if ( ret > 0 ) {
			d = d + (i + 1);
			n = n - (i + 1);
		}else{
			d[i].fn(&d[i], v);
			break;
		}
	}
}

/* Get a version code for a given major and minor version */
/* Actually parse an HTTP request */
static size_t http_decode_buf(struct http_hcb *d, size_t num_dcb,
				const uint8_t *p, const uint8_t *end)
{
	const uint8_t *cur;
	struct ro_vec hv[3]; /* method, url, proto */
	struct ro_vec k,v;
	int i = 0;
	int state = 0;
	int ret = 0;

	hv[0].v_len = 0;
	hv[1].v_len = 0;
	hv[2].v_len = 0;

	for(cur = p; cur < end; cur++) {
		switch ( state ) {
		case 0:
			if ( *cur != ' ' ) {
				state = 1;
				hv[i].v_ptr = (void *)cur;
				hv[i].v_len = 0;
			}
			break;
		case 1:
			hv[i].v_len++;
			switch(*cur) {
			case ' ':
				if ( i<2 ) {
					state = 0;
					i++;
				}
				break;
			case '\n':
				if ( hv[i].v_len && *(cur - 1) == '\r' )
					hv[i].v_len--;
				k.v_ptr = (void *)cur + 1;
				k.v_len = 0;
				state = 2;
				ret = (cur - p) + 1;
				break;
			}
			break;
		case 2:
			if ( *cur == ':' ) {
				state = 3;
				break;
			}else if ( *cur == '\n' ) {
				ret = (cur - p) + 1;
				cur = end;
			}
			k.v_len++;
			break;
		case 3:
			if ( *cur != ' ' ) {
				v.v_ptr = (void *)cur;
				v.v_len = 0;
				state = 4;
			}
			break;
		case 4:
			v.v_len++;
			if ( *cur == '\n' ) {
				if ( v.v_len && *(cur-1) == '\r' )
					v.v_len--;
				dispatch_hdr(d + 3, num_dcb - 3, &k, &v);
				k.v_ptr = (void *)cur + 1;
				k.v_len = 0;
				state = 2;
				break;
			}
			break;
		}
	}

	if ( !hv[0].v_len || !hv[1].v_len )
		return 0;

	/* Setup method/url/proto */
	d[0].fn(&d[0], &hv[0]);
	d[1].fn(&d[1], &hv[1]);
	d[2].fn(&d[2], &hv[2]);

	return ret;
}

/* Parse an HTTP request header and fill in the http response structure */
size_t http_req(struct http_request *r, const uint8_t *ptr, size_t len)
{
	const uint8_t *end = ptr + len;
	int clen = -1;
	size_t hlen;
	struct ro_vec pv = {0,};
	int prox = 0;
	size_t i;
	int state, do_host;
	struct http_hcb hcb[] = {
		{"method", htype_string, {.vec = &r->method}},
		{"uri", htype_string, {.vec = &r->uri}},
		{"protocol", htype_string, {.vec = &pv}},
		{"Host", htype_string , {.vec = &r->host}},
		{"Content-Type", htype_string,
					{.vec = &r->content_type}},
		{"Content-Length", htype_int, {.val = &clen}},
		{"Proxy-Connection", htype_present, {.val = &prox}},
		{"Content-Encoding", htype_string,
					{.vec = &r->content_enc}},
		{"Transfer-Encoding", htype_string,
					{.vec = &r->transfer_enc}},
	};

	/* Do the decode */
	hlen = http_decode_buf(hcb, sizeof(hcb)/sizeof(*hcb), ptr, end);
	if ( !hlen )
		return 0;

	r->proto_vers = http_proto_version(&pv);

	if ( clen > 0 )
		r->content.v_len = clen;

	/* Strip out Request-URI to just abs_path, Filling in host
	 * information if there was no host header
	 */
	if ( !prox ) {
		if ( r->uri.v_len < 7 ||
			strncasecmp((const char *)r->uri.v_ptr, "http://", 7) )
			goto done;
	}

	for(i = state = do_host = 0; i < r->uri.v_len; i++) {
		if ( state == 0 ) {
			if ( ((char *)r->uri.v_ptr)[i] == ':' )
				state++;
		}else if ( state >= 1 && state < 4 ) {
			if ( ((char *)r->uri.v_ptr)[i] == '/' ) {
				if ( state == 3 && !r->host.v_ptr ) {
					r->host.v_ptr = r->uri.v_ptr + i + 1;
					r->host.v_len = 0;
					do_host = 1;
				}
				state++;
			}else if ( do_host ) {
				r->host.v_len++;
			}
		}else{
			i -= 1;
			break;
		}
	}

	if ( state < 3 )
		goto done;

	r->uri.v_ptr += i;
	r->uri.v_len -= i;

	if ( r->uri.v_len == 0 )
		r->uri.v_ptr = NULL;

done:
	/* Extract the port from the host header */
	r->port = HTTP_DEFAULT_PORT;
	if ( r->host.v_ptr ) {
		size_t i;
		struct ro_vec port = { .v_ptr = NULL };
		unsigned int prt;

		for(i = r->host.v_len; i; i--) {
			if (  ((uint8_t *)r->host.v_ptr)[i-1] == ':' ) {
				port.v_len = r->host.v_len - i;
				port.v_ptr = r->host.v_ptr + i;
				r->host.v_len = i - 1;
				break;
			}
		}

		if ( port.v_len ) {
			if ( vtouint(&port, &prt) == port.v_len ) {
				if ( prt & ~0xffffUL ) {
					/* TODO */
					//alert_tag(p, &a_invalid_port, -1);
				}else{
					r->port = prt;
				}
			}
		}
	}

	/* rfc2616: An empty abs_path is equivalent to an abs_path of "/" */
	if ( r->uri.v_ptr == NULL ) {
		r->uri.v_ptr = (const uint8_t *)"/";
		r->uri.v_len = 1;
	}

	return hlen;
}


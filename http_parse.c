#include <limits.h>
#include <ctype.h>

#include <ashttpd.h>
#include <http-parse.h>

/* Parse and HTTP version string (eg: "HTTP/1.0") */
http_ver_t http_proto_version(struct ro_vec *str)
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
	if ( maj < 0 || maj > 9 )
		return HTTP_VER_UNKNOWN;
	if ( min < 0 || min > 9 )
		return HTTP_VER_UNKNOWN;

	return (min << 4) | maj;
}

void htype_string(struct http_hcb *h, struct ro_vec *v)
{
	if ( !h->u.vec )
		return;

	h->u.vec->v_ptr = v->v_ptr;
	h->u.vec->v_len = v->v_len;
}

void htype_present(struct http_hcb *h, struct ro_vec *v)
{
	*h->u.val = 1;
}

void htype_int(struct http_hcb *h, struct ro_vec *v)
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
void htype_code(struct http_hcb *h, struct ro_vec *v)
{
	unsigned int val;
	size_t len;

	len = vtouint(v, &val);
	if ( len != 3 || val > 999 )
		*h->u.u16 = 0xffff;
	else
		*h->u.u16 = val;
}

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
		ret = vstrcmp_fast(k, d[i].label);
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
size_t http_decode_buf(struct http_hcb *d, size_t num_dcb,
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

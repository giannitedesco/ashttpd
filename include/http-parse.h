#ifndef _HTTP_PARSE_H
#define _HTTP_PARSE_H

#define HTTP_VER_UNKNOWN	0xff
#define HTTP_VER_0_9		0x09
#define HTTP_VER_1_0		0x10
#define HTTP_VER_1_1		0x11
typedef uint8_t http_ver_t;

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

_private http_ver_t http_proto_version(struct ro_vec *str);
_private void htype_string(struct http_hcb *h, struct ro_vec *v);
_private void htype_present(struct http_hcb *h, struct ro_vec *v);
_private void htype_int(struct http_hcb *h, struct ro_vec *v);
_private size_t http_decode_buf(struct http_hcb *d, size_t num_dcb,
				const uint8_t *p, const uint8_t *end);

#endif /*_HTTP_PARSE_H */

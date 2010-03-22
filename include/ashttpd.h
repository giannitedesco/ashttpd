#ifndef _ASHTTPD_H
#define _ASHTTPD_H

#include <compiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <list.h>
#include <nbio.h>
#include <os.h>

#define HTTP_MAX_REQ		2048
#define HTTP_MAX_RESP		1024
#define HTTP_DATA_BUFFER	4096

struct vec {
	uint8_t *v_ptr;
	size_t v_len;
};

struct ro_vec {
	const uint8_t *v_ptr;
	size_t v_len;
};

_private int vcasecmp(const struct ro_vec *v1, const struct ro_vec *v2);
_private int vcmp(const struct ro_vec *v1, const struct ro_vec *v2);
_private int vstrcmp(const struct ro_vec *v1, const char *str);
_private int vcasecmp_fast(const struct ro_vec *v1, const struct ro_vec *v2);
_private int vcmp_fast(const struct ro_vec *v1, const struct ro_vec *v2);
_private int vstrcmp_fast(const struct ro_vec *v1, const char *str);
_private size_t vtouint(struct ro_vec *v, unsigned int *u);

#define BM_SKIP_LEN 0x100
_private void bm_skip(const uint8_t *x, size_t plen, int *skip);
_private const uint8_t *bm_find(const uint8_t *n, size_t nlen,
			const uint8_t *hs, size_t hlen,
			int *skip);

struct webroot_name {
	struct ro_vec name;
	unsigned int mime_type;
	off_t f_ofs;
	size_t f_len;
};

_private int generic_webroot_fd(const char *fn);
_private const struct webroot_name *webroot_find(struct ro_vec *uri);
_private const char * const webroot_mime_type(unsigned int idx);

/* FIXME: more comprehensive handling of "code" pages */
_private extern const off_t obj404_f_ofs;
_private extern const size_t obj404_f_len;
_private extern const unsigned int obj404_mime_type;

#define HTTP_VER_UNKNOWN	0xff
#define HTTP_VER_0_9		0x09
#define HTTP_VER_1_0		0x10
#define HTTP_VER_1_1		0x11
typedef uint8_t http_ver_t;

#define HTTP_DEFAULT_PORT	80
struct http_request {
	struct ro_vec	method;
	struct ro_vec	host;
	struct ro_vec	uri;
	struct ro_vec	uri_path;
	struct ro_vec	uri_query;
	struct ro_vec	transfer_enc;
	struct ro_vec	content_type;
	struct ro_vec	content_enc;
	struct ro_vec	content;
	http_ver_t 	proto_vers;
	uint8_t 	_pad0;
	uint16_t 	port;
};
_private size_t http_req(struct http_request *r,
			const uint8_t *ptr, size_t len);

#endif /* _ASHTTPD_H */

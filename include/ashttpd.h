#ifndef _ASHTTPD_H
#define _ASHTTPD_H

#include <compiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <list.h>
#include <nbio.h>
#include <os.h>
#include <hgang.h>

#define HTTP_VEC_BYTES	512
#define HTTP_MAX_VEC	8
#define HTTP_MAX_REQ	(HTTP_VEC_BYTES * HTTP_MAX_VEC)

#define HTTP_MAX_RESP	1024

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

_private void http_conn(struct iothread *t, int s, void *priv);

#define HTTP_CONN_REQUEST	0
/* TODO: gobble any POST data */
#define HTTP_CONN_HEADER	1
#define HTTP_CONN_DATA		2
struct http_conn {
	struct nbio	h_nbio;
	unsigned int	h_state;

	uint8_t		*h_req;
	uint8_t		*h_req_ptr;
	const uint8_t	*h_req_end;

	uint8_t		*h_res;
	const uint8_t	*h_res_ptr;
	const uint8_t	*h_res_end;

	off_t		h_data_off;
	size_t		h_data_len;
};

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

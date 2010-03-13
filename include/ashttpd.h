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
#define HTTP_MAX_VEC	16
#define HTTP_MAX_REQ	(HTTP_VEC_BYTES * HTTP_MAX_VEC)

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

_private void http_conn(struct iothread *t, int s, void *priv);

struct http_conn {
	struct nbio	h_nbio;
	uint8_t		h_buf[HTTP_MAX_REQ];
	size_t		h_buflen;
};

#endif /* _ASHTTPD_H */

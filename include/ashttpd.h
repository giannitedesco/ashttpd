#ifndef _ASHTTPD_H
#define _ASHTTPD_H

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <compiler.h>
#include <list.h>
#include <nbio.h>
#include <vec.h>
#include <os.h>

typedef struct _webroot *webroot_t;

#define ETAG_SZ				20

#define HTTP_FOUND			200
#define HTTP_MOVED_PERMANENTLY		301
#define HTTP_FORBIDDEN			403
struct webroot_name {
	struct ro_vec mime_type;
	union {
		struct {
			off_t f_ofs;
			size_t f_len;
			uint32_t f_mtime;
			uint8_t f_etag[ETAG_SZ];
		}data;
		struct ro_vec moved;
	}u;
	unsigned int code;
};

_private webroot_t webroot_open(const char *fn);
_private int webroot_get_fd(webroot_t r);
_private int webroot_find(webroot_t r, const struct ro_vec *uri,
				struct webroot_name *out);
_private void webroot_close(webroot_t r);

#endif /* _ASHTTPD_H */

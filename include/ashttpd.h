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
#include <nbio-listener.h>
#include <vec.h>
#include <os.h>

typedef struct _webroot *webroot_t;

#define ETAG_SZ				20

#define HTTP_FOUND			200
#define HTTP_MOVED_PERMANENTLY		301
#define HTTP_NOT_MODIFIED		304
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

struct http_listener {
	struct list_head l_list;
	listener_t l_listen;
	webroot_t l_webroot;
};

typedef struct _vhosts *vhosts_t;
struct _vhosts *vhosts_new(struct iothread *t, const char *dirname);
webroot_t vhosts_lookup(vhosts_t v, const char *host);

/* webroot API */
_private webroot_t webroot_open(const char *fn);
_private int webroot_get_fd(webroot_t r);
_private int webroot_find(webroot_t r, const struct ro_vec *uri,
				struct webroot_name *out);
_private void webroot_close(webroot_t r);

/* handle HTTP protocol connections */
_private int http_proto_init(struct iothread *t);
_private void http_conn(struct iothread *t, int s, void *priv);
_private void http_oom(struct iothread *t, struct nbio *listener);

/* current file I/O model */
extern struct http_fio *fio_current;

#endif /* _ASHTTPD_H */

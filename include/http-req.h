#ifndef _HTTP_REQ_H
#define _HTTP_REQ_H

#define HTTP_DEFAULT_PORT	80
struct http_request {
	struct ro_vec	method;
	struct ro_vec	host;
	struct ro_vec	uri;
	struct ro_vec	query;
#if 0
	/* FUCK IT: no support for POST method */
	struct ro_vec	transfer_enc;
	struct ro_vec	content_type;
	struct ro_vec	content_enc;
#endif
	struct ro_vec	hostname;
	struct ro_vec	etag;
	size_t		content_len;
	uint16_t	port;
	http_ver_t	proto_vers;
	uint8_t		conn_close;
};
_private size_t http_req(struct http_request *r,
			const uint8_t *ptr, size_t len);

#endif /*_HTTP_REQ_H */

#ifndef _HTTP_REQ_H
#define _HTTP_REQ_H

#define HTTP_DEFAULT_PORT	80
struct http_request {
	struct ro_vec	method;
	struct ro_vec	host;
	struct ro_vec	uri;
	struct ro_vec	uri_path;
	struct ro_vec	uri_query;
	struct ro_vec	connection;
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

#endif /*_HTTP_REQ_H */

#ifndef _HTTP_RESP_H
#define _HTTP_RESP_H

struct http_response {
	struct ro_vec	host;
	size_t		content_len;
	uint16_t	code;
	http_ver_t 	proto_vers;
	uint8_t 	conn_close;
	struct ro_vec	transfer_enc;
	struct ro_vec	content_type;
	struct ro_vec	content_enc;
};
_private size_t http_resp(struct http_response *r,
				const uint8_t *ptr, size_t len);

#endif /*_HTTP_RESP_H */

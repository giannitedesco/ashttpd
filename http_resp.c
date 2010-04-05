#include <ashttpd.h>
#include <http-parse.h>
#include <http-resp.h>

/* Parse an HTTP response header and fill in the http response structure */
size_t http_resp(struct http_response *r, const uint8_t *ptr, size_t len)
{
	const uint8_t *end = ptr + len;
	int clen = -1;
	size_t hlen;
	struct ro_vec pv = {0,}, connection = {0,};
	struct http_hcb hcb[] = {
		{"protocol", htype_string, {.vec = &pv}},
		{"code", htype_code, {.u16 = &r->code}},
		{"msg", htype_string, {.vec = NULL}},
		{"Connection", htype_string, { .vec = &connection}},
		{"Content-Type", htype_string, {.vec = &r->content_type}},
		{"Content-Length", htype_int, {.val = &clen}},
		{"Content-Encoding", htype_string,
					{.vec = &r->content_enc}},
		{"Transfer-Encoding", htype_string,
					{.vec = &r->transfer_enc}},
	};
	static const struct ro_vec close_token = {
		.v_ptr = (uint8_t *)"Close",
		.v_len = 5,
	};

	hlen = http_decode_buf(hcb, sizeof(hcb)/sizeof(*hcb), ptr, end);
	if ( !hlen )
		return 0;

	if ( clen > 0 )
		r->content_len = clen;

	if ( !vcasecmp_fast(&connection, &close_token) ) {
		r->conn_close = 1;
	}else{
		r->conn_close = 0;
	}

	r->proto_vers = http_proto_version(&pv);

	return hlen;
}

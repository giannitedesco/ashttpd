#include <ashttpd.h>
#include <http-parse.h>
#include <http-req.h>

/* Parse an HTTP request header and fill in the http response structure */
size_t http_req(struct http_request *r, const uint8_t *ptr, size_t len)
{
	const uint8_t *end = ptr + len;
	int clen = -1;
	size_t hlen;
	struct ro_vec pv = {0,};
	struct http_hcb hcb[] = {
		{"method", htype_string, {.vec = &r->method}},
		{"uri", htype_string, {.vec = &r->uri}},
		{"protocol", htype_string, {.vec = &pv}},
		{"Host", htype_string , {.vec = &r->host}},
		{"Connection", htype_string, { .vec = &r->connection}},
		{"Content-Type", htype_string,
					{.vec = &r->content_type}},
		{"Content-Length", htype_int, {.val = &clen}},
		{"Content-Encoding", htype_string,
					{.vec = &r->content_enc}},
		{"Transfer-Encoding", htype_string,
					{.vec = &r->transfer_enc}},
	};

	/* Do the decode */
	hlen = http_decode_buf(hcb, sizeof(hcb)/sizeof(*hcb), ptr, end);
	if ( !hlen )
		return 0;

	r->proto_vers = http_proto_version(&pv);

	if ( clen > 0 )
		r->content.v_len = clen;

	/* Extract the port from the host header */
	r->port = HTTP_DEFAULT_PORT;
	if ( r->host.v_ptr ) {
		size_t i;
		struct ro_vec port = { .v_ptr = NULL };
		unsigned int prt;

		for(i = r->host.v_len; i; i--) {
			if (  ((uint8_t *)r->host.v_ptr)[i - 1] == ':' ) {
				port.v_len = r->host.v_len - i;
				port.v_ptr = r->host.v_ptr + i;
				r->host.v_len = i - 1;
				break;
			}
		}

		if ( port.v_len ) {
			if ( vtouint(&port, &prt) == port.v_len ) {
				if ( 0 == (prt & ~0xffffUL) )
					r->port = prt;
			}
		}
	}

	/* rfc2616: An empty abs_path is equivalent to an abs_path of "/" */
	if ( r->uri.v_ptr == NULL ) {
		r->uri.v_ptr = (const uint8_t *)"/";
		r->uri.v_len = 1;
	}

	/* FIXME: Split out query strings */

	return hlen;
}

#include <ashttpd.h>
#include <http-parse.h>
#include <http-req.h>

/* Parse an HTTP request header and fill in the http response structure */
size_t http_req(struct http_request *r, const uint8_t *ptr, size_t len)
{
	const uint8_t *end = ptr + len;
	int clen = -1;
	size_t hlen;
	struct ro_vec pv = {0,}, connection = {0,};
	struct http_hcb hcb[] = {
		{"method", htype_string, {.vec = &r->method}},
		{"uri", htype_string, {.vec = &r->uri}},
		{"protocol", htype_string, {.vec = &pv}},
		{"Host", htype_string , {.vec = &r->host}},
		{"Connection", htype_string, { .vec = &connection}},
#if 0
		{"Content-Type", htype_string,
					{.vec = &r->content_type}},
#endif
		{"Content-Length", htype_int, {.val = &clen}},
#if 0
		{"Content-Encoding", htype_string,
					{.vec = &r->content_enc}},
		{"Transfer-Encoding", htype_string,
					{.vec = &r->transfer_enc}},
#endif
	};

	/* Do the decode */
	hlen = http_decode_buf(hcb, sizeof(hcb)/sizeof(*hcb), ptr, end);
	if ( !hlen )
		return 0;

	r->proto_vers = http_proto_version(&pv);

	if ( clen > 0 )
		r->content_len = clen;

	if ( r->proto_vers >= HTTP_VER_1_1 ) {
		static const struct ro_vec close_token = {
			.v_ptr = (uint8_t *)"Close",
			.v_len = 5,
		};
		if ( !vcasecmp_fast(&connection, &close_token) ) {
			r->conn_close = 1;
		}else{
			r->conn_close = 0;
		}
	}else{
		/* For HTTP/1.0 close connection regardless of connection
		 * header token. Due to buggy proxies which may pass on
		 * connection: Keep-Alive token without understanding it
		 * resulting in a hung connection
		 */
		r->conn_close = 1;
	}

	/* Extract the port from the host header */
	r->port = HTTP_DEFAULT_PORT;
	if ( r->host.v_ptr ) {
		size_t i;
		struct ro_vec port = { .v_ptr = NULL };
		unsigned int prt;

		memcpy(&r->hostname, &r->host, sizeof(r->hostname));

		for(i = r->hostname.v_len; i; i--) {
			if (  ((uint8_t *)r->hostname.v_ptr)[i - 1] == ':' ) {
				port.v_len = r->hostname.v_len - i;
				port.v_ptr = r->hostname.v_ptr + i;
				r->hostname.v_len = i - 1;
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

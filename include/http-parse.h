#ifndef _HTTP_PARSE_H
#define _HTTP_PARSE_H

#define HTTP_VER_UNKNOWN	0xff
#define HTTP_VER_0_9		0x09
#define HTTP_VER_1_0		0x10
#define HTTP_VER_1_1		0x11
typedef uint8_t http_ver_t;

/* HTTP Decode Control Block */
struct http_hcb {
	const char *label;
	void (*fn)(struct http_hcb *, struct ro_vec *);
	union {
		struct ro_vec *vec;
		uint16_t *u16;
		int *val;
	}u;
};

_private http_ver_t http_proto_version(struct ro_vec *str);
_private void htype_string(struct http_hcb *h, struct ro_vec *v);
_private void htype_present(struct http_hcb *h, struct ro_vec *v);
_private void htype_int(struct http_hcb *h, struct ro_vec *v);
_private size_t http_decode_buf(struct http_hcb *d, size_t num_dcb,
				const uint8_t *p, const uint8_t *end);

/* State machine for incremental HTTP request parse */
#define RSTATE_INITIAL		0
#define RSTATE_CR		1
#define RSTATE_LF		2
#define RSTATE_CRLF		3
#define RSTATE_LFCR		4
#define RSTATE_CRLFCR		5
#define RSTATE_LFLF		6
#define RSTATE_CRLFLF		7
#define RSTATE_LFCRLF		8
#define RSTATE_CRLFCRLF		9

#define RSTATE_NR_NONTERMINAL	RSTATE_LFLF
#define RSTATE_TERMINAL(x)	((x) >= RSTATE_LFLF)
static inline int http_parse_incremental(uint8_t *state,
					const uint8_t **rptr,
					const uint8_t *end)
{
	static const uint8_t cr_map[RSTATE_NR_NONTERMINAL] = {
			[RSTATE_INITIAL] = RSTATE_CR,
			[RSTATE_CR] = RSTATE_CR,
			[RSTATE_LF] = RSTATE_LFCR,
			[RSTATE_CRLF] = RSTATE_CRLFCR,
			[RSTATE_LFCR] = RSTATE_CR,
			[RSTATE_CRLFCR] = RSTATE_CR};
	static const uint8_t lf_map[RSTATE_NR_NONTERMINAL] = {
			[RSTATE_INITIAL] = RSTATE_LF,
			[RSTATE_CR] = RSTATE_CRLF,
			[RSTATE_LF] = RSTATE_LFLF,
			[RSTATE_CRLF] = RSTATE_CRLFLF,
			[RSTATE_LFCR] = RSTATE_LFCRLF,
			[RSTATE_CRLFCR] = RSTATE_CRLFCRLF};

	assert(end >= *rptr);

	for(; *rptr < end; (*rptr)++) {
		switch((*rptr)[0]) {
		case '\r':
			assert((*state) < RSTATE_NR_NONTERMINAL);
			(*state) = cr_map[(*state)];
			break;
		case '\n':
			assert((*state) < RSTATE_NR_NONTERMINAL);
			(*state) = lf_map[(*state)];
			break;
		default:
			(*state) = RSTATE_INITIAL;
			continue;
		}
		if ( RSTATE_TERMINAL((*state)) )
			return 1;
	}

	return 0;
}


#endif /*_HTTP_PARSE_H */

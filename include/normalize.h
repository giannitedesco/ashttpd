#ifndef __NORMALIZE_HEADER_INCLUDED__
#define __NORMALIZE_HEADER_INCLUDED__

struct nads {
	const char	*buf;
	size_t		buf_len;
	char		*uri;
	char		*query;
};

#define NADS_MAX_URI			16384

/* return values are one of these */
#define NADS_OK				1
#define NADS_FAIL			0

/* Possible error codes */
#define NADS_ERR_SUCCESS		0
#define NADS_ERR_NO_EMULATION		1
#define NADS_ERR_BAD_HEX		2
#define NADS_ERR_TRAVERSAL		3
#define NADS_ERR_BAD_UTF8		4
#define NADS_ERR_NULL			5
#define NADS_ERR_OOM			6

/* Web-server details */
#define NADS_WS_CASE_SENSITIVE		0x01UL /* Server has case sensitive paths */
#define NADS_WS_MS_UTF16		0x02UL /* MS-UTF-16 (%u1234) */
#define NADS_WS_PERCENT_HACK		0x04UL /* Allow %%35 etc. */
#define NADS_WS_DOUBLE_HEX		0x08UL /* Do hex decode twice */

/* =====[ Exported API ]===== */

int nads_normalize(struct nads *res);
int nads_error(void);
const char *nads_strerror(unsigned int err);
unsigned int nads_errno(void);
const char *nads_errstr(void);

#endif /* __NORMALIZE_HEADER_INCLUDED__ */

#ifndef _ASHTTPD_BUF_H
#define _ASHTTPD_BUF_H

#define HTTP_MAX_REQ		2048
#define HTTP_MAX_RESP		1024
#define HTTP_DATA_BUFFER	4096

struct http_buf {
	/* On the real */
	uint8_t		*b_base;
	const uint8_t	*b_end;

	/* For the reader */
	const uint8_t	*b_read;

	/* For the writer */
	uint8_t		*b_write;
};

_private struct http_buf *buf_alloc_naked(void);
_private void buf_free_naked(struct http_buf *b);

_private struct http_buf *buf_alloc_req(void);
_private void buf_free_req(struct http_buf *b);

_private struct http_buf *buf_alloc_res(void);
_private void buf_free_res(struct http_buf *b);

_private struct http_buf *buf_alloc_data(void);
_private void buf_free_data(struct http_buf *b);

_private const uint8_t *buf_read(struct http_buf *b, size_t *sz);
_private uint8_t *buf_write(struct http_buf *b, size_t *sz);

_private size_t buf_done_read(struct http_buf *b, size_t sz);
_private size_t buf_done_write(struct http_buf *b, size_t sz);

_private void buf_reset(struct http_buf *b);

#endif /* _ASHTTPD_BUF_H */

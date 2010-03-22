#ifndef _ASHTTPD_CONN_H
#define _ASHTTPD_CONN_H

typedef struct _http_conn *http_conn_t;
_private size_t http_conn_data(http_conn_t h, int *fd, off_t *off);
_private size_t http_conn_data_read(http_conn_t h, size_t len);
_private void http_conn_data_complete(struct iothread *t, http_conn_t h);
_private void http_conn_abort(struct iothread *t, http_conn_t h);
_private void *http_conn_get_priv(http_conn_t h, unsigned short *s);
_private void http_conn_set_priv(http_conn_t h, void *priv, unsigned short s);
_private int http_conn_socket(http_conn_t h);
_private void http_conn_inactive(struct iothread *t, http_conn_t h);
_private void http_conn_wait_on(struct iothread *t, http_conn_t h,
				unsigned short w);
_private void http_conn_to_waitq(struct iothread *t, http_conn_t h,
				struct list_head *wq);
_private void http_conn_wake_one(struct iothread *t, http_conn_t h);
_private void http_conn_wake(struct iothread *t, struct list_head *waitq,
				void(*)(struct iothread *, http_conn_t));

#endif /* _ASHTTPD_CONN_H */

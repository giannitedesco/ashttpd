#ifndef _ASHTTPD_FIO_H
#define _ASHTTPD_FIO_H

struct http_fio {
	const char *label;
	int (*prep)(struct iothread *t, http_conn_t h);
	int (*write)(struct iothread *t, http_conn_t h);
	void (*abort)(http_conn_t h);
	int (*init)(struct iothread *t);
	void (*fini)(struct iothread *t);
};

_private extern struct http_fio fio_sync;
_private extern struct http_fio fio_sendfile;
_private extern struct http_fio fio_async;
_private extern struct http_fio fio_async_sendfile;

#endif /* _ASHTTPD_FIO_H */

/*
 * This file is part of Firestorm NIDS
 * Copyright (c) 2004 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
*/
#ifndef _NBIO_HEADER_INCLUDED_
#define _NBIO_HEADER_INCLUDED_

typedef uint8_t nbio_flags_t;

/* Represents a given fd */
struct nbio {
	int fd;
#define NBIO_READ	(1<<0)
#define NBIO_WRITE	(1<<1)
#define NBIO_ERROR	(1<<2)
#define NBIO_WAIT	(NBIO_READ|NBIO_WRITE|NBIO_ERROR)
	nbio_flags_t mask;
	nbio_flags_t flags;
	const struct nbio_ops *ops;
	struct list_head list;
	union {
		int poll;
		void *ptr;
	}ev_priv;
};

/* Represents all the I/Os for a given thread */
struct iothread {
	struct list_head inactive;
	struct list_head active;
	struct eventloop *plugin;
	union {
		int epoll;
		void *ptr;
	}priv;
	struct list_head deleted;
};

struct nbio_ops {
	void (*read)(struct iothread *t, struct nbio *n);
	void (*write)(struct iothread *t, struct nbio *n);
	void (*dtor)(struct iothread *t, struct nbio *n);
};

/* nbio API */
_private void nbio_add(struct iothread *, struct nbio *, nbio_flags_t);
_private void nbio_del(struct iothread *, struct nbio *);
_private void nbio_pump(struct iothread *, int mto);
_private void nbio_fini(struct iothread *);
_private int nbio_init(struct iothread *, const char *plugin);
_private void nbio_inactive(struct iothread *, struct nbio *, nbio_flags_t);
_private void nbio_set_wait(struct iothread *, struct nbio *, nbio_flags_t);
_private nbio_flags_t nbio_get_wait(struct nbio *io);
_private void nbio_to_waitq(struct iothread *, struct nbio *,
				struct list_head *q);
_private void nbio_wake(struct iothread *, struct nbio *, nbio_flags_t);
_private void nbio_wait_on(struct iothread *t, struct nbio *n, nbio_flags_t);

/* eventloop plugin API */
struct eventloop {
	const char *name;
	int (*init)(struct iothread *);
	void (*fini)(struct iothread *);
	void (*pump)(struct iothread *, int);
	void (*inactive)(struct iothread *, struct nbio *);
	void (*active)(struct iothread *, struct nbio *);
	struct eventloop *next;
};

_private void eventloop_add(struct eventloop *e);
_private struct eventloop *eventloop_find(const char *name);
_private void _eventloop_poll_ctor(void);
_private void _eventloop_epoll_ctor(void);

#endif /* _NBIO_HEADER_INCLUDED_ */

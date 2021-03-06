#ifndef _NBIO_LISTENER_H
#define _NBIO_LISTENER_H

typedef struct _listener *listener_t;

typedef void(*listener_cbfn_t)(struct iothread *t, int s, void *priv);
typedef void(*listener_oom_t)(struct iothread *t, struct nbio *io);

_private listener_t listener_inet(struct iothread *t, int type, int proto,
					uint32_t addr, uint16_t port,
					listener_cbfn_t cb, void *priv,
					listener_oom_t oom);
_private void listener_wake(struct iothread *t, struct nbio *io);

#endif /* _NBIO_LISTENER_H */

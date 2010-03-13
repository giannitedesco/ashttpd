#ifndef _NBIO_LISTENER_H
#define _NBIO_LISTENER_H

typedef void(*listener_cbfn_t)(struct iothread *t, int s, void *priv);

_private struct nbio *listener_inet(int type, int proto,
					uint32_t addr, uint16_t port,
					listener_cbfn_t cb, void *priv);

_private void listener_add(struct iothread *t, struct nbio *io);

#endif /* _NBIO_LISTENER_H */

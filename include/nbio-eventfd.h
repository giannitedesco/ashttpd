#ifndef _NBIO_EVENTFD_H
#define _NBIO_EVENTFD_H

#include <sys/eventfd.h>

typedef void(*eventfd_cb_t)(struct iothread *t, void *priv, eventfd_t val);

_private struct nbio *nbio_eventfd_new(eventfd_t initval,
					eventfd_cb_t cb, void *priv);
_private void nbio_eventfd_add(struct iothread *t, struct nbio *io);

#endif /* _NBIO_EVENTFD_H */

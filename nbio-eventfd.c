/*
 * This is the listener object it manages listening TCP sockets, for
 * each new connection that comes in we spawn off a new proxy object.
*/

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include <compiler.h>
#include <list.h>
#include <nbio.h>
#include <nbio-eventfd.h>
#include <os.h>

struct nb_efd {
	struct nbio	e_nbio;
	eventfd_cb_t	e_cb;
	void		*e_priv;
};

static void efd_read(struct iothread *t, struct nbio *n)
{
	struct nb_efd *efd = (struct nb_efd *)n;
	eventfd_t val;

	if ( eventfd_read(efd->e_nbio.fd, &val) ) {
		if ( errno == EAGAIN ) {
			nbio_inactive(t, &efd->e_nbio, NBIO_READ);
			return;
		}
		fprintf(stderr, "eventfd_read: %s\n", os_err());
		return;
	}

	efd->e_cb(t, efd->e_priv, val);
}

static void efd_dtor(struct iothread *t, struct nbio *n)
{
	struct nb_efd *efd = (struct nb_efd *)n;
	close(efd->e_nbio.fd);
	free(efd);
}

static const struct nbio_ops ops = {
	.read = efd_read,
	.dtor = efd_dtor,
};

struct nbio *nbio_eventfd_new(eventfd_t initval, eventfd_cb_t cb, void *priv)
{
	struct nb_efd *efd;

	efd = calloc(1, sizeof(*efd));
	if ( NULL == efd ) {
		fprintf(stderr, "nbio_eventfd_new: %s\n", os_err());
		return 0;
	}

	efd->e_nbio.fd = eventfd(initval, EFD_NONBLOCK|EFD_CLOEXEC);
	if ( efd->e_nbio.fd < 0 ) {
		fprintf(stderr, "eventfd: %s\n", os_err());
		free(efd);
		return 0;
	}

	efd->e_nbio.ops = &ops;
	efd->e_cb = cb;
	efd->e_priv = priv;
	return &efd->e_nbio;
}

void nbio_eventfd_add(struct iothread *t, struct nbio *io)
{
	nbio_add(t, io, NBIO_READ);
}

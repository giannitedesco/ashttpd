/*
 * This is the listener object it manages listening TCP sockets, for
 * each new connection that comes in we spawn off a new proxy object.
*/

#include <compiler.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <list.h>
#include <nbio.h>
#include <nbio-listener.h>
#include <os.h>

struct listener {
	struct nbio io;
	listener_cbfn_t cbfn;
	void *priv;
};

static void listener_read(struct iothread *t, struct nbio *io)
{
	struct listener *l = (struct listener *)io;
	struct sockaddr_in sa;
	socklen_t salen = sizeof(sa);
	int fd;

	fd = accept(l->io.fd, (struct sockaddr *)&sa, &salen);
	if ( fd < 0 ) {
		if ( errno == EAGAIN )
			nbio_inactive(t, &l->io);
		return;
	}

	if ( !os_socket_nonblock(fd) )
		return;

	printf("Accepted connection from %s:%u\n",
		inet_ntoa(sa.sin_addr),
		htons(sa.sin_port));

	l->cbfn(t, fd, l->priv);
}

static void listener_dtor(struct iothread *t, struct nbio *io)
{
	os_fd_close(io->fd);
	free(io);
}

static struct nbio_ops listener_ops = {
	.read = listener_read,
	.dtor = listener_dtor,
};

struct nbio *listener_inet(int type, int proto, uint32_t addr, uint16_t port,
				listener_cbfn_t cb, void *priv)
{
	struct sockaddr_in sa;
	struct listener *l;

	l = calloc(1, sizeof(*l));
	if ( l == NULL )
		return NULL;

	INIT_LIST_HEAD(&l->io.list);

	l->cbfn = cb;
	l->priv = priv;

	l->io.fd = socket(PF_INET, type, proto);
	if ( l->io.fd < 0 )
		goto err_free;

#if 1
	do{
		int val = 1;
		setsockopt(l->io.fd, SOL_SOCKET, SO_REUSEADDR,
				&val, sizeof(val));
	}while(0);
#endif

	if ( !os_socket_nonblock(l->io.fd) )
		goto err_close;

	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(addr);
	sa.sin_port = htons(port);

	if ( bind(l->io.fd, (struct sockaddr *)&sa, sizeof(sa)) )
		goto err_close;

	if ( listen(l->io.fd, 64) )
		goto err_close;

	l->io.ops = &listener_ops;

	return &l->io;

err_close:
	os_fd_close(l->io.fd);
err_free:
	free(l);
	return NULL;
}

void listener_add(struct iothread *t, struct nbio *io)
{
	nbio_add(t, io, NBIO_READ);
}

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <ashttpd.h>
#include <nbio-connecter.h>
#include <ashttpd-buf.h>
#include <hgang.h>

#ifndef SOCK_NONBLOCK
#define FCNTL_NONBLOCK 1
#define SOCK_NONBLOCK 0
#else
#define FCNTL_NONBLOCK 0
#endif

struct connecter {
	struct nbio io;
	connect_cbfn_t cb;
	void *priv;
};

static hgang_t connecters;

static void __attribute__((constructor)) connecter_ctor(void)
{
	connecters = hgang_new(sizeof(struct connecter), 0);
}

static void connect_write(struct iothread *t, struct nbio *io)
{
	struct connecter *c = (struct connecter *)io;
	int err;
	socklen_t len = sizeof(err);

	if ( getsockopt(c->io.fd, SOL_SOCKET, SO_ERROR, &err, &len) ) {
		fprintf(stderr, "connecter: getsockopt: %s\n", os_err());
		goto barf;
	}

	if ( len != sizeof(err) ) {
		fprintf(stderr, "connecter: getsockopt: %u != %zu\n",
			len, sizeof(err));
		goto barf;
	}

	if ( err ) {
		errno = err;
		fprintf(stderr, "connecter: deferred connect: %s\n", os_err());
		goto barf;
	}

	(*c->cb)(t, c->io.fd, c->priv);
	c->io.fd = -1;
	nbio_del(t, &c->io);
	return;
barf:
	close(c->io.fd);
	c->io.fd = -1;
	nbio_del(t, &c->io);
}

static void connect_dtor(struct iothread *t, struct nbio *io)
{
	struct connecter *c = (struct connecter *)io;
	if ( c->io.fd >= 0 )
		close(c->io.fd);
	hgang_return(connecters, c);
}

static const struct nbio_ops c_ops = {
	.write = connect_write,
	.dtor = connect_dtor,
};

int connecter(struct iothread *t, int type, int proto,
				uint32_t addr, uint16_t port,
				connect_cbfn_t cb, void *priv)
{
	struct sockaddr_in sa;
	struct connecter *c;
	int s;


	s = socket(PF_INET, type | SOCK_NONBLOCK, proto);
	if ( s < 0 ) {
		fprintf(stderr, "connecter: socket: %s\n", os_err());
		return 0;
	}

#if FCNTL_NONBLOCK
	if ( !fd_block(s, 0) ) {
		fd_close(s);
		return 0;
	}
#endif

	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = addr;
	sa.sin_port = htons(port);
	if ( !connect(s, (struct sockaddr *)&sa, sizeof(sa)) ) {
		(*cb)(t, s, priv);
	}

	if ( errno != EINPROGRESS ) {
		fprintf(stderr, "connecter: connect: %s\n", os_err());
		return 0;
	}

	c = hgang_alloc0(connecters);
	if ( NULL == c )
		return 0;

	c->io.fd = s;
	c->io.ops = &c_ops;
	c->cb = cb;
	c->priv = priv;
	nbio_add(t, &c->io, NBIO_WRITE);
	nbio_inactive(t, &c->io, NBIO_WRITE);
	return 1;
}

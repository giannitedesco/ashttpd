#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <httprape.h>
#include <nbio-connecter.h>
#include <ashttpd-buf.h>
#include <hgang.h>

#define MAX_PIPELINE		8

#define CLIENT_RX_HEADER	0
#define CLIENT_RX_DATA		1

struct http_client {
	struct nbio	c_nbio;

	/* -- tx */
	struct iovec	c_req[3 * MAX_PIPELINE];
	unsigned int	c_num_vec;
	unsigned int	c_cur_vec;
	struct mnode	*c_markov;

	/* -- rx */
	struct http_buf	*c_resp;
	unsigned int	c_rx_state;
	size_t		c_rx_clen;
};

static unsigned int target_concurrency;
static unsigned int max_concurrency;
static unsigned int concurrency;
static hgang_t clients;

static struct mnode *markov_step(struct mnode *n)
{
	if ( NULL == n )
		// n == initial;
		;
	// next node
	return NULL;
}

static void abort_client(struct iothread *t, struct http_client *c)
{
	fd_close(c->c_nbio.fd);
	c->c_nbio.fd = -1;
	nbio_del(t, &c->c_nbio);
}

static void advance_vec(struct http_client *c, size_t bytes)
{
}

static void client_read(struct iothread *t, struct nbio *io)
{
	struct http_client *c = (struct http_client *)io;
	// read response
	abort_client(t, c);
}

static void client_write(struct iothread *t, struct nbio *io)
{
	struct http_client *c = (struct http_client *)io;
	// add to vec if necessary
	// writev(...)
	abort_client(t, c);
	//nbio_inactive(t, io);
}

static void client_dtor(struct iothread *t, struct nbio *io)
{
	struct http_client *c = (struct http_client *)io;
	hgang_return(clients, c);
	concurrency--;
}

static const struct nbio_ops client_ops = {
	.read = client_read,
	.write = client_write,
	.dtor = client_dtor,
};

static void handle_connect(struct iothread *t, int s, void *priv)
{
	struct http_client *c;

	c = hgang_alloc0(clients);
	if ( NULL == c ) {
		fprintf(stderr, "OOM on client\n");
		return;
	}

	c->c_nbio.fd = s;
	c->c_nbio.ops = &client_ops;
	c->c_markov = markov_step(NULL);
	nbio_add(t, &c->c_nbio, NBIO_WRITE);
	concurrency++;
}

static uint32_t svr_addr;
static uint16_t svr_port;

static void ramp_up(struct iothread *t)
{
	unsigned int i;

	//printf("Ramping up from %u to %u\n", concurrency, target_concurrency);
	for(i = concurrency; i < target_concurrency; i++) {
		if ( !connecter(t, SOCK_STREAM, IPPROTO_TCP,
			svr_addr, svr_port, handle_connect, NULL) )
			break;
	}

	max_concurrency = i;
	if ( max_concurrency < target_concurrency )
		printf(" - max_concurrency = %u\n", i);
}

int main(int argc, char **argv)
{
	struct iothread iothread;
	struct in_addr in;

	inet_aton("127.0.0.1", &in);
	svr_addr = in.s_addr;
	svr_port = 1234;

	clients = hgang_new(sizeof(struct http_client), 0);
	if ( NULL == clients )
		return EXIT_FAILURE;

	if ( !nbio_init(&iothread, NULL) )
		return EXIT_FAILURE;

	target_concurrency = 1000;

	ramp_up(&iothread);
	do {
		nbio_pump(&iothread, -1);
		ramp_up(&iothread);
	}while ( !list_empty(&iothread.active) );

	nbio_fini(&iothread);
	return EXIT_SUCCESS;
}

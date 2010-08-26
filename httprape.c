#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include <httprape.h>
#include <nbio-connecter.h>
#include <ashttpd-buf.h>
#include <http-parse.h>
#include <http-resp.h>
#include <hgang.h>

static uint32_t svr_addr;
static uint16_t svr_port;
static char *host_addr = "127.0.0.1";

#if 1
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

#define CLIENT_RX_HEADER	0
#define CLIENT_RX_PAGE		1
#define CLIENT_RX_ANCILLARY	2

#define CLIENT_TX_THINK		0
#define CLIENT_TX_PAGE_IMPRESS	1
#define CLIENT_TX_ANCILLARY	2
#define CLIENT_TX_FINAL_FLUSH	3

struct http_client {
	struct nbio	c_nbio;

	unsigned short	c_rx_state;
	unsigned short	c_tx_state;

	const struct mnode	*c_markov;

	/* tx/rx */
	unsigned short	c_inpipe;

	/* -- tx */
	unsigned short	c_tx_total;
	unsigned int	c_tx_sub_idx;
	struct http_buf	*c_tx_buf;

	/* -- rx */
	size_t		c_rx_clen;
	const uint8_t	*c_rx_rptr;
	struct http_buf	*c_rx_buf;
	unsigned char	c_rx_rstate;

	http_ver_t	c_http_proto_ver;
};

static unsigned int target_concurrency;
static unsigned int max_concurrency;
static unsigned int concurrency;
static const unsigned int pipeline_depth = 0;
static hgang_t clients;

#if USE_SYSTEM_RANDOM
static uint32_t get_random_bits(unsigned int bits)
{
	assert(bits <= 32);
	abort(); /* not implemented */
}
#else
#define ASSUME_RAND_MAX_BITS ((sizeof(int) << 3) - 1)
static void __attribute__((constructor)) prng_ctor(void)
{
	assert(RAND_MAX >= (1 << ASSUME_RAND_MAX_BITS));
	srand(0x31337);
}

static uint32_t get_random_bits(unsigned int bits)
{
	static unsigned int cached_bits;
	static int rbits;
	uint32_t ret;

	assert(bits <= ASSUME_RAND_MAX_BITS);

	if ( bits > cached_bits ) {
		ret = (rbits & (cached_bits - 1)) << cached_bits;
		bits -= cached_bits;

		rbits = rand();
		cached_bits = ASSUME_RAND_MAX_BITS;
	}else{
		ret = 0;
	}

	ret |= rbits & ((1 << bits) - 1);
	rbits >>= bits;
	cached_bits -= bits;
	return ret;
}
#endif

static const struct mnode *markov_step(const struct mnode *n)
{
	unsigned int rb, i;
	unsigned int pmin, pmax;

	if ( NULL == n )
		n = markov_root;

	if ( !n->n_num_edges )
		return NULL;

	rb = get_random_bits(n->n_edge_prob_bits);
	dprintf("%u bits of random = %u\n", n->n_edge_prob_bits, rb);
	rb %= n->n_edge_prob_max;
	dprintf("%u clamped to edge prob max (%u)\n", rb, n->n_edge_prob_max);

	for(pmin = i = 0; i < n->n_num_edges; i++) {
		pmax = n->n_edges[i].e_prob_max;
		dprintf(" edge[%u] = <%u, %u>\n", i, pmin, pmax);
		if ( rb >= pmin && rb < pmax )
			return n->n_edges[i].e_node;
		pmin = pmax;
	}

	abort();
}

static void abort_client(struct iothread *t, struct http_client *c)
{
	fd_close(c->c_nbio.fd);
	c->c_nbio.fd = -1;
	nbio_del(t, &c->c_nbio);
}

static void client_read(struct iothread *t, struct nbio *io)
{
	struct http_client *c = (struct http_client *)io;
	uint8_t *wptr;
	ssize_t ret;
	size_t wsz;

	printf("Read response\n");

	if( NULL == c->c_rx_buf ) {
		c->c_rx_buf = buf_alloc_res();
		if ( NULL == c->c_rx_buf ) {
			printf("OOM on res buffer\n");
			abort_client(t, c);
			return;
		}

		c->c_rx_rptr = c->c_rx_buf->b_base;
		c->c_rx_rstate = RSTATE_INITIAL;
	}

	wptr = buf_write(c->c_rx_buf, &wsz);
	if ( 0 == wsz ) {
		printf("OOM on res\n");
		abort_client(t, c);
		return;
	}

	ret = recv(c->c_nbio.fd, wptr, wsz, 0);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &c->c_nbio);
		return;
	}else if ( ret <= 0 ) {
		abort_client(t, c);
		return;
	}

	dprintf("Received %u bytes: %.*s\n",
		ret, ret, wptr);
	buf_done_write(c->c_rx_buf, ret);

	if ( !http_parse_incremental(&c->c_rx_rstate,
					&c->c_rx_rptr,
					c->c_rx_buf->b_write) ) {
		printf("no response yet, waiting for more data\n");
		return;
	}

	/* TODO: Parse header */

	do {
		const uint8_t *rptr;
		size_t rsz;

		rptr = buf_read(c->c_rx_buf, &rsz);
		dprintf("HTTP: %.*s\n", c->c_rx_rptr - rptr, rptr);
		dprintf("DATA: %.*s\n", (rptr + rsz) - c->c_rx_rptr,
			c->c_rx_rptr);
	}while(0);

	c->c_tx_state = CLIENT_TX_THINK;
	nbio_set_wait(t, &c->c_nbio, NBIO_WRITE);
}

static int do_http_req(struct http_client *c, const struct ro_vec *uri)
{
	uint8_t *wptr;
	size_t wsz;
	int len;

	wptr = buf_write(c->c_tx_buf, &wsz);
	if ( NULL == wptr )
		return 0;

	len = snprintf((char *)wptr, wsz,
			"GET %.*s HTTP/1.1\r\n"
			"Host: %s:%u\r\n"
			"Connection: Keep-Alive\r\n"
			"User-Agent: httprape\r\n"
			"\r\n",
			uri->v_len, uri->v_ptr,
			host_addr, svr_port);
	if ( len < 0 )
		len = 0;
	if ( (size_t)len >= wsz )
		return 0;

	buf_done_write(c->c_tx_buf, len);
	return 1;
}

static void client_write(struct iothread *t, struct nbio *io)
{
	struct http_client *c = (struct http_client *)io;
	struct ro_vec *sub_uri;
	const uint8_t *rptr;
	int flags = MSG_NOSIGNAL;
	size_t rsz;
	ssize_t ret;

	switch(c->c_tx_state) {
	case CLIENT_TX_THINK:
		/* fall through */
		c->c_markov = markov_step(c->c_markov);
		if ( NULL == c->c_markov ) {
			printf("%p markov walk ended\n", c);
			goto die;
		}
		printf("%p request: %.*s\n", c,
			c->c_markov->n_uri.v_len,
			c->c_markov->n_uri.v_ptr);
		c->c_tx_total = 0;
		c->c_tx_buf = buf_alloc_req();
		if ( NULL == c->c_tx_buf )
			goto die;
		c->c_tx_state = CLIENT_TX_PAGE_IMPRESS;
		/* fall through */
	case CLIENT_TX_PAGE_IMPRESS:
		if ( !do_http_req(c, &c->c_markov->n_uri) )
			goto die;
		break;
	case CLIENT_TX_ANCILLARY:
		assert(c->c_tx_total <= c->c_markov->n_num_sub + 1);
		sub_uri = &c->c_markov->n_ancillary[c->c_tx_total - 1];
		if ( !do_http_req(c, sub_uri) )
			goto die;
		/* try and fill output buffer with more requests */
		break;
	}

	rptr = buf_read(c->c_tx_buf, &rsz);

	ret = send(c->c_nbio.fd, rptr, rsz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &c->c_nbio);
	}else if ( ret <= 0 )
		goto die;

	/* FIXME: this is fucked, see switch statement above */
	rsz = buf_done_read(c->c_tx_buf, ret);
	if ( rsz == 0 ) {
		buf_reset(c->c_tx_buf);
		c->c_tx_total++;
		c->c_inpipe++;

		if ( c->c_tx_total == 1 && c->c_markov->n_num_sub )
			c->c_tx_state = CLIENT_TX_PAGE_IMPRESS;

		assert(c->c_tx_total <= c->c_markov->n_num_sub + 1);
		if ( c->c_tx_total == c->c_markov->n_num_sub + 1 &&
				c->c_inpipe ) {
			/* unset write flag until responses are handled */
			nbio_set_wait(t, &c->c_nbio, NBIO_READ);
			c->c_tx_state = CLIENT_TX_THINK;
		}
	}

	return;
die:
	abort_client(t, c);
	return;
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
	nbio_add(t, &c->c_nbio, NBIO_WRITE);
	concurrency++;
}

static void ramp_up(struct iothread *t)
{
	unsigned int i;

	dprintf("Ramping up from %u to %u\n", concurrency, target_concurrency);
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

	svr_port = 80;

	if ( argc > 1 )
		host_addr = argv[1];
	if ( argc > 2 )
		svr_port = atoi(argv[2]);

	printf("Connecting to %s:%u\n", host_addr, svr_port);
	inet_aton(host_addr, &in);
	svr_addr = in.s_addr;

	clients = hgang_new(sizeof(struct http_client), 0);
	if ( NULL == clients )
		return EXIT_FAILURE;

	if ( !nbio_init(&iothread, NULL) )
		return EXIT_FAILURE;

	target_concurrency = 1;

	ramp_up(&iothread);
	do {
		nbio_pump(&iothread, -1);
		//ramp_up(&iothread);
	}while ( !list_empty(&iothread.active) );

	nbio_fini(&iothread);
	return EXIT_SUCCESS;
}

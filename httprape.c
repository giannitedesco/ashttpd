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

static unsigned int target_concurrency;
static unsigned int max_concurrency;
static unsigned int concurrency;

static void handle_connect(struct iothread *t, int s, void *priv)
{
	printf("Connection established\n");
	concurrency++;
	//fd_close(s);
}

static void ramp_up(struct iothread *t)
{
	unsigned int i;
	//struct in_addr in;
	//inet_aton("127.0.0.1", &in);
	//in.s_addr

	printf("Ramping up from %u to %u\n", concurrency, target_concurrency);
	for(i = concurrency; i < target_concurrency; i++) {
		if ( !connecter(t, SOCK_STREAM, IPPROTO_TCP,
			htonl(0x7f000001), 1234, handle_connect, NULL) )
			break;
	}

	max_concurrency = i;
	printf(" - max_concurrency = %u\n", i);
}

int main(int argc, char **argv)
{
	struct iothread iothread;

	if ( !nbio_init(&iothread, NULL) )
		return EXIT_FAILURE;

	target_concurrency = 1000;
	ramp_up(&iothread);

	do {
		nbio_pump(&iothread, -1);
	}while ( !list_empty(&iothread.active) );
	nbio_fini(&iothread);
	return EXIT_SUCCESS;
}

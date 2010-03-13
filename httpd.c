#include <ashttpd.h>
#include <nbio-listener.h>

int main(int argc, char **argv)
{
	struct iothread iothread;
	struct nbio *io;

	if ( !nbio_init(&iothread, NULL) )
		return EXIT_FAILURE;

	io = listener_inet(SOCK_STREAM, IPPROTO_TCP, 0, 1234, http_conn, NULL);
	listener_add(&iothread, io);

	do {
		nbio_pump(&iothread, -1);
	}while ( !list_empty(&iothread.active) );

	nbio_fini(&iothread);

	return EXIT_SUCCESS;
}

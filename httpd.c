#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <ashttpd.h>
#include <ashttpd-conn.h>
#include <ashttpd-buf.h>
#include <ashttpd-fio.h>
#include <http-parse.h>
#include <http-req.h>
#include <nbio-inotify.h>
#include <normalize.h>
#include <hgang.h>
#include <critbit.h>

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

static LIST_HEAD(listeners);

static struct http_fio *io_model(const char *name)
{
	unsigned int i;
	static const struct {
		char * const n;
		struct http_fio *fio;
	}ionames[]={
		/* traditional pread based */
		{"sync", &fio_sync},

		/* sendfile: regular synchronous version */
		{"sendfile", &fio_sendfile},

		/* Kernel AIO on regular file, not currently
		 * supported in linux 2.6 so falls back to synchronous
		 */
		{"aio", &fio_async},
		{"async", &fio_async},

		/* Kernel AIO on O_DIRECT file descriptor, re-implementing
		 * page cache in userspace fucking alice in wonderland
		 */
#if 0
		{"dasync", &fio_dasync},
		{"direct", &fio_dasync},
		{"dio", &fio_dasync},
#endif

		/* Kernel based AIO / sendfile utilizing kernel pipe
		 * buffers and splicing... experimental
		 */
#if HAVE_AIO_SENDFILE
		{"aio-sendfile", &fio_async_sendfile},
		{"async-sendfile", &fio_async_sendfile},
#endif
	};

	if ( NULL == name )
		return &fio_sync;

	for(i = 0; i < sizeof(ionames)/sizeof(*ionames); i++) {
		if ( !strcmp(name, ionames[i].n) )
			return ionames[i].fio;
	}

	printf("%s not found!\n", name);
	return &fio_sync;
}

static struct http_listener *http_listen(struct iothread *t,
					uint32_t addr, uint16_t port,
					vhosts_t vhosts)
{
	struct http_listener *hl;

	hl = calloc(1, sizeof(*hl));
	if ( NULL == hl )
		goto out;

	hl->l_listen = listener_inet(t, SOCK_STREAM, IPPROTO_TCP,
					addr, port, http_conn,
					hl, http_oom);
	if ( NULL == hl->l_listen )
		goto out_free;

	hl->l_vhosts = vhosts;
	list_add_tail(&hl->l_list, &listeners);
	printf("http: Listening on %s:%d\n",
		inet_ntoa((struct in_addr){addr}), port);

	goto out; /* success */

out_free:
	free(hl);
	hl = NULL;
out:
	return hl;
}

int main(int argc, char **argv)
{
	const char *vhosts_dir;
	struct iothread iothread;
	vhosts_t vhosts;

	vhosts_dir = (argc > 1) ? argv[1] : "./vhosts";
	fio_current = io_model((argc > 2) ? argv[2] : NULL);

	printf("data: %s model\n", fio_current->label);
	printf("webroot: %s\n", vhosts_dir);

	if ( !nbio_init(&iothread, NULL) )
		return EXIT_FAILURE;

	if ( !http_proto_init(&iothread) ) {
		return EXIT_FAILURE;
	}

	vhosts = vhosts_new(&iothread, vhosts_dir);
	if ( NULL == vhosts )
		return EXIT_FAILURE;

	http_listen(&iothread, 0, 80, vhosts);
	http_listen(&iothread, 0, 1234, vhosts);

	do {
		nbio_pump(&iothread, -1);
	}while ( !list_empty(&iothread.active) );

	nbio_fini(&iothread);
	//vhosts_free(vhosts);

	return EXIT_SUCCESS;
}

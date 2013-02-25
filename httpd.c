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
					webroot_t root)
{
	struct http_listener *hl;

	hl = calloc(1, sizeof(*hl));
	if ( NULL == hl )
		goto out;

	hl->l_listen = listener_inet(t, SOCK_STREAM, IPPROTO_TCP,
					addr, port, http_conn, hl, http_oom);
	if ( NULL == hl->l_listen )
		goto out_free;

	hl->l_webroot = root;
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

static struct cb_tree vhosts;
static const char *vhosts_dirname;
static webroot_t vdefault;

static void vhost_add(void *priv, const char *name, unsigned isdir)
{
	char fn[strlen(vhosts_dirname) + strlen(name) + 2];
	void **pptr;
	webroot_t w;

	if ( isdir )
		return;

	snprintf(fn, sizeof(fn), "%s/%s", vhosts_dirname, name);
	printf("add vhost: %s -> %s\n", name, fn);

	w = webroot_open(fn);
	if ( NULL == w )
		return;

	if ( !strcmp(name, "__default__") ) {
		webroot_close(vdefault);
		vdefault = w;
		return;
	}

	if ( !cb_insert(&vhosts, name, &pptr) )
		return;

	if ( *pptr ) {
		printf(" - closing old\n");
		webroot_close(*pptr);
	}

	*pptr = w;
}

static void vhost_del(void *priv, const char *name, unsigned isdir)
{
	webroot_t w;

	if ( isdir )
		return;
	printf("del vhost: %s\n", name);

	if ( !strcmp(name, "__default__") ) {
		webroot_close(vdefault);
		vdefault = NULL;
		return;
	}

	if ( !cb_delete(&vhosts, name, (void **)&w) ) {
		printf(" - not found\n");
	}

	webroot_close(w);
}

static void server_quit(void *priv)
{
	printf("quit now..\n");
}

static const struct watch_ops vhost_ops = {
	.create = vhost_add,
	.moved_to = vhost_add,
	.delete = vhost_del,
	.moved_from = vhost_del,
	.move_self = server_quit,
	.delete_self = server_quit,
};

static int setup_vhosts(struct iothread *t, const char *dirname)
{
	struct dirent *de;
	nbnotify_t n;
	DIR *dir;

	vhosts_dirname = dirname;

	dir = opendir(dirname);
	if ( NULL == dir ) {
		fprintf(stderr, "opendir: %s: %s\n", dirname, os_err());
		return 0;
	}

	while ( (de = readdir(dir)) ) {
#if defined(_DIRENT_HAVE_D_TYPE)
		if ( de->d_type == DT_DIR )
			continue;
		/* if you put FIFO's, sockets, or dev nodes in here
		 * then what do you expect to happen?
		*/
#endif
		vhost_add(NULL, de->d_name, 0);
	}

	closedir(dir);

	n = nbio_inotify_new(t);
	if ( NULL == n )
		return 0;

	if ( !nbio_inotify_watch_dir(n, dirname, &vhost_ops, NULL) )
		return 0;

	return 1;
}

int main(int argc, char **argv)
{
	static webroot_t webroot;
	const char * webroot_fn;
	struct iothread iothread;

	webroot_fn = (argc > 1) ? argv[1] : "webroot.objdb";
	fio_current = io_model((argc > 2) ? argv[2] : NULL);

	printf("data: %s model\n", fio_current->label);
	printf("webroot: %s\n", webroot_fn);

	if ( !nbio_init(&iothread, NULL) )
		return EXIT_FAILURE;

	if ( !http_proto_init(&iothread) ) {
		return EXIT_FAILURE;
	}

	if ( !setup_vhosts(&iothread, "./vhosts") )
		return EXIT_FAILURE;

	webroot = webroot_open(webroot_fn);
	if ( NULL == webroot ) {
		fprintf(stderr, "webroot: %s: %s\n", webroot_fn, os_err());
		return EXIT_FAILURE;
	}

	http_listen(&iothread, 0, 80, webroot);
	http_listen(&iothread, 0, 1234, webroot);

	do {
		nbio_pump(&iothread, -1);
	}while ( !list_empty(&iothread.active) );

	nbio_fini(&iothread);
	webroot_close(webroot);

	return EXIT_SUCCESS;
}

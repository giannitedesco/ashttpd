#include <ashttpd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <errno.h>
#define __USE_GNU
#include <fcntl.h>
#include <nbio-eventfd.h>
#include "../libaio/src/libaio.h"
#include <rbtree.h>
#include <hgang.h>

/* ugly ugly ugly */
#define PAGE_SHIFT	12U
#define PAGE_SIZE	(1U<<12U)
#define PAGE_MASK	(PAGE_SIZE - 1U)

#define EXTRA_SHIFT	1U

#define CHUNK_SHIFT	(PAGE_SHIFT + EXTRA_SHIFT)
#define CHUNK_SIZE	(1U << CHUNK_SHIFT)
#define CHUNK_MASK	(CHUNK_SIZE - 1U)

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

#define AIO_QUEUE_SIZE		4096
static io_context_t aio_ctx;
static hgang_t aio_iocbs;
static struct nbio *efd;
static unsigned in_flight;

/* A cache entry can be in one of three states:
 * - Free: on the freelist
 * - Pending: in the page tree with a zero ref count
 * - Allocated: in the page tree with a non-zero ref count
 *
 * Only cache items with a ref count of 1 appear in the lru list,
 * an empty freelist and empty lru list means no more buffer space.
 */
struct cache {
	struct rb_node		c_rbt;
	union {
		struct list_head	cu_free;
		struct diocb		*cu_pending;
		struct list_head	cu_lru;
	}c_u;
	size_t			c_num;
	int			c_ref;
};

struct diocb {
	struct iocb		iocb;
	struct list_head	waitq;
	// struct list_head	list; // for cancellations
};

static struct cache *cache_chunks;
static LIST_HEAD(lru);
static LIST_HEAD(freelist);
struct rbtree cache;
static uint8_t *cache_base;
static unsigned cache_nr_chunks;
static off_t off_end;

static int get_lock_limit(rlim_t *ll)
{
	struct rlimit rlim;

	if ( getrlimit(RLIMIT_MEMLOCK, &rlim) ) {
		fprintf(stderr, "dio: getrlimit: %s\n", os_err());
		return 0;
	}

	if ( rlim.rlim_cur < rlim.rlim_max ) {
		rlim.rlim_cur = rlim.rlim_max;
		if ( setrlimit(RLIMIT_MEMLOCK, &rlim) ) {
			fprintf(stderr, "dio: setrlimit: %s\n", os_err());
			return 0;
		}
	}

	*ll = rlim.rlim_cur;
	return 1;
}

static int init_cache(int fd)
{
	unsigned int i;
	struct stat st;
	rlim_t lock_limit;
	size_t lock_pages;
	size_t sz;

	if ( fstat(fd, &st) ) {
		fprintf(stderr, "dio: stat: %s\n", os_err());
		return 0;
	}

	off_end = st.st_size;

	if ( !get_lock_limit(&lock_limit) )
		return 0;

	cache_nr_chunks = (st.st_size + PAGE_MASK) >> PAGE_SHIFT;
	lock_pages = (size_t)lock_limit >> PAGE_SHIFT;
	printf("dio: webroot: %llx bytes, %u pages required, %u lockable\n",
		st.st_size, cache_nr_chunks, lock_pages);

	if ( lock_pages < cache_nr_chunks ) {
		sz = lock_pages << PAGE_SHIFT;
	}else{
		sz = st.st_size;
	}
	cache_base = mmap(NULL, sz, PROT_READ|PROT_WRITE,
				MAP_PRIVATE|MAP_ANONYMOUS|
				MAP_POPULATE|MAP_NORESERVE, -1, 0);
	if ( cache_base == MAP_FAILED ) {
		fprintf(stderr, "dio: mmap: %s\n", os_err());
		return 0;
	}
	printf("Allocated %u bytes in %u pages\n",
		sz, (sz + PAGE_MASK) >> PAGE_SHIFT);

	cache_nr_chunks = (sz + CHUNK_MASK) >> CHUNK_SHIFT;
	printf("dio: chunks: %u bytes, %u chunks required\n",
		CHUNK_SIZE, cache_nr_chunks);

	if ( mlock(cache_base, sz) ) {
		fprintf(stderr, "dio: mlock: %s\n", os_err());
		return 0;
	}
	printf("dio: it's all locked in baby\n");

	cache_chunks = calloc(cache_nr_chunks, sizeof(*cache_chunks));
	if ( NULL == cache_chunks ) {
		fprintf(stderr, "dio: calloc: %s\n", os_err());
		return 0;
	}

	for(i = 0; i < cache_nr_chunks; i++)
		list_add_tail(&cache_chunks[i].c_u.cu_free, &freelist);
	printf("dio: %u cache chunk descriptors on free list\n", i);
	return 1;
}

static size_t chunk_num(struct http_conn *h)
{
	return (size_t)(h->h_data_off >> CHUNK_SHIFT);
}

static int cache_lookup(struct iothread *t, struct http_conn *h)
{
	size_t idx;

	idx = chunk_num(h);
	dprintf("dio: cache_lookup: data_off=%llx chunk_off=%llx idx=%u\n",
		h->h_data_off, (off_t)idx << CHUNK_SHIFT, idx);

	dprintf("TODO: lookup\n");

	return 0;
}

static uint8_t *cache2ptr(struct cache *c)
{
	size_t idx;
	assert(c >= cache_chunks);
	assert(c < cache_chunks + cache_nr_chunks);
	idx = c - cache_chunks;
	return cache_base + (idx << CHUNK_SHIFT);
}

static struct cache *ptr2cache(uint8_t *ptr)
{
	size_t idx;
	assert(ptr >= cache_base);
	assert(ptr <= cache_base + off_end);
	idx = (ptr - cache_base) >> CHUNK_SHIFT;
	return cache_chunks + idx;
}

static void cache_free(struct cache *c)
{
	assert(c->c_ref == 0);
	list_add(&c->c_u.cu_free, &freelist);
	dprintf("dio: cache to freelist: %p\n", c);
}

static void new_refcnt(struct cache *c)
{
	switch(c->c_ref) {
	case 2:
		dprintf("dio: cache from lru: %p\n", c);
		list_del(&c->c_u.cu_lru);
		break;
	case 1:
		dprintf("dio: cache to lru: %p\n", c);
		list_add_tail(&c->c_u.cu_lru, &lru);
		break;
	case 0:
		dprintf("TODO: Remove from tree\n");
		list_del(&c->c_u.cu_lru);
		cache_free(c);
		break;
	default:
		break;
	}
}

static void cache_get(struct cache *c)
{
	if ( c->c_ref == 0 )
		INIT_LIST_HEAD(&c->c_u.cu_lru);
	c->c_ref++;
	assert(c->c_ref);
	new_refcnt(c);
}

static void cache_put(struct cache *c)
{
	assert(c->c_ref);
	--c->c_ref;
	new_refcnt(c);
}

static void buf_put(struct http_buf *buf)
{
	struct cache *c;
	c = ptr2cache(buf->b_base);
	cache_put(c);
	buf_free_naked(buf);
}

static void lru_reaper(void)
{
	struct cache *c, *tmp;

	if ( list_empty(&lru) )
		return;
	
	list_for_each_entry_safe(c, tmp, &lru, c_u.cu_lru) {
		assert(c->c_ref == 1);
		cache_put(c);
		dprintf("dio: reaped %p\n", c);
	}
}

static struct cache *cache_alloc(size_t key)
{
	struct cache *c;

	if ( list_empty(&freelist) ) {
		lru_reaper();
		if ( list_empty(&freelist) )
			return NULL;
	}

	c = list_entry(freelist.next, struct cache, c_u.cu_free);
	list_del(&c->c_u.cu_free);
	c->c_num = key;
	c->c_ref = 0;

	dprintf("TODO: Insert in tree\n");

	dprintf("dio: cache_alloc: %p\n", c);
	return c;
}

static void diocb_prep(struct cache *c, struct iocb *iocb, int fd)
{
	uint8_t *base;
	off_t off;
	size_t sz;

	off = c->c_num << CHUNK_SHIFT;
	base = cache2ptr(c);
	sz = ((off + CHUNK_SIZE) > off_end) ?
		off_end - off : CHUNK_SIZE;

	io_prep_pread(iocb, fd, base, sz, off);
	io_set_eventfd(iocb, efd->fd);
	dprintf("io_submit: pread: %u bytes @ %llx\n", sz, off);
}

static int aio_submit(struct iothread *t, struct http_conn *h, int fd)
{
	struct diocb *iocb;
	struct cache *c;
	int ret;

	dprintf("\n");
	assert(h->h_data_len);

	if ( cache_lookup(t, h) )
		return 1;

	c = cache_alloc(chunk_num(h));
	if ( NULL == c ) {
		dprintf("dio: cache OOM\n");
		return 0;
	}

	iocb = hgang_alloc0(aio_iocbs);
	if ( NULL == iocb ) {
		dprintf("dio: iocb OOM\n");
		cache_free(c);
		return 0;
	}

	c->c_u.cu_pending = iocb;

	INIT_LIST_HEAD(&iocb->waitq);
	nbio_to_waitq(t, &h->h_nbio, &iocb->waitq);

	diocb_prep(c, &iocb->iocb, fd);
	iocb->iocb.data = c;

	ret = io_submit(aio_ctx, 1, (struct iocb **)&iocb);
	if ( ret <= 0 ) {
		errno = -ret;
		fprintf(stderr, "io_submit: %s\n", os_err());
		return 0;
	}

	in_flight++;
	return 1;
}

static void handle_completion(struct iothread *t, struct diocb *iocb,
				struct cache *c, int ret)
{
	struct http_conn *h, *tmp;
	uint8_t *baseptr;
	off_t c_base;

	hgang_return(aio_iocbs, iocb);

	if ( ret <= 0 ) {
		/* FIXME: loop on wait queue and kill everything */
		errno = -ret;
		fprintf(stderr, "aio_pread: %s\n", os_err());
		hgang_return(aio_iocbs, iocb);
		return;
	}

	/* FIXME: check for truncated reads, can't handle this,
	 * as above, loop on wait queue and kill everything
	 */

	c->c_u.cu_pending = NULL;
	cache_get(c);

	c_base = c->c_num << CHUNK_SHIFT;
	baseptr = cache2ptr(c);

	list_for_each_entry_safe(h, tmp, &iocb->waitq, h_nbio.list) {
		struct http_buf *buf;
		size_t ofs, sz;

		buf = buf_alloc_naked();
		if ( NULL == buf ) {
			nbio_del(t, &h->h_nbio);
			continue;
		}

		/* set up buffer */
		assert(h->h_data_off >= c_base);
		ofs = h->h_data_off - c_base;
		sz = ((ofs + h->h_data_len) < CHUNK_SIZE) ?
			h->h_data_len : CHUNK_SIZE - ofs;

		buf->b_read = buf->b_base = baseptr + ofs;
		buf->b_end = buf->b_write = baseptr + ofs + sz;

		h->h_dat = buf;
		cache_get(c);
		dprintf("buffer: %u bytes at ofs %x\n", sz, ofs);
		nbio_wake(t, &h->h_nbio, NBIO_WRITE);
	}

	hgang_return(aio_iocbs, iocb);
}

static void aio_event(struct iothread *t, void *priv, eventfd_t val)
{
	struct io_event ev[in_flight];
	struct timespec tmo;
	int ret, i;

	memset(&tmo, 0, sizeof(tmo));

	dprintf("aio_event ready, %llx/%u in flight\n", val, in_flight);

	ret = io_getevents(aio_ctx, 1, in_flight, ev, &tmo);
	if ( ret < 0 ) {
		errno = -ret;
		fprintf(stderr, "io_getevents: %s\n", os_err());
		return;
	}

	in_flight -= ret;
	dprintf("got %u events, %u still in flight\n", ret, in_flight);

	for(i = 0; i < ret; i++)
		handle_completion(t, (struct diocb *)ev[i].obj,
					ev[i].data, ev[i].res);
}

static int io_dasync_init(struct iothread *t, int webroot_fd)
{
	if ( !init_cache(webroot_fd) )
		return 0;

	memset(&aio_ctx, 0, sizeof(aio_ctx));
	if ( io_queue_init(AIO_QUEUE_SIZE, &aio_ctx) ) {
		fprintf(stderr, "io_queue_init: %s\n", os_err());
		return 0;
	}

	aio_iocbs = hgang_new(sizeof(struct diocb), 0);
	if ( NULL == aio_iocbs )
		return 0;

	efd = nbio_eventfd_new(0, aio_event, NULL);
	if ( NULL == efd )
		return 0;
	nbio_eventfd_add(t, efd);

	return 1;
}

static int webroot_dio_fd(const char *fn)
{
	return open(fn, O_RDONLY|O_DIRECT);
}

static int io_dasync_write(struct iothread *t, struct http_conn *h, int fd)
{
	int flags = MSG_NOSIGNAL;
	const uint8_t *ptr;
	ssize_t ret;
	size_t sz;

	dprintf("\n");
	ptr = buf_read(h->h_dat, &sz);

	if ( h->h_data_len )
		flags |= MSG_MORE;

	ret = send(h->h_nbio.fd, ptr, sz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		nbio_inactive(t, &h->h_nbio);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}

	h->h_data_off += (size_t)ret;
	h->h_data_len -= (size_t)ret;

	dprintf("Transmitted %u\n", (size_t)ret);
	buf_done_read(h->h_dat, ret);
	buf_read(h->h_dat, &sz);

	if ( sz ) {
		dprintf("Partial transmit: %u bytes left\n", sz);
		return 1;
	}

	buf_put(h->h_dat);
	h->h_dat = NULL;

	if ( h->h_data_len ) {
		dprintf("Submit more, %u bytes left\n", h->h_data_len);
		return aio_submit(t, h, fd);
	}

	nbio_set_wait(t, &h->h_nbio, NBIO_READ);
	h->h_state = HTTP_CONN_REQUEST;
	dprintf("DONE\n");

	return 1;
}

static int io_dasync_prep(struct iothread *t, struct http_conn *h, int fd)
{
	return aio_submit(t, h, fd);
}

static void io_dasync_abort(struct http_conn *h)
{
	buf_free_naked(h->h_dat);
}

static void io_dasync_fini(struct iothread *t)
{
	/* fuck it */
}

struct http_fio fio_dasync = {
	.label = "O_DIRECT KAIO",
	.prep = io_dasync_prep,
	.write = io_dasync_write,
	.abort = io_dasync_abort,
	.webroot_fd = webroot_dio_fd,
	.init = io_dasync_init,
	.fini = io_dasync_fini,
};

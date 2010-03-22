#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <errno.h>
#include "../libaio/src/libaio.h"
#define __USE_GNU /* O_DIRECT */
#include <fcntl.h>
#undef __USE_GNU

#include <ashttpd.h>
#include <ashttpd-conn.h>
#include <ashttpd-buf.h>
#include <ashttpd-fio.h>
#include <nbio-eventfd.h>
#include <rbtree.h>
#include <hgang.h>

#define PAGE_SHIFT	12U
#define PAGE_SIZE	(1U<<12U)
#define PAGE_MASK	(PAGE_SIZE - 1U)

#define EXTRA_SHIFT	1U

#define CHUNK_SHIFT	(PAGE_SHIFT + EXTRA_SHIFT)
#define CHUNK_SIZE	(1U << CHUNK_SHIFT)
#define CHUNK_MASK	(CHUNK_SIZE - 1U)

#define IO_STATE_IDLE		0
#define IO_STATE_PENDING	1
#define IO_STATE_COMPLETED	2

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
static struct rbtree cache;
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

	if ( rlim.rlim_cur == RLIM_INFINITY ) {
		*ll = rlim.rlim_cur;
		return 1;
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
	if ( lock_limit == RLIM_INFINITY )
		lock_pages = cache_nr_chunks << PAGE_SHIFT;
	else
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
	printf("dio: %u x %u byte cache chunk descriptors on free list\n",
		cache_nr_chunks, sizeof(struct cache));
	return 1;
}

static size_t chunk_num(off_t off)
{
	assert(off < off_end);
	return (size_t)(off >> CHUNK_SHIFT);
}

static uint8_t *cache2ptr(struct cache *c)
{
	size_t idx;
	assert(c >= cache_chunks);
	assert(c < cache_chunks + cache_nr_chunks);
	idx = c - cache_chunks;
	assert(idx < cache_nr_chunks);
	return cache_base + (idx << CHUNK_SHIFT);
}

static struct cache *ptr2cache(uint8_t *ptr)
{
	size_t idx;
	assert(ptr >= cache_base);
	assert(ptr <= cache_base + off_end);
	idx = (ptr - cache_base) >> CHUNK_SHIFT;
	assert(idx < cache_nr_chunks);
	return cache_chunks + idx;
}

static void cache_free(struct cache *c)
{
	dprintf("dio: cache to freelist: %p\n", c);
	assert(c->c_ref == 0);
	/* rbtree_delete_rebalance() is b0rk? */
	rbtree_delete_node(&cache, &c->c_rbt);
	memset(c, 0, sizeof(*c));
	list_add(&c->c_u.cu_free, &freelist);
}

static void cache_get(struct cache *c)
{
	c->c_ref++;
	assert(c->c_ref);
	switch(c->c_ref) {
	case 0:
		break;
	case 1:
		dprintf("dio: cache to lru: %p\n", c);
		list_add_tail(&c->c_u.cu_lru, &lru);
		break;
	case 2:
		dprintf("dio: cache from lru: %p\n", c);
		list_del(&c->c_u.cu_lru);
		break;
	default:
		dprintf("dio: cache_get: %p %u\n", c, c->c_ref);
		break;
	}
}

static void cache_put(struct cache *c)
{
	assert(c->c_ref);
	--c->c_ref;
	switch(c->c_ref) {
	case 1:
		dprintf("dio: cache to lru: %p\n", c);
		list_add_tail(&c->c_u.cu_lru, &lru);
		break;
	case 0:
		dprintf("dio: cache from lru: %p\n", c);
		list_del(&c->c_u.cu_lru);
		cache_free(c);
		break;
	default:
		dprintf("dio: cache_put: %p %u\n", c, c->c_ref);
		break;
	}
}

static struct http_buf *cache2buf(struct cache *c, http_conn_t h)
{
	uint8_t *baseptr;
	off_t c_base, data_len;
	struct http_buf *buf;
	off_t data_off;
	size_t ofs, sz;

	buf = buf_alloc_naked();
	if ( NULL == buf )
		return NULL;

	data_len = http_conn_data(h, NULL, &data_off);
	assert(chunk_num(data_off) == c->c_num);
	assert(data_len);

	c_base = c->c_num << CHUNK_SHIFT;
	baseptr = cache2ptr(c);

	assert(data_off >= c_base);
	ofs = data_off - c_base;
	assert(ofs < CHUNK_SIZE);
	sz = ((ofs + data_len) < CHUNK_SIZE) ? data_len : (CHUNK_SIZE - ofs);
	assert(ofs + sz <= CHUNK_SIZE);

	buf->b_read = buf->b_base = baseptr + ofs;
	buf->b_end = buf->b_write = baseptr + ofs + sz;

	cache_get(c);
	dprintf("buffer: %u bytes at ofs %x\n", sz, ofs);
	return buf;
}

static int cache_lookup(struct iothread *t, http_conn_t h)
{
	struct cache *c;
	off_t data_off;
	size_t key;

	key = http_conn_data(h, NULL, &data_off);
	assert(key);

	key = chunk_num(data_off);
	dprintf("dio: cache_lookup: data_off=%llx chunk_off=%llx key=%u\n",
		data_off, (off_t)key << CHUNK_SHIFT, key);

	for(c = (struct cache *)cache.rbt_root; c; ) {
		if ( key == c->c_num )
			break;
		if ( key < c->c_num ) {
			c = (struct cache *)c->c_rbt.rb_child[CHILD_LEFT];
		}else{
			c = (struct cache *)c->c_rbt.rb_child[CHILD_RIGHT];
		}
	}

	if ( NULL == c ) {
		dprintf("cache miss\n");
		return 0;
	}

	assert(c->c_num == key);
	if ( c->c_ref ) {
		struct http_buf *buf;
		dprintf("cache hit\n");
		buf = cache2buf(c, h);
		if ( NULL == buf ) {
			http_conn_abort(t, h);
			return 1;
		}
		http_conn_set_priv(h, buf, IO_STATE_COMPLETED);
		return 1;
	}

	dprintf("cache hit (pending)\n");
	http_conn_set_priv(h, c, IO_STATE_PENDING);
	http_conn_to_waitq(t, h, &c->c_u.cu_pending->waitq);

	return 1;
}

static void buf_put(struct http_buf *buf)
{
	struct cache *c;
	if ( NULL == buf )
		return;
	c = ptr2cache(buf->b_base);
	cache_put(c);
	buf_free_naked(buf);
}

static void lru_reaper(void)
{
	struct cache *c, *tmp;

	list_for_each_entry_safe(c, tmp, &lru, c_u.cu_lru) {
		dprintf("dio: reaping %p\n", c);
		assert(c->c_ref == 1);
		cache_put(c);
		/* lets not kill our cache in one fell swoop */
		break;
	}
}

static void cache_insert(struct cache *c)
{
	struct rb_node *n, *parent, **p;

	for(p = &cache.rbt_root, n = parent = NULL; *p; ) {
		struct cache *node;

		parent = *p;

		node = (struct cache *)parent;
		assert(node->c_num != c->c_num);
		if ( c->c_num < node->c_num ) {
			p = &(*p)->rb_child[CHILD_LEFT];
		}else{
			p = &(*p)->rb_child[CHILD_RIGHT];
		}
	}

	c->c_rbt.rb_parent = parent;
	*p = &c->c_rbt;
	rbtree_insert_rebalance(&cache, &c->c_rbt);
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
	dprintf("dio: cache_alloc: %p\n", c);
	list_del(&c->c_u.cu_free);
	memset(c, 0, sizeof(*c));
	c->c_num = key;

	cache_insert(c);

	return c;
}

static void diocb_prep(struct cache *c, struct iocb *iocb, int fd)
{
	uint8_t *base;
	off_t off;
	size_t sz;

	off = c->c_num << CHUNK_SHIFT;
	base = cache2ptr(c);
	sz = ((off + CHUNK_SIZE) > off_end) ? (off_end - off) : CHUNK_SIZE;

	io_prep_pread(iocb, fd, base, sz, off);
	io_set_eventfd(iocb, efd->fd);
	dprintf("io_submit: pread: %u bytes @ %llx\n", sz, off);
}

static void wake_diediedie(struct iothread *t, http_conn_t h)
{
	http_conn_set_priv(h, NULL, IO_STATE_IDLE);
	http_conn_abort(t, h);
}

static int aio_submit(struct iothread *t, http_conn_t h)
{
	unsigned short io_state;
	struct diocb *iocb;
	struct cache *c;
	size_t data_len;
	off_t data_off;
	int ret, fd;

	dprintf("\n");
	data_len = http_conn_data(h, &fd, &data_off);
	assert(data_len);

	if ( cache_lookup(t, h) )
		return 1;

	c = cache_alloc(chunk_num(data_off));
	if ( NULL == c ) {
		printf("dio: cache OOM\n");
		return 0;
	}

	iocb = hgang_alloc(aio_iocbs);
	if ( NULL == iocb ) {
		printf("dio: iocb OOM\n");
		cache_free(c);
		return 0;
	}

	c->c_u.cu_pending = iocb;

	diocb_prep(c, &iocb->iocb, fd);
	iocb->iocb.data = c;
	INIT_LIST_HEAD(&iocb->waitq);
	http_conn_to_waitq(t, h, &iocb->waitq);

	ret = io_submit(aio_ctx, 1, (struct iocb **)&iocb);
	if ( ret <= 0 ) {
		errno = -ret;
		fprintf(stderr, "io_submit: %s\n", os_err());
		http_conn_wake(t, &iocb->waitq, wake_diediedie);
		cache_free(c);
		hgang_return(aio_iocbs, iocb);
		return 0;
	}

	assert(NULL == http_conn_get_priv(h, &io_state));
	assert(io_state == IO_STATE_IDLE);
	http_conn_set_priv(h, c, IO_STATE_PENDING);
	in_flight++;
	return 1;
}

static void wake_completed(struct iothread *t, http_conn_t h)
{
	struct http_buf *data_buf;
	unsigned short io_state;
	struct cache *c;

	c = http_conn_get_priv(h, &io_state);
	assert(io_state == IO_STATE_PENDING);

	data_buf = cache2buf(c, h);
	if ( NULL == data_buf ) {
		http_conn_set_priv(h, NULL, IO_STATE_IDLE);
		http_conn_abort(t, h);
		return;
	}

	http_conn_set_priv(h, data_buf, IO_STATE_COMPLETED);
}

static void handle_completion(struct iothread *t, struct diocb *iocb,
				struct cache *c, int ret)
{
	assert(c == iocb->iocb.data);

	if ( ret <= 0 ) {
		errno = -ret;
		fprintf(stderr, "aio_pread: %s\n", os_err());
		http_conn_wake(t, &iocb->waitq, wake_diediedie);
		hgang_return(aio_iocbs, iocb);
		return;
	}

	/* FIXME: check for truncated reads, can't handle this,
	 * as above, loop on wait queue and kill everything
	 */

	c->c_u.cu_pending = NULL;
	cache_get(c);

	http_conn_wake(t, &iocb->waitq, wake_completed);
	assert(list_empty(&iocb->waitq));
	hgang_return(aio_iocbs, iocb);
}

static void aio_event(struct iothread *t, void *priv, eventfd_t val)
{
	struct io_event ev[in_flight];
	struct timespec tmo;
	int ret, i;

	/* Spurious eventfd wakeup */
	if ( !in_flight )
		return;

	dprintf("aio_event ready, %llx/%u in flight\n", val, in_flight);

	memset(&tmo, 0, sizeof(tmo));
	ret = io_getevents(aio_ctx, 1, in_flight, ev, &tmo);
	if ( ret < 0 ) {
		errno = -ret;
		fprintf(stderr, "io_getevents: %s\n", os_err());
		return;
	}
	assert((size_t)ret <= in_flight);

	in_flight -= ret;
	dprintf("got %u events, %u still in flight\n", ret, in_flight);

	for(i = 0; i < ret; i++)
		handle_completion(t, (struct diocb *)ev[i].obj,
					ev[i].data, ev[i].res);
}

static int io_dasync_init(struct iothread *t, int webroot_fd)
{
	int ret;

	if ( !init_cache(webroot_fd) )
		return 0;

	memset(&aio_ctx, 0, sizeof(aio_ctx));
	ret = io_queue_init(AIO_QUEUE_SIZE, &aio_ctx);
	if ( ret < 0 ) {
		errno = -ret;
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

static int io_dasync_write(struct iothread *t, http_conn_t h)
{
	int flags = MSG_NOSIGNAL;
	struct http_buf *data_buf;
	const uint8_t *ptr;
	size_t sz, data_len;
	unsigned short io_state;
	ssize_t ret;

	dprintf("\n");
	data_buf = http_conn_get_priv(h, &io_state);
	assert(io_state == IO_STATE_COMPLETED);
	ptr = buf_read(data_buf, &sz);

	if ( http_conn_data(h, NULL, NULL) )
		flags |= MSG_MORE;

	ret = send(http_conn_socket(h), ptr, sz, flags);
	if ( ret < 0 && errno == EAGAIN ) {
		http_conn_inactive(t, h);
		return 1;
	}else if ( ret <= 0 ) {
		return 0;
	}


	dprintf("Transmitted %u\n", (size_t)ret);
	sz = buf_done_read(data_buf, ret);
	if ( sz ) {
		dprintf("Partial transmit: %u bytes left\n", sz);
		return 1;
	}

	buf_put(data_buf);
	http_conn_set_priv(h, NULL, IO_STATE_IDLE);

	data_len = http_conn_data_read(h, ret);
	if ( data_len ) {
		dprintf("Submit more, %u bytes left\n", data_len);
		return aio_submit(t, h);
	}

	http_conn_data_complete(t, h);
	dprintf("DONE\n");

	return 1;
}

static int io_dasync_prep(struct iothread *t, http_conn_t h)
{
	http_conn_set_priv(h, NULL, IO_STATE_IDLE);
	return aio_submit(t, h);
}

static void io_dasync_abort(http_conn_t h)
{
	unsigned short io_state;
	struct http_buf *buf;
	void *priv;

	priv = http_conn_get_priv(h, &io_state);
	switch(io_state) {
	case IO_STATE_IDLE:
		assert(priv == NULL);
		break;
	case IO_STATE_PENDING:
		printf("EEK\n");
		abort();
		break;
	case IO_STATE_COMPLETED:
		buf = priv;
		buf_put(buf);
		break;
	default:
		abort();
	}
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

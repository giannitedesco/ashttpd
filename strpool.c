/*
* This file is part of Firestorm NIDS
* Copyright (c) 2003 Gianni Tedesco
* Released under the terms of the GNU GPL version 2
*/

#include <compiler.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <strpool.h>

/** Gang memory area descriptor.
 * \ingroup g_strpool
*/
struct strpool_slab {
	/** Amount of data remaining in this area. */
	size_t len;
	/** Pointer to the remaining free data. */
	void *ptr;
	/** Mext memory area in the list. */
	struct strpool_slab *next;
};

/** Gang memory allocator.
 * \ingroup g_strpool
*/
struct strpool {
	/** Size to make system memory requests. */
	size_t slab_size;
	/** Head of memory area list. */
	struct strpool_slab *head;
	/** Tail of memory area list. */
	struct strpool_slab *tail;
};

/** Initialize a strpool allocator.
 * \ingroup g_strpool
 * @param m An strpool structure to use
 * @param slab_size default size of system allocation units
 *
 * Creates a new strpool allocator descriptor with the passed values
 * set.
 *
 * @return zero on error, non-zero for success.
 */
strpool_t strpool_new(size_t slab_size)
{
	struct strpool *m;

	m = calloc(1, sizeof(*m));
	if ( NULL == m )
		return NULL;

	if ( slab_size == 0 )
		slab_size = 8192;
	if ( slab_size < sizeof(struct strpool_slab) )
		slab_size = sizeof(struct strpool_slab);

	m->slab_size = slab_size;

	return m;
}

/** Destroy a strpool allocator.
 * \ingroup g_strpool
 * @param m The strpool structure to destroy.
 *
 * Frees all memory allocated by the passed strpool descriptor and
 * resets all members to invalid values.
 */
void strpool_free(strpool_t m)
{
	struct strpool_slab *s;

	if ( m ) {
		for(s = m->head; s;) {
			struct strpool_slab *t = s;
			s = s->next;
			free(t);
		}
		free(m);
	}
}

/** Allocator slow path.
 * \ingroup g_strpool
 * @param m The strpool structure to allocate from.
 * @param sz Object size.
 *
 * Slow path for allocations which may request additional memory
 * from the system.
*/
static void *strpool_alloc_slow(struct strpool *m, size_t sz)
{
	size_t alloc;
	size_t overhead;
	struct strpool_slab *s;
	void *ret;

	overhead = sizeof(struct strpool_slab);

	alloc = overhead + sz;
	if ( alloc < m->slab_size )
		alloc = m->slab_size;

	s = malloc(alloc);
	if ( s == NULL )
		return NULL;

	ret = (void *)s + overhead;
	s->ptr = (void *)s + overhead + sz;
	s->len = alloc - (overhead + sz);
	s->next = NULL;

	if ( m->tail == NULL ) {
		m->head = s;
	}else{
		m->tail->next = s;
	}

	m->tail = s;

	return ret;
}

/** Allocate an object.
 * \ingroup g_strpool
 * @param m The strpool structure to allocate from.
 * @param sz Object size.
 *
 * This is the fast path for allocations. If there is memory
 * available in slabs then no call will be made to the system
 * allocator malloc/brk/mmap/HeapAlloc or whatever.
 *
 * \bug There's no way to specify any alignment requierments.
*/
void *strpool_alloc(strpool_t m, size_t sz)
{
	void *ret;
	void *end;

	if ( unlikely(m->tail == NULL || m->tail->len < sz) ) {
		return strpool_alloc_slow(m, sz);
	}

	ret = m->tail->ptr;
	end = m->tail->ptr + m->tail->len;
	m->tail->ptr += sz;

	if ( m->tail->ptr > end )
		m->tail->len = 0;
	else
		m->tail->len -= sz;

	return ret;
}

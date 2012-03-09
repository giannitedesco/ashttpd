/*
 * This file is part of dotscara
 * Copyright (c) 2003 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
 */
#ifndef _STRPOOL_HEADER_INCLUDED_
#define _STRPOOL_HEADER_INCLUDED_

/* for memset */
#include <string.h>

typedef struct strpool *strpool_t;

strpool_t strpool_new(size_t slab_size);
void strpool_free(strpool_t m);
void * strpool_alloc(strpool_t m, size_t sz);

/** Allocate an object initialized to zero.
 * \ingroup g_strpool
 * @param m Gang allocator object to allocate from.
 * @param sz Size of object to allocate.
 *
 * Inline function which calls strpool_alloc() and zero's the
 * returned memory area.
 *
 * @return pointer to allocated object, or NULL on failure
*/
static inline void *strpool_alloc0(struct strpool *m, size_t sz)
{
	void *ret;

	ret = strpool_alloc(m, sz);
	if ( ret != NULL )
		memset(ret, 0, sz);

	return ret;
}

#endif /* _STRPOOL_HEADER_INCLUDED_ */

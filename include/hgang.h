/*
 * This file is part of dotscara
 * Copyright (c) 2003 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
 */
#ifndef _HGANG_HEADER_INCLUDED_
#define _HGANG_HEADER_INCLUDED_

typedef struct _hgang *hgang_t;

#define HGANG_POISON 		1
#define HGANG_POISON_PATTERN 	0xa5

typedef int(*hgang_cb_t)(void *priv, void *obj);

_private hgang_t hgang_new(size_t obj_size, unsigned slab_size);
_private void hgang_free(hgang_t h);
_private void * hgang_alloc(hgang_t h) _malloc;
_private void *hgang_alloc0(hgang_t h) _malloc;
_private void hgang_return(hgang_t h, void *obj);

_private int hgang_foreach(hgang_t h, hgang_cb_t cb, void *priv);

_private size_t hgang_object_size(hgang_t h);

#endif /* _HGANG_HEADER_INCLUDED_ */

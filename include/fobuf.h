/*
 * This file is part of dotscara
 * Copyright (c) 2003 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
 */
#ifndef _FOBUF_HEADER_INCLUDED_
#define _FOBUF_HEADER_INCLUDED_

/* for memcpy */
#include <string.h>

typedef struct _fobuf *fobuf_t;

_private _check_result fobuf_t fobuf_new(int fd, size_t bufsz);
_private _check_result int fobuf_newfd(fobuf_t b, int fd);
_private _check_result int fobuf_write(fobuf_t b, const void *buf, size_t len);
_private _check_result int fobuf_flush(fobuf_t b);
_private _check_result int fobuf_close(fobuf_t b);
_private void fobuf_abort(fobuf_t b);
_private int fobuf_fd(fobuf_t b);

#endif /* _FOBUF_HEADER_INCLUDED_ */

/*
 * This file is part of dotscara
 * Copyright (c) 2003 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
 */
#ifndef _FIRESTORM_OS_HEADER_INCLUDED_
#define _FIRESTORM_OS_HEADER_INCLUDED_

_private const uint8_t *map_file(int fd, size_t *len);
_private int os_errno(void);
_private const char *os_error(int);
_private const char *os_err(void);
_private const char *os_err2(const char *);

_private int fd_read(int fd, void *buf, size_t *sz, int *eof) _check_result;
_private int fd_pread(int fd, off_t off, void *buf, size_t *sz, int *eof) _check_result;
_private int fd_write(int fd, const void *buf, size_t len) _check_result;
_private int fd_close(int fd);

_private int fd_block(int fd, int b);
_private int fd_coe(int fd, int coe);

_private int os_sigpipe_ignore(void);

#endif /* _FIRESTORM_OS_HEADER_INCLUDED_ */

/*
 * This file is part of dotscara
 * Copyright (c) 2003 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
 */
#ifndef _FIRESTORM_OS_HEADER_INCLUDED_
#define _FIRESTORM_OS_HEADER_INCLUDED_

#include <endian.h>

_private int os_errno(void);
_private const char *os_error(int);
_private const char *os_err(void);
_private const char *os_err2(const char *);
_private int os_socket_nonblock(int s);
_private int os_fd_close(int fd);

#endif /* _FIRESTORM_OS_HEADER_INCLUDED_ */

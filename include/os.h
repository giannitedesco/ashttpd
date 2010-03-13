/*
 * This file is part of dotscara
 * Copyright (c) 2003 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
 */
#ifndef _FIRESTORM_OS_HEADER_INCLUDED_
#define _FIRESTORM_OS_HEADER_INCLUDED_

#include <endian.h>

int os_errno(void);
const char *os_error(int);
const char *os_err(void);
const char *os_err2(const char *);

#endif /* _FIRESTORM_OS_HEADER_INCLUDED_ */

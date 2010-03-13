/*
* This file is part of Firestorm NIDS
* Copyright (c) 2003 Gianni Tedesco
* Released under the terms of the GNU GPL version 2
*/

#include <compiler.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <os.h>

int os_errno(void)
{
	return errno;
}

const char *os_error(int e)
{
	return strerror(e);
}

const char *os_err(void)
{
	return strerror(errno);
}

const char *os_err2(const char *def)
{
	if ( def == NULL )
		def = "Internal Error";
	return (errno ? strerror(errno) : def);
}

int os_socket_nonblock(int s)
{
	int f;

	if ( (f = fcntl(s, F_GETFL, 0)) < 0 )
		return 0;

	if ( fcntl(s, F_SETFL, f|O_NONBLOCK) < 0 )
		return 0;

	return 1;
}

int os_fd_close(int fd)
{
	int ret;

	if ( fd < 0 )
		return 1;
intr:
	ret = close(fd);
	if ( ret && errno == EINTR )
		goto intr;

	return (ret == 0);
}

/*
* This file is part of Firestorm NIDS
* Copyright (c) 2003 Gianni Tedesco
* Released under the terms of the GNU GPL version 2
*/

#include <compiler.h>

#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>

#include <os.h>
#include <fobuf.h>

/** fobuf - File output buffer object */
struct _fobuf {
	/* [ Do not change these fields, they are ABI */
	/** Underlying file descriptor */
	int fd;

	/** @internal Pointer to base of buffer */
	char *buf;
	/** @internal Pointer to current position in buffer */
	char *ptr;
	/** @internal Amount of data remaining in the buffer */
	size_t buf_len;
	/** @internal Total size of the buffer */
	size_t buf_sz;
};

/** _fobuf_flush
 * @param b: the fobuf structure to flush
 *
 * Flush the userspace buffer to disk. Note this does not call fsync()
 * so do not rely on it in order to verify that data is written to disk.
 *
 * Failure modes:
 *  0. return 1: success, all buffered data was written to the kernel
 *  1. undefined: any of the failure modes of fd_write()
 *  2. sig11|sig6|file-corruption: b->buf_len > b->buf_sz
 */
static int _fobuf_flush(struct _fobuf *b)
{
	size_t len = b->buf_sz - b->buf_len;
	const void *buf = b->buf;

	/* buffer empty */
	if ( len == 0 )
		return 1;

	if ( !fd_write(b->fd, buf, len) ) {
		//ERR("fd_write: %s", os_err());
		return 0;
	}

	b->ptr = b->buf;
	b->buf_len = b->buf_sz;

	return 1;
}

int fobuf_flush(fobuf_t b)
{
	return _fobuf_flush(b);
}

/** fobuf_new
 * @param b: a fobuf structure to use
 * @param fd: file descriptor to write to
 * @param bufsz: size of output buffer, 0 is default
 *
 * Attach a file descriptor to a buffer and ready it for use.
 *
 * Failure modes:
 *  0. return 1: success, buffer was allocated, @b is safe to use.
 *  1. return 0: endianness not defined or supported
 *  2. undefined: any of the failure modes of malloc(3)
 */
fobuf_t fobuf_new(int fd, size_t bufsz)
{
	struct _fobuf *b;

	b = malloc(sizeof(*b));
	if ( b ) {
		if ( !bufsz )
			bufsz = 4096;

		b->buf = malloc(bufsz);
		if ( b->buf == 0 )
			return 0;

		b->fd = fd;
		b->ptr = b->buf;
		b->buf_len = bufsz;
		b->buf_sz = bufsz;
		//DEBUG("%p: fd=%i bufsz=%zu", b, b->fd, b->buf_sz);
	}

	return b;
}

/** fobuf_abort()
 * @param b: the fobuf structure to finish up with.
 *
 * Free up any allocated memory and anything in the buffer. Note that this does
 * not close the file descriptor, you must do that yourself.
 *
 * Failure modes:
 *  0. undefined: any of the failure modes of free()
 */
void fobuf_abort(fobuf_t b)
{
	if ( b ) {
		if ( b->buf )
			free(b->buf);
		free(b);
	}
}

/** fobuf_close
 * @param b: the fobuf structure to finish up with.
 *
 * Flush the buffers, ensure data is on disk and close the file descriptor.
 *
 * Return value: zero on error, non-zero on success.
 */
int fobuf_close(fobuf_t b)
{
	int ret=1;

	//DEBUG("%p", b);

	if ( b->fd == -1 )
		goto noclose;

	if ( !_fobuf_flush(b) )
		ret = 0;

	/* don't error if the output file is a special file
	 * which does not support fsync (eg: a pipe)
	 */
	if ( fsync(b->fd) && errno != EROFS && errno != EINVAL ) {
		//ERR("fsync: %s", os_err());
		ret = 0;
	}

	if ( !close(b->fd) ) {
		//ERR("close: %s", os_err());
		ret = 0;
	}

noclose:
	if ( b->buf )
		free(b->buf);
	free(b);

	return ret;
}

/** fobuf_newfd
 * @param b: the fobuf structure to modify
 * @param fd: the new file descriptor
 *
 * Like fobuf_close() except that the buffer and fobuf are not free'd but
 * attached to a new file descriptor.
 *
 * Return value: zero on error, non-zero on success.
 */
int fobuf_newfd(fobuf_t b, int fd)
{
	int ret=1;

	if ( b->fd == -1 )
		goto noflush;

	if ( !_fobuf_flush(b) )
		ret = 0;

	/* don't error if the output file is a special file
	 * which does not support fsync (eg: a pipe)
	 */
	if ( fsync(b->fd) && errno!=EROFS && errno!=EINVAL )
		ret = 0;

	if ( !close(b->fd) )
		ret = 0;

noflush:
	b->fd = fd;

	return ret;
}

/**
 * @param b: a fobuf structure to use
 * @param buf: pointer to the data you want to write
 * @param len: size of data pointed to by buf
 *
 * Slow path for writing to the buffer, do not call directly instead
 * use fobuf_write().
 *
 * Return value: zero on error, non-zero on success.
 */
static int fobuf_write_slow(struct _fobuf *b, const void *buf, size_t len)
{
	/* fill up the buffer before flushing, we already know
	 * that len >= b->buf_len so a full buffer flush is
	 * inevitable.
	 */
	memcpy(b->ptr, buf, b->buf_len);
	buf += b->buf_len;
	len -= b->buf_len;
	b->ptr += b->buf_len;
	b->buf_len = 0;
	if ( !_fobuf_flush(b) )
		return 0;

	/* If the remaining data is the same size as the buffer then
	 * buffering is doing an un-necessary copy of the data.
	 *
	 * If the remaining data is bigger than the buffer then we
	 * must write it out right away anyway.
	 */
	if ( len >= b->buf_sz )
		return fd_write(b->fd, buf, len);

	/* normal write - len may be zero */
	memcpy(b->ptr, buf, len);
	b->ptr += len;
	b->buf_len -= len;

	return 1;
}

int fobuf_write(fobuf_t b, const void *buf, size_t len)
{
	if ( likely(len < b->buf_len) ) {
		memcpy(b->ptr, buf, len);
		b->ptr += len;
		b->buf_len -= len;
		return 1;
	}

	return fobuf_write_slow(b, buf, len);
}

int fobuf_fd(fobuf_t b)
{
	return b->fd;
}


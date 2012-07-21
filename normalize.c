/*
 * This file is part of nads
 * Copyright (c) 2003 ECSC Ltd.
 * Author: Gianni Tedesco <gianni@scaramanga.co.uk>
 * Released under the terms of the GNU GPL version 2
 *
 * This is real slow right now because we go through the URL
 * many times over, in future we need to optimize it for a
 * single pass.
 *
 * TODO:
 *  o Fix endian bugs in unicode stuff
 *  o normalize UTF-16 (%00%xx)
 *  o Research effect of UTF-16 encodings
 *  o Reduce stack usage in n_path()
 *  o Optimize
 *
 * Functions:
 *  o chartype() - ctype-alike
 *  o hexchar() - convert a hex character to an integer
 *  o n_hex() - normalize hex-encoded characters
 *  o n_pcount() - count file path components
 *  o n_path() - normalize file paths for /./ and /../
 *  o n_split() - split file path from query string
 *  o n_utf8() - normalize UTF-8 characters
 *  o nads_normalize() - external API to normalize a URL
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>

#include <compiler.h>
#include <normalize.h>

static int nads_ws(unsigned long x)
{
	static const unsigned int flags = NADS_WS_CASE_SENSITIVE;
	return ( (flags & x) != 0 );
}

/* The actual error code, kinda like errno */
static unsigned int nads_errcode;

static const char *estr[]={
	[ NADS_ERR_SUCCESS ] = "Success",
	[ NADS_ERR_NO_EMULATION ] = "Unable to emulate specified webserver",
	[ NADS_ERR_BAD_HEX ] = "Bad Hex encoding",
	[ NADS_ERR_TRAVERSAL ] = "Attempt to traverse outside of webroot",
	[ NADS_ERR_BAD_UTF8 ] = "Bad UTF-8 character sequence",
	[ NADS_ERR_NULL ] = "NUL character insertion",
	[ NADS_ERR_OOM ] = "Out of memory",
};

/* nads_error:
 *
 * Get the nads error code.
 *
 * Return Value: The error code for the last call in to the nads
 * library.
 */
int nads_error(void)
{
	return nads_errcode;
}

unsigned int nads_errno(void)
{
	return nads_errcode;
}

/* nads_strerror:
 * @err: A nads error code returned from nads_error()
 *
 * Given a nads error code, return a string describing the error.
 *
 * Return value: A pointer to a string constant containing an
 * error description in english.
 */
const char *nads_strerror(unsigned int err)
{
	if ( err > sizeof(estr)/sizeof(*estr) )
		return "Unknown error";

	return estr[err];
}

/* nads_errstr:
 *
 * Returns a string describing the error for the last call in to the
 * nads library.
 *
 * Return value: A pointer to a string constant containing an
 * error description in english.
 */
const char *nads_errstr(void)
{
	return nads_strerror(nads_errcode);
}
static char uribuf[NADS_MAX_URI];

/* ===== Ctype like functions ===== */
#define T_HEX (1<<0)
static const unsigned char tbl[256]={
	['a'] = T_HEX,
	['b'] = T_HEX,
	['c'] = T_HEX,
	['d'] = T_HEX,
	['e'] = T_HEX,
	['f'] = T_HEX,
	['A'] = T_HEX,
	['B'] = T_HEX,
	['C'] = T_HEX,
	['D'] = T_HEX,
	['E'] = T_HEX,
	['F'] = T_HEX,
	['0'] = T_HEX,
	['1'] = T_HEX,
	['2'] = T_HEX,
	['3'] = T_HEX,
	['4'] = T_HEX,
	['5'] = T_HEX,
	['6'] = T_HEX,
	['7'] = T_HEX,
	['8'] = T_HEX,
	['9'] = T_HEX,
};

/* chartype:
 * @c: A character
 *
 * Returns value: a bitmap specifying what groups the
 * character belongs to.
 */
static int chartype(char c)
{
	return (int)tbl[(unsigned char)c];
}

/* hexchar:
 * @c: A character
 *
 * Converts a hex character to decimal.
 *
 * Return value: an integer value of the caracters hexadecimal
 * representation.
 *
 * Bugs: No way of retuning an error.
 */
static char hexchar(unsigned int c)
{
	if ( c - '0' < 10 )
		return c - '0';
	if ( c - 'a' < 6 )
		return (c - 'a') + 10;
	if ( c - 'A' < 6 )
		return (c - 'A') + 10;

	return 0;
}

/* n_hex:
 * @buf: A URL buffer
 *
 * Takes all URL encoded characters in a string and decodes them.
 * Also decodes Microsoft UTF-16 (%uXXXX) if necessary.
 *
 * Return value: zero for success, -1 for error. In the error case
 * the contents of @buf are undefined.
 */
static int n_hex(char *buf)
{
	char *in, *out;
	int state = 0;
	char hbuf[5] = {0,0,0,0,0};
	int i = 0;
	int maxlen = 0;

	for(in = out = buf; *in; in++) {
		switch (state) {
		case 0:
			if ( *in == '%' ) {
				if ( nads_ws(NADS_WS_MS_UTF16) &&
					(*(in+1) == 'u' || *(in+1) == 'U') ) {
					/* MS-UTF16 (%u1234) */
					maxlen=2;
					in++;
				}else{
					maxlen=1;
				}
				state=1;
				i = 0;
				break;
			}
			*out++ = *in;
			break;
		case 1:
			if ( chartype(*in) & T_HEX ) {
				hbuf[i++] = *in;
				if ( i >= (maxlen*2) ) {
					char d1,d2;

					state=0;

					d1 = hexchar(hbuf[0]) << 4;
					d1 |= hexchar(hbuf[1]);
					d2 = hexchar(hbuf[2]) << 4;
					d2 |= hexchar(hbuf[3]);

					/* FIXME: Treat %u00XX as %XX ? */
					if ( maxlen == 2 && d1 == 0 ) {
						d1 = d2;
						maxlen = 1;
					}

					/* Can't put NULs in the stream */
					if ( d1==0 || (maxlen==2 && d2==0) ) {
						nads_errcode = NADS_ERR_NULL;
						return 0;
					}

					*out++ = d1;
					if ( maxlen==2 )
						*out++ = d2;
				}
			}else if ( maxlen == 1 ) {
				/* IIS seems to ignore percent signs
				 * on their own followed by crap...
				 */
				if ( nads_ws(NADS_WS_PERCENT_HACK) && i==0 ) {
					*out++='%';
					in--;
					state=0;
					break;
				}

				nads_errcode = NADS_ERR_BAD_HEX;
				return 0;
			}
			break;
		}
	}

	if ( state != 0 ) {
		nads_errcode = NADS_ERR_BAD_HEX;
		return 0;
	}

	*out='\0';
	return 1;
}

/* n_pcount:
 * @buf: A string containing a URL.
 *
 * Counts all file path components in a string. Converts backslashes to
 * forward slashes on the way.
 *
 * Return value: The number of path components.
 */
static int n_pcount(char *buf)
{
	int cnt = 0;
	char *in, *out;
	char *last = NULL;

	for(in = out = buf; *in; in++){
		if ( *in == '\\' )
			*in = '/';

		if ( *in == '/' ) {
			if ( last && last == in-1 ) {
				last=in;
				continue;
			}

			last = in;
			cnt++;
		}

		if ( nads_ws(NADS_WS_CASE_SENSITIVE) ) {
			*out++ = *in;
		}else{
			*out++ = tolower(*in);
		}
	}

	*out = '\0';
	return cnt;
}

/* n_path:
 * @buf: A string containing a URL
 *
 * This function normalizes various file-path related trickery such as:
 *  - double slashes
 *  - DOS/Windows syntax (via n_pcount)
 *  - reverse traversal
 *  - self reference directories
 *
 * Return value: zero for success, -1 for error. In the error case
 * the contents of @buf are undefined.
*/
static int n_path(char *buf)
{
	int x = n_pcount(buf);
	char *obuf = buf;
	char *c[x];
	int in,out;
	int i=0;
	char *str;

	/* Grab the directory components */
	for(; *buf; buf++) {
		if ( *buf == '/' ) {
			*buf = 0;
			c[i] = buf + 1;
			i++;
		}
	}

	/* Strip out /./ and /../ */
	for(in = out = 0; in < x; in++) {
		if ( !strcmp(c[in], ".") )
			continue;

		if ( !strcmp(c[in], "..") ) {
			if ( --out < 0 ) {
				nads_errcode = NADS_ERR_TRAVERSAL;
				return 0;
			}
			continue;
		}

		c[out++]=c[in];
	}

	/* Reconstruct the URL */
	for(str=obuf+1,i=0; i<out; i++) {
		char *tmp;
		for(tmp=c[i]; *tmp; tmp++){
			*str=*tmp;
			str++;
		}

		if ( i < out-1 ) {
			*str='/';
			str++;
		}
	}

	*obuf = '/';
	*str = 0;

	return 1;
}

/* n_split:
 * @buf: A nads struct with a valid buf
 *
 * Copies out the input buffer and splits it at the '?' in order to
 * seperate the query string. Fills in n->uri and n->query.
 *
 * Return value: A pointer to the query string within buf or NULL if
 * there is no query string.
 */
static void n_split(struct nads *n)
{
	const char *buf, *end;
	char *out;

	n->uri = uribuf;
	n->query = NULL;
	buf = n->buf;
	end = buf + n->buf_len;
	out = n->uri;

	for(; buf < end; buf++) {
		if ( *buf == '?' ) {
			*(out++) = '\0';
			n->query = out;
			buf++;
			break;
		}else{
			*(out++) = *buf;
		}
	}

	for(; buf < end; buf++) {
		*(out++) = *buf;
	}

	*(out++) = '\0';
}

/* n_ucs4:
 * @n: Pointer to an unsigned 32 bit integer to store the UCS4
 * @buf: A 1-6 byte buffer containing a UTF-8 character sequence
 * @len: Length of the UTF-8 sequence.
 *
 * Converts a UTF-8 character sequence in to a UCS4 character
 */
static void n_ucs4(uint32_t *n, char *buf, int len)
{
	int s;
	int i;
	static const char fmask[6] = {
		0x01|0x02|0x04|0x08|0x10,
		0x01|0x02|0x04|0x08,
		0x01|0x02|0x04,
		0x01|0x02,
		0x01,
		0,
	};

	*n = 0;

	for(s = 0,i = len; i-- > 0; s += 6) {
		char mask;

		if ( i == 0 ) {
			mask = fmask[len - 2];
		}else{
			mask = 0x3f;
		}

		*n |= (buf[i] & mask) << s;
	}
}

/* n_utf8:
 * @buf: A string containing a URL
 *
 * Strips out any UTF-8 encoded ASCII values. Real UTF-8 stays
 * untouched.
 *
 * Return value: zero for success, -1 for error. In the error case
 * the contents of @buf are undefined.
 */
static int n_utf8(char *buf)
{
	char *in,*out;
	char chr[6];
	int len=0, i=0;
	int state=0;
	uint32_t ucs4;

	for(in=out=buf; *in; in++) {
		if ( *in == 0xff || *in == 0xfe ) {
			nads_errcode = NADS_ERR_BAD_UTF8;
			return 0;
		}

		if ( state == 0 ) {
			if ( *in & 0x80 ) {
				char n=0x40;

				for(len=1; n != 0x01; len++){
					if ( (*in & n) == 0 )
						break;
					n >>= 1;
				}

				chr[0] = *in;
				state = 1;
				i = 1;
			}else{
				*out = *in;
				out++;
			}
		}else{
			if ( (*in & 0xc0) != 0x80 ) {
				/* FIXME: IIS Ignores continuation markers */
#if 0
				nads_errcode = NADS_ERR_BAD_UTF8;
				return 0;
#endif
			}

			chr[i++] = *in;
			if ( i >= len ) {
				state=0;
				n_ucs4(&ucs4, chr, len);
				if ( ucs4 & 0xffffff00 ) {
					memcpy(out, chr, len);
					out += len;
				}else{
					*out = ucs4 & 0xff;
					out++;
				}
			}
		}
	}

	if ( state != 0 ) {
		nads_errcode = NADS_ERR_BAD_UTF8;
		return 0;
	}

	*out='\0';
	return 1;
}

/* nads_normalize:
 * @n: A nads structure with buf set
 *
 * Normalizes a URL and passes it on to the signature detection engines
 * if necessary.
 */
int nads_normalize(struct nads *n)
{
	/* Can we hex encode the '?' */
	n_split(n);

	/* Process the URI */
	if ( !n_hex(n->uri) )
		return NADS_FAIL;

	if ( nads_ws(NADS_WS_DOUBLE_HEX) && !n_hex(n->uri) )
		return NADS_FAIL;

	if ( !n_utf8(n->uri) )
		return NADS_FAIL;

	if ( !n_path(n->uri) )
		return NADS_FAIL;

	/* TODO: Process the query string if one exists */
	if ( n->query == NULL )
		goto finish;

	if ( !n_hex(n->query) )
		return NADS_FAIL;

finish:
	nads_errcode = NADS_ERR_SUCCESS;
	return NADS_OK;
}

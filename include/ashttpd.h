#ifndef _ASHTTPD_H
#define _ASHTTPD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <compiler.h>
#include <list.h>
#include <nbio.h>
#include <vec.h>
#include <os.h>

struct webroot_name {
	struct ro_vec name;
	unsigned int mime_type;
	off_t f_ofs;
	size_t f_len;
};

_private int generic_webroot_fd(const char *fn);
_private const struct webroot_name *webroot_find(struct ro_vec *uri);
_private const char * const webroot_mime_type(unsigned int idx);

/* FIXME: more comprehensive handling of "code" pages */
_private extern const off_t obj404_f_ofs;
_private extern const size_t obj404_f_len;
_private extern const unsigned int obj404_mime_type;

#endif /* _ASHTTPD_H */

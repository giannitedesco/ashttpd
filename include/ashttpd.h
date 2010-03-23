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

#define BM_SKIP_LEN 0x100
_private void bm_skip(const uint8_t *x, size_t plen, int *skip);
_private const uint8_t *bm_find(const uint8_t *n, size_t nlen,
			const uint8_t *hs, size_t hlen,
			int *skip);

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

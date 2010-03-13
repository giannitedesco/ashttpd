#ifndef _ASHTTPD_H
#define _ASHTTPD_H

#include <compiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <list.h>
#include <nbio.h>
#include <hgang.h>

struct vec {
	uint8_t *v_ptr;
	size_t v_len;
};

struct ro_vec {
	const uint8_t *v_ptr;
	size_t v_len;
};

_private int vcasecmp(const struct ro_vec *v1, const struct ro_vec *v2);
_private int vcmp(const struct ro_vec *v1, const struct ro_vec *v2);
_private int vstrcmp(const struct ro_vec *v1, const char *str);
_private int vcasecmp_fast(const struct ro_vec *v1, const struct ro_vec *v2);
_private int vcmp_fast(const struct ro_vec *v1, const struct ro_vec *v2);
_private int vstrcmp_fast(const struct ro_vec *v1, const char *str);
_private size_t vtouint(struct ro_vec *v, unsigned int *u);

#endif /* _ASHTTPD_H */

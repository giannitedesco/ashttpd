#include <ashttpd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "webroot.h"

const struct webroot_name *webroot_find(struct ro_vec *uri)
{
	const struct webroot_name *haystack;
	unsigned int n;

	for(n = sizeof(webroot_namedb)/sizeof(*webroot_namedb),
			haystack = webroot_namedb; n; ) {
	 	unsigned int i;
		int cmp;

		i = n / 2U;

		cmp = vcmp_fast(uri, &haystack[i].name);
		if ( cmp < 0 ) {
			n = i;
		}else if ( cmp > 0 ) {
			haystack = haystack + (i + 1U);
			n = n - (i + 1U);
		}else
			return haystack + i;
	}

	return NULL;
}

int generic_webroot_fd(const char *fn)
{
	return open(fn, O_RDONLY);
}

const char * const webroot_mime_type(unsigned int idx)
{
	assert(idx < sizeof(mime_types)/sizeof(*mime_types));
	return mime_types[idx];
}

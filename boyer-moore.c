#include <ashttpd.h>
#include <limits.h>
#include <assert.h>

const uint8_t *bm_find(const uint8_t *n, size_t nlen,
			const uint8_t *hs, size_t hlen,
			int *skip)
{
	int skip_stride, shift_stride, p_idx;
	int b_idx;

	assert(hlen < (size_t)INT_MAX);

	/* Do the search */
	for(b_idx = nlen; b_idx <= (int)hlen; ) {
		p_idx = nlen;

		while(hs[--b_idx] == n[--p_idx]) {
			if (b_idx < 0)
				return NULL;
			if (p_idx == 0)
				return hs + b_idx;
		}

		skip_stride = skip[hs[b_idx]];
		shift_stride = (nlen - p_idx) + 1;

		/* micro-optimised max() function */
		b_idx += ( (skip_stride - shift_stride) > 0 )
			? skip_stride : shift_stride;
	}

	return NULL;
}

void bm_skip(const uint8_t *x, size_t plen, int *skip)
{
	int *sptr = &skip[0x100];

	while( sptr-- != skip )
		*sptr = plen + 1;

	while(plen != 0)
		skip[*x++] = plen--;
}


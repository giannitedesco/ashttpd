#include <ashttpd.h>
#include <webroot-format.h>
#include <os.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

#include "webroot-common.h"

static gidx_oid_t trie_query(struct _webroot *r,
				const struct trie_dedge *re,
				unsigned int num_edges,
				struct ro_vec *str)
{
	unsigned int i;

	while(num_edges) {
		uint32_t edges_idx, strtab_ofs;
		uint8_t res[3];
		const uint8_t *ptr;
		struct ro_vec match;
		int cmp;

		i = (num_edges / 2);

		edges_idx = trie_edges_index(re + i);
		strtab_ofs = trie_strtab_ofs(re + i);
		if ( string_is_resident(re + i) ) {
			res[0] = strtab_ofs & 0xff;
			res[1] = (strtab_ofs >> 8) & 0xff;
			res[2] = (strtab_ofs >> 16) & 0xff;
			ptr = res;
		}else{
			ptr = r->r_strtab + strtab_ofs;
		}

		match = *str;
		match.v_len = re[i].re_strtab_len;

		cmp = memcmp(match.v_ptr, ptr, match.v_len);
		dprintf("'%.*s' vs '%.*s' (idx[%lu] / %u) = %d\n",
			(int)match.v_len,
			match.v_ptr,
			(int)re[i].re_strtab_len,
			ptr,
			&re[i] - r->r_trie,
			num_edges,
			cmp);

		if ( cmp < 0 ) {
			num_edges = i;
		}else if ( cmp > 0 ) {
			re = re + (i + 1);
			num_edges = num_edges - (i + 1);
		}else{
			struct ro_vec suff;
			suff = *str;
			suff.v_ptr += match.v_len;
			suff.v_len -= match.v_len;
			if ( !suff.v_len ) {
				dprintf("found %d\n", re[i].re_oid);
				return re[i].re_oid;
			}
			dprintf("RECURSE %d => %d\n", edges_idx,
				re[i].re_num_edges);
			assert(edges_idx < r->r_num_edges);
			return trie_query(r, r->r_trie + edges_idx,
					re[i].re_num_edges, &suff);
		}
	}

	return GIDX_INVALID_OID;
}

static int map_webroot(struct _webroot *r, uint64_t sz)
{
	struct stat st;

	if ( fstat(r->r_fd, &st) )
		return 0;

	if ( (uint64_t)st.st_size < sz ) {
		fprintf(stderr, "webroot: unable to map index\n");
		return 0;
	}

	r->r_map = mmap(NULL, sz, PROT_READ, MAP_SHARED, r->r_fd, 0);
	if ( r->r_map == MAP_FAILED )
		return 0;

	r->r_map_sz = sz;
	return 1;
}

webroot_t webroot_open(const char *fn)
{
	struct _webroot *r;
	struct webroot_hdr hdr;
	const uint8_t *ptr;
	size_t sz;
	int eof;

	r = calloc(1, sizeof(*r));
	if ( NULL == r )
		goto out;

	r->r_fd = open(fn, O_RDONLY);
	if ( r->r_fd < 0 ) {
		fprintf(stderr, "webroot: %s: %s\n", fn, os_err());
		goto out_free;
	}

	sz = sizeof(hdr);
	if ( !fd_read(r->r_fd, &hdr, &sz, &eof) || eof || sz != sizeof(hdr) ) {
		fprintf(stderr, "webroot: %s: failed to read header\n", fn);
		goto out_close;
	}

	if ( hdr.h_magic != WEBROOT_MAGIC ) {
		fprintf(stderr, "webroot: %s: bad magic\n", fn);
		goto out_close;
	}
	if ( hdr.h_vers != WEBROOT_CURRENT_VER ) {
		fprintf(stderr, "webroot: %s: unexpected version\n", fn);
		goto out_close;
	}

	if ( !map_webroot(r, hdr.h_files_begin) )
		goto out_close;

	r->r_num_edges = hdr.h_num_edges;
	r->r_num_redirect = hdr.h_num_redirect;
	r->r_num_oid = hdr.h_num_redirect + hdr.h_num_file;

	ptr = r->r_map + sizeof(hdr);

	r->r_trie = (struct trie_dedge *)ptr;
	ptr += hdr.h_num_edges * sizeof(*r->r_trie);

	r->r_redir = (struct webroot_redirect *)ptr;
	ptr += hdr.h_num_redirect * sizeof(*r->r_redir);

	r->r_file = (struct webroot_file *)ptr;
	ptr += hdr.h_num_file * sizeof(*r->r_file);

	r->r_strtab = ptr;
	ptr += hdr.h_strtab_sz;

	if ( ptr > (uint8_t *)r->r_map + r->r_map_sz ) {
		fprintf(stderr, "webroot: %s: index truncated\n", fn);
		goto out_unmap;
	}

	goto out; /* success */

out_unmap:
	munmap((void *)r->r_map, r->r_map_sz);
out_close:
	fd_close(r->r_fd);
out_free:
	free(r);
	r = NULL;
out:
	return r;
}

int webroot_get_fd(webroot_t r)
{
	return r->r_fd;
}

int webroot_find(webroot_t r, const struct ro_vec *uri,
				struct webroot_name *out)
{
	struct ro_vec match = *uri;
	gidx_oid_t idx;

	dprintf("matching %.*s\n", (int)match.v_len, match.v_ptr);
	idx = trie_query(r, r->r_trie, 1, &match);
	if ( idx == GIDX_INVALID_OID ) {
		dprintf("NOPE\n\n");
		return 0;
	}
	assert(idx < r->r_num_oid);

	if ( idx < r->r_num_redirect ) {
		const struct webroot_redirect *redir;

		redir = r->r_redir + idx;
		out->code = HTTP_MOVED_PERMANENTLY;

		out->u.moved.v_ptr = r->r_map + redir->r_off;
		out->u.moved.v_len = redir->r_len;
		dprintf("redirect %.*s -> %.*s\n",
			(int)uri->v_len,
			uri->v_ptr,
			(int)redir->r_len,
			(char *)r->r_map + redir->r_off);
	}else{
		const struct webroot_file *file;

		dprintf("file %u - %u\n", idx, r->r_num_redirect);
		file = r->r_file + (idx - r->r_num_redirect);
		out->code = HTTP_FOUND;

		out->mime_type.v_ptr = r->r_map + file->f_type;
		out->mime_type.v_len = file->f_type_len;
		out->u.data.f_ofs = file->f_off;
		out->u.data.f_len = file->f_len;
	}

	dprintf("\n");

	return 1;
}

void webroot_close(webroot_t r)
{
	if ( r ) {
		munmap((void *)r->r_map, r->r_map_sz);
		fd_close(r->r_fd);
		free(r);
	}
}

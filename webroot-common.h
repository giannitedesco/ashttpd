#ifndef _WEBROOT_COMMON_H
#define _WEBROOT_COMMON_H

struct _webroot {
	int r_fd;
	const void *r_map;
	size_t r_map_sz;

	unsigned int r_num_edges;
	unsigned int r_num_redirect;
	unsigned int r_num_oid;

	const struct trie_dedge *r_trie;
	const struct webroot_redirect *r_redir;
	const struct webroot_file *r_file;
	const uint8_t *r_strtab;
};

static inline uint32_t trie_edges_index(const struct trie_dedge *e)
{
	return (e->re_edges_hi << 16) | e->re_edges_idx;
}

static inline uint32_t trie_strtab_ofs(const struct trie_dedge *e)
{
	return (e->re_strtab_hi << 16) | e->re_strtab_ofs;
}

static inline int string_is_resident(const struct trie_dedge *e)
{
	return e->re_strtab_len < 4;
}

#endif /* _WEBROOT_COMMON_H */

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

#endif /* _WEBROOT_COMMON_H */

/*
* This file is part of gidx
* Copyright (c) 2010 Gianni Tedesco
* Released under the terms of the GNU GPL version 3
*/
#ifndef _TRIE_H
#define _TRIE_H

struct trie_entry {
	struct ro_vec t_str;
	gidx_oid_t t_oid;
};

typedef struct trie *trie_t;
trie_t trie_new(const struct trie_entry *t, unsigned int cnt);
int trie_write_trie(struct trie *r, fobuf_t buf);
uint64_t trie_trie_size(struct trie *r);
uint64_t trie_num_edges(struct trie *r);
void trie_free(trie_t r);

#endif /* _TRIE_H */

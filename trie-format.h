/*
* This file is part of gidx
* Copyright (c) 2010 Gianni Tedesco
* Released under the terms of the GNU GPL version 3
*/
#ifndef _GIDX_STR_FORMAT_H
#define _GIDX_STR_FORMAT_H

struct trie_dedge {
	gidx_oid_t	re_oid;
	uint8_t		re_num_edges;
	uint8_t		re_strtab_len;
	uint8_t		re_edges_hi;
	uint8_t		re_strtab_hi;
	uint16_t	re_edges_idx;
	uint16_t	re_strtab_ofs;
}_packed;

#endif /* _GIDX_STR_FORMAT_H */

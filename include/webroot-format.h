/*
* This file is part of gidx
* Copyright (c) 2010 Gianni Tedesco
* Released under the terms of the GNU GPL version 3
*/
#ifndef _WEBROOT_FORMAT_H
#define _WEBROOT_FORMAT_H

/* File layout:
 *  - Mapped
 *      Header
 *      Trie edges
 *      Redirect objects
 *	File objects
 *      Trie string table
 *      Mime string table
 *      Redirect string table
 * - Not mapped
 *      Data
 *
 * Notes on limits:
 *  - max 2^24 - 1 files
 *  - 4GB of combined mimet types and redirects
*/

#define WEBROOT_MAGIC		('w' << 24 | 'w' << 16 | 0x1337)
#define WEBROOT_CURRENT_VER	0
struct webroot_hdr {
	uint32_t	h_num_edges;
	uint32_t	h_num_redirect;
	uint32_t	h_num_files;
	uint32_t	h_strtab_sz; /* all strings */
	uint32_t	h_magic;
	uint32_t	h_vers;
} _packed;

struct webroot_obj {
	union {
		struct {
			uint64_t f_off;
			uint64_t f_len;
			uint32_t f_type;
			uint32_t f_type_len;
		} file;
		struct {
			uint32_t r_off;
			uint32_t r_len;
		} redirect;
	}o_u;
}_packed;

#define GIDX_INVALID_OID 0xffffffffU
typedef uint32_t gidx_oid_t;

struct trie_dedge {
	gidx_oid_t	re_oid;
	uint8_t		re_num_edges;
	uint8_t		re_strtab_len;
	uint8_t		re_edges_hi;
	uint8_t		re_strtab_hi;
	uint16_t	re_edges_idx;
	uint16_t	re_strtab_ofs;
}_packed;

#endif /* _WEBROOT_FORMAT_H */

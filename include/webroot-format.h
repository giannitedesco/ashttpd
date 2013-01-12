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
 *  - 4GB of combined mime types and redirects
*/

#define WEBROOT_MAGIC		((0x37 << 24) | (0x13 << 16) | 'W' << 8 | 'w')
#define WEBROOT_CURRENT_VER	1
struct webroot_hdr {
	uint32_t	h_num_edges;
	uint32_t	h_num_redirect;
	uint32_t	h_num_file;
	uint32_t	h_strtab_sz; /* all strings */
	uint32_t	h_magic;
	uint32_t	h_vers;
	uint32_t	h_files_begin;
} _packed;

struct webroot_file {
	uint64_t f_off;
	uint64_t f_len;
	uint32_t f_type;
	uint32_t f_type_len;
} _packed;

#define WEBROOT_INVALID_REDIRECT 0xffffffffU
struct webroot_redirect {
	uint32_t r_off;
	uint32_t r_len;
} _packed;

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

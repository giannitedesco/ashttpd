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
#define WEBROOT_CURRENT_VER	3
struct webroot_hdr {
	uint32_t	h_num_edges;
	uint32_t	h_num_redirect;
	uint32_t	h_num_file;
	uint32_t	h_strtab_sz; /* all strings */
	uint32_t	h_magic;
	uint32_t	h_vers;
	uint32_t	h_files_begin;
	uint32_t	h__pad;
} _packed;

#define WEBROOT_DIGEST_LEN	20
struct webroot_file {
	uint64_t f_off;
	uint64_t f_len;
	uint32_t f_type;
	uint32_t f_type_len;
	uint32_t f_modified;
	uint8_t f_digest[WEBROOT_DIGEST_LEN];
} _packed;

#define WEBROOT_INVALID_REDIRECT 0xffffffffU
struct webroot_redirect {
	uint32_t r_off;
	uint32_t r_len;
} _packed;

#define GIDX_INVALID_OID 0xffffffffU
typedef uint32_t gidx_oid_t;

#define RE_EDGE_MAX	8
struct trie_dedge {
	gidx_oid_t	re_oid;
	uint32_t	re_edges_idx;
	uint8_t		re_num_edges;
	uint8_t		re_strlen;
	uint8_t		re_str[RE_EDGE_MAX];
}_packed;

#endif /* _WEBROOT_FORMAT_H */

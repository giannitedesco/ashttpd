/*
 * This file is part of SkunkDB
 * Copyright (c) 2002 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
*/
#ifndef __SKUNK_HEADER_INCLUDED__
#define __SKUNK_HEADER_INCLUDED__

#define SKUNK_HASHSZ 2048UL

typedef uint32_t skunk_ofs_t;

/* Hash size */
#ifdef SKUNK_HASH_64
typedef uint64_t skunk_hash_t;
#define FNV_PRIME       1099511628211ULL
#define FNV_OFFSET      14695981039346656037ULL
#else
typedef uint32_t skunk_hash_t;
#define FNV_PRIME       16777619UL
#define FNV_OFFSET      2166136261UL
#endif

/* Generic record structure */
struct rec {
	char *key, *val;
	size_t key_len, val_len;
};

/* Database creation structures */
struct skunk_rec {
	struct skunk_rec *next;
	size_t key_len, val_len;
	void *key, *val;
	skunk_hash_t hash;
};

struct skunk_head {
	size_t count;
	struct skunk_rec *list;
};

struct skunk_make {
	int fd;
	hgang_t rec;
	struct skunk_head head[SKUNK_HASHSZ];
};

/* Database query structures */
struct skunk_db {
	char	*map;
	char	*end;
	size_t	maplen;
	struct rec *result;
};

/* These are our two on-disk structures */
struct skunk_table{
	skunk_ofs_t	ofs;
	skunk_ofs_t	len;
};

struct skunk_bucket {
	skunk_hash_t	hash;
	skunk_ofs_t	ofs;
	skunk_ofs_t	klen;
	skunk_ofs_t	vlen;
};

/* Database creation API */
struct skunk_make *skunk_new_db(int);
int skunk_insert(struct skunk_make *, char *, size_t, char *, size_t);
int skunk_commit(struct skunk_make *);
int skunk_abort(struct skunk_make *);

/* Database query API */
struct skunk_db *skunk_open(int);
int skunk_select(struct skunk_db *, char *, size_t);
void skunk_close(struct skunk_db *);

/* Fowler/Noll/Vo hash 
 * Its fast to compute, its obvious code, it performs well... */
static inline skunk_hash_t skunk_hash(char *data, size_t len)
{
	skunk_hash_t h = FNV_OFFSET;

	while(len--)
		h = (h * FNV_PRIME) ^ *data++;

	return h;
}

#endif /* __SKUNK_HEADER_INCLUDED__ */

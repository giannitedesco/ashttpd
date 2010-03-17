/*
 * This file is part of SkunkDB
 * Copyright (c) 2002 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
*/

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <compiler.h>
#include <hgang.h>
#include <fobuf.h>
#include <skunk.h>

#define SKUNK_BATCH 32768

/* Open the file, and allocate the db object */
struct skunk_make *skunk_new_db(int fd)
{
	struct skunk_make *d;
	
	if ( fd < 0 )
		return NULL;

	d = calloc(1, sizeof(*d));
	if ( NULL == d )
		return NULL;

	d->fd = fd;

	d->rec = hgang_new(sizeof(struct skunk_rec), SKUNK_BATCH);
	if ( NULL == d->rec ) {
		free(d);
		return NULL;
	}

	return d;
}

/* Add a new item to the database */
int skunk_insert(struct skunk_make *d, char * key, size_t klen,
		char *val, size_t vlen)
{
	struct skunk_rec *r;
	struct skunk_head *h;

	if ( !klen || !key || !val )
		return 0;

	r = hgang_alloc(d->rec);
	if ( NULL == r )
		return 0;

	r->key = key;
	r->key_len = klen;

	r->val = val;
	r->val_len = vlen;

	/* Hash in this entry */
	r->hash = skunk_hash(r->key, r->key_len);
	h = &d->head[r->hash % SKUNK_HASHSZ];
	r->next = h->list;
	h->list = r;
	h->count++;

	return 1;
}

/* Size up a hash-table */
static inline void skunk_size(struct skunk_head *h)
{
	/* Make sure load factor is no greater than 66% */
	h->count += h->count / 2;

	/* HACK: Hardcoded prime number */
	if ( h->count > 512 && h->count < 1021 )
		h->count = 1021;

	/* Make sure we aren't a power of 2 */
	h->count |= 1;
}

/* Collision resolution (linear probing) */
static void skunk_resolve(struct skunk_head *h, struct skunk_rec **r2)
{
	struct skunk_rec *r;
	struct skunk_rec *list=h->list;
	int i;

	memset(r2, 0, h->count * sizeof(struct skunk_rec *));

	for(r = list; r; r = r->next)
	{
		i=(r->hash ^ r->key_len) % h->count;
probe:
		if ( r2[i] ) {
			i = (i + 1) % h->count;
			goto probe;
		}else{
			r2[i] = r;
		}
	}

	for(h->list = NULL, i = h->count - 1; i >= 0; i--) {
		if ( r2[i] ) {
			r2[i]->next = h->list;
			h->list = r2[i];
		}
	}
}

/* Actually write all the records to disk */
int skunk_commit(struct skunk_make *d)
{
	struct skunk_rec **table;
	struct skunk_rec *r;
	struct skunk_head *h;
	size_t cur, max = 0;
	unsigned int i, j;
	int ret = 0;
	fobuf_t buf;

	/* Setup write buffering */
	buf = fobuf_new(d->fd, 32768);
	if ( NULL == buf )
		return 0;

	cur = SKUNK_HASHSZ * sizeof(struct skunk_table);

	/* Write out the primary hash table */
	for(i = 0; i < SKUNK_HASHSZ; i++) {
		struct skunk_table t;

		h = &d->head[i];

		if ( !h->count ) {
			t.ofs = 0;
			t.len = 0;
			if ( !fobuf_write(buf, &t, sizeof(t)) )
				return 0;
			continue;
		}

		skunk_size(h);

		if ( h->count > max )
			max = h->count;

		t.ofs = cur;
		t.len = h->count;
		cur += t.len * sizeof(struct skunk_bucket);

		if ( !fobuf_write(buf, &t, sizeof(t)) )
			return 0;
	}

	table=calloc(max, sizeof(*table));
	if ( NULL == table ) {
		goto finish;
	}

	/* Write main hash tables */
	for(i=0; i<SKUNK_HASHSZ; i++) {
		struct skunk_bucket b;

		h=&d->head[i];

		skunk_resolve(h, table);

		for(j = 0; j < h->count; j++) {
			if ( (r = table[j]) ) {
				b.hash = r->hash;
				b.ofs = cur;
				b.klen = r->key_len;
				b.vlen = r->val_len;
				cur += r->key_len+r->val_len;
			}else{
				memset(&b, 0, sizeof(b));
			}

			if ( !fobuf_write(buf, &b, sizeof(b)) ) {
				free(table);
				goto finish;
			}
		}
	}

	free(table);

	/* Write out the actual key/value pairs */
	for(i = 0; i < SKUNK_HASHSZ; i++) {
		h = &d->head[i];

		for(r = h->list; r; r = r->next) {
			if ( !fobuf_write(buf, r->key, r->key_len) ) {
				goto finish;
			}
			if ( !fobuf_write(buf, r->val, r->val_len) ) {
				goto finish;
			}
		}
	}

finish:
	if ( fobuf_close(buf) )
		ret = 1;

	hgang_free(d->rec);
	free(d);
	return ret;
}

int skunk_abort(struct skunk_make *d)
{
	hgang_free(d->rec);
	free(d);
	return 0;
}

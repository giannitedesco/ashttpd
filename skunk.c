/*
 * This file is part of SkunkDB
 * Copyright (c) 2002 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
 *
 * TODO
 *  o Support cursors.
*/

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include <compiler.h>
#include <hgang.h>
#include <skunk.h>

/* Map in the database */
struct skunk_db *skunk_open(int fd)
{
	struct skunk_db *d;
	struct stat st;

	if ( fstat(fd, &st) )
		return NULL;

	if ( st.st_size < SKUNK_HASHSZ * 
		sizeof(struct skunk_table) )
		return NULL;

	if ( !(d=calloc(1, sizeof(*d))) )
		return NULL;

	if ( (d->map=mmap(NULL, st.st_size, PROT_READ,
		MAP_SHARED, fd, 0))==MAP_FAILED ) {
		free(d);
		return NULL;
	}

	d->maplen=st.st_size;
	d->end=d->map + st.st_size;

	/* Closing the file does not unmap it, we
	 * just close it to save resources, heh */
	close(fd);

	return d;
}

/* Grab the first matching record from the database */
int skunk_select(struct skunk_db *d, char *key, size_t klen)
{
	struct skunk_table *t;
	struct skunk_bucket *b;
	skunk_ofs_t i, dist=0;
	skunk_hash_t h;

	if ( !klen )
		return -1;

	h=skunk_hash(key, klen);

	t=((struct skunk_table *)d->map) + (h % SKUNK_HASHSZ);

	if ( !t->ofs || !t->len )
		return 0;

	if ( t->ofs > d->maplen )
		return -1;

	i=(h ^ klen) % t->len;
	for(;;) {
		b=((struct skunk_bucket *)(d->map+t->ofs)) + i;

		if ( (char *)b > d->end )
			return -1;

		if ( b->hash == h && b->klen == klen ) {
			char *record=(char *)(d->map + b->ofs);

			if ( b->ofs + b->klen + b->vlen > d->maplen )
				goto not_this;

			if ( memcmp(record, key, klen) != 0 )
				goto not_this;

			d->result->key=record;
			d->result->val=record+b->klen;
			d->result->key_len=b->klen;
			d->result->val_len=b->vlen;
			return 1;
		}
not_this:
		if ( ++dist>t->len )
			break;

		i=(i+1) % t->len;
	}
	return 0;
}

/* Close the database */
void skunk_close(struct skunk_db *d)
{
	munmap(d->map, d->maplen);
	free(d);
}

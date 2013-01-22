#include <compiler.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <list.h>
#include <hgang.h>
#include <strpool.h>
#include <fobuf.h>
#include <vec.h>
#include <webroot-format.h>

#include "trie.h"

#define DEBUG_INCORE		0

struct trie {
	hgang_t			r_edges;
	struct trie_edge	*r_root_edge;
	struct list_head	r_bfs_order;
	unsigned int		r_num_edges;
	unsigned int		r_max_fanout;
	unsigned int		r_max_cmp;
	unsigned int		r_max_path;
};

/* nodes in a radix tree do nothing but point to more edges so fuck it,
 * we just store the edges
 */
struct trie_edge {
	struct list_head	e_list;
	struct list_head	e_bfs;
	struct ro_vec		e_cmp;
	const struct trie_entry	*e_terminal;
	unsigned int		e_num_edges;
	unsigned int		e_bfs_idx;
	struct list_head	e_edges;
};

static struct trie_edge *edge_new(struct trie *r, struct trie_edge *parent)
{
	struct trie_edge *e;

	e = hgang_alloc0(r->r_edges);
	if ( NULL == e ) {
		//ERR("%s", os_err());
		return NULL;
	}

	INIT_LIST_HEAD(&e->e_edges);
	INIT_LIST_HEAD(&e->e_bfs);
	e->e_num_edges = 0;
	e->e_terminal = NULL;

	if ( parent ) {
		list_add_tail(&e->e_list, &parent->e_edges);
		parent->e_num_edges++;
		if ( parent->e_num_edges > r->r_max_fanout )
			r->r_max_fanout = parent->e_num_edges;
	}
	r->r_num_edges++;

	return e;
}

static const struct trie_entry *next_string(const struct trie_entry *v,
						const struct trie_entry *end)
{
	if ( (v + 1) >= end )
		return NULL;
	return v + 1;
}

static size_t common_prefix(const struct ro_vec *a, const struct ro_vec *b,
				size_t ofs)
{
	size_t ret, min;

	min = (a->v_len < b->v_len) ? a->v_len : b->v_len;
	//DEBUG("%.*s %.*s ofs=%zu min=%zu",
	//	(int)a->v_len, a->v_ptr,
	//	(int)b->v_len, b->v_ptr,
	//	ofs, min);
	//assert(ofs < min);
	if ( ofs >= min )
		return 0;
	//DEBUG("%.*s %.*s",
	//	(int)(min - ofs), a->v_ptr + ofs,
	//	(int)(min - ofs), b->v_ptr + ofs);

	for(ret = 0; ret < min - ofs; ret++)
		if ( a->v_ptr[ofs + ret] != b->v_ptr[ofs + ret] )
			break;

	return (ret > RE_EDGE_MAX) ? RE_EDGE_MAX : ret;
}

static int do_radix(struct trie *r, struct trie_edge *n,
			const struct trie_entry *v,
			const struct trie_entry *end,
			size_t ofs,
			unsigned int depth)
{
	const struct trie_entry *tmp, *last;
	size_t len;

	if ( depth > r->r_max_path )
		r->r_max_path = depth;

	//DEBUG("ofs = %zu", ofs);
	while(v) {
		struct trie_edge *e;

		assert(v < end);

		for(len = v->t_str.v_len - ofs, last = tmp = v;
				tmp && len < 0x100;
				last = tmp, tmp = next_string(tmp, end)) {
			size_t x;
			x = common_prefix(&v->t_str, &tmp->t_str, ofs);
			if ( 0 == x )
				break;
			len = (x < len) ? x : len;
		}

		e = edge_new(r, n);
		if ( NULL == e )
			return 0;

		e->e_cmp.v_ptr = v->t_str.v_ptr + ofs;
		e->e_cmp.v_len = len;
		if ( e->e_cmp.v_len > r->r_max_cmp )
			r->r_max_cmp = e->e_cmp.v_len;
		assert(ofs + len <= v->t_str.v_len);

		if ( ofs + len == v->t_str.v_len ) {
			//mesg(M_DEBUG, "radix-terminal: %.*s",
			//	(int)v->t_str.v_len, v->t_str.v_ptr);
			e->e_terminal = v;
			if ( v == last ) {
				//mesg(M_DEBUG, "  - no recurse");
				v = next_string(last, end);
				continue;
			}else{
				//mesg(M_DEBUG, "  - recurse without first");
				v = next_string(v, end);
			}
		}else{
			//mesg(M_DEBUG, "  - plain recurse");
		}

		//printf("prefix: %.*s\n",
		//		(int)(ofs + len),
		//		v->t_str.v_ptr);
		for(tmp = v; tmp; tmp = next_string(tmp, end)) {
			//printf("  shared by: %.*s\n",
			//		(int)tmp->t_str.v_len,
			//		tmp->t_str.v_ptr);
			if ( tmp == last )
				break;
		}

		do_radix(r, e, v, last + 1, ofs + len, depth + 1);
		v = next_string(last, end);
	}

	return 1;
}

static int construct_radix(struct trie *r,
				const struct trie_entry *ent,
				unsigned int cnt)
{
	struct trie_edge *n;

	if ( NULL == ent )
		return 1; /* empty radix tree */

	r->r_root_edge = n = edge_new(r, NULL);
	if ( NULL == n )
		return 0;

	return do_radix(r, n, ent, ent + cnt, 0, 1);
}

#if DEBUG_INCORE
#include <stdio.h>
static void do_dump(FILE *f, struct trie_edge *n)
{
	struct trie_edge *e;

	fprintf(f, "\t\"n_%p\" [label=\"\"];\n", n);
	if ( n->e_terminal ) {
		fprintf(f, "\t\"t_%p\" [shape=rectangle label=\"%.*s\"];\n",
			n->e_terminal,
			(int)n->e_terminal->t_str.v_len,
			n->e_terminal->t_str.v_ptr);
		fprintf(f, "\t\"n_%p\" -> \"t_%p\";\n", n, n->e_terminal);
	}

	list_for_each_entry(e, &n->e_edges, e_list) {
		do_dump(f, e);
		fprintf(f, "\t\"n_%p\" -> \"n_%p\" [label=\"%.*s\"];\n",
			n, e, (int)e->e_cmp.v_len, e->e_cmp.v_ptr);
	}
}

static void dump_radix(struct trie *r)
{
	FILE *f;

	f = fopen("radix.dot", "w");
	fprintf(f, "strict digraph \"Radix trie\" {\n");
	fprintf(f, "\tgraph[rankdir=LR];\n");
	fprintf(f, "\tnode[shape=ellipse, style=filled, "
			"fillcolor=transparent];\n");
	//fprintf(f, "\tedge[fontsize=6];\n");
	do_dump(f, r->r_root_edge);
	fprintf(f, "}\n");
	fclose(f);
}
#endif

/* lay out nodes in BFS order */
static void do_layout_bfs(struct trie *r)
{
	/* cur = list of nodes under consideration */
	/* next = list of their children */
	struct list_head cur, next;
	struct trie_edge *e;

	if ( NULL == r->r_root_edge )
		return;

	INIT_LIST_HEAD(&cur);
	INIT_LIST_HEAD(&next);
	list_add(&cur, &r->r_root_edge->e_bfs);

again:
	list_for_each_entry(e, &cur, e_bfs) {
		struct trie_edge *ee;
		list_for_each_entry(ee, &e->e_edges, e_list) {
			list_add_tail(&ee->e_bfs, &next);
		}
	}

	list_splice(&cur, r->r_bfs_order.prev);

	INIT_LIST_HEAD(&cur);
	if ( list_empty(&next) )
		return;

	list_splice(&next, &cur);
	INIT_LIST_HEAD(&next);
	goto again;

}

static void do_layout_inorder(struct trie_edge *e, gidx_oid_t *pk)
{
	struct trie_edge *ee;

	list_for_each_entry(ee, &e->e_edges, e_list)
		do_layout_inorder(ee, pk);
}

static void layout_radix(struct trie *r)
{
	struct trie_edge *e;
	uint32_t i = 0;
	gidx_oid_t pk = 0;

	do_layout_inorder(r->r_root_edge, &pk);

	do_layout_bfs(r);

	list_for_each_entry(e, &r->r_bfs_order, e_bfs) {
		e->e_bfs_idx = i++;
	}
}

trie_t trie_new(const struct trie_entry *ent, unsigned int cnt)
{
	struct trie *r;

	r = calloc(1, sizeof(*r));
	if ( NULL == r )
		return 0;

	INIT_LIST_HEAD(&r->r_bfs_order);

	r->r_edges = hgang_new(sizeof(struct trie_edge), 0);
	if ( NULL == r->r_edges )
		goto out_free;

	if ( !construct_radix(r, ent, cnt) )
		goto out_free_edges;
	layout_radix(r);

#if DEBUG_INCORE
	dump_radix(r);
#endif

	//INFO("%u edges, %u max fanout, %u max_cmp, %u max path",
	//	r->r_num_edges, r->r_max_fanout,
	//	r->r_max_cmp, r->r_max_path);
	return r;
out_free_edges:
	hgang_free(r->r_edges);
out_free:
	free(r);
	return NULL;
}

void trie_free(trie_t r)
{
	if (r) {
		hgang_free(r->r_edges);
		free(r);
	}
}

static int node_to_disk(fobuf_t buf, struct trie_edge *e, uint32_t idx)
{
	struct trie_dedge d;
	memset(&d, 0, sizeof(d));

	if ( e->e_terminal ) {
		d.re_oid = e->e_terminal->t_oid;
	}else{
		d.re_oid = GIDX_INVALID_OID;
	}

	/* PROOF: if there are n : n > 0xff outward edges then common prefix
	 * length must be > 1. But if common prefix is > 1 then there are at
	 * most 0xff outward edges with coomon prefix = 1. Therefore max
	 * outward edges is at most 0xff. QED
	 *
	 * FIXME: actually it's 0x100 so that's off by one. we could encode
	 * this as n - 1 and for the case outgoing edges = 0 then edges
	 * index should be zero which is invalid because thats the root
	 * node so any reference to that would introduce a cycle
	 */
	assert(e->e_num_edges < 0x100);
	assert(e->e_cmp.v_len <= sizeof(d.re_str));

	d.re_num_edges = e->e_num_edges;
	d.re_strlen = e->e_cmp.v_len;
	d.re_edges_idx = idx;
	memcpy(d.re_str, e->e_cmp.v_ptr, e->e_cmp.v_len);

	if ( !fobuf_write(buf, &d, sizeof(d)) )
		return 0;

	return 1;
}

int trie_write_trie(struct trie *r, fobuf_t buf)
{
	struct trie_edge *e;

	list_for_each_entry(e, &r->r_bfs_order, e_bfs) {
		uint32_t idx;

		if ( !list_empty(&e->e_edges) ) {
			struct trie_edge *ee;
			ee = list_entry(e->e_edges.next, struct trie_edge, e_list);
			idx = ee->e_bfs_idx;
		}else{
			idx = 0;
		}
		if ( !node_to_disk(buf, e, idx) )
			return 0;
	}

	return 1;
}

uint64_t trie_trie_size(struct trie *r)
{
	return sizeof(struct trie_dedge) * r->r_num_edges;
}

uint64_t trie_num_edges(struct trie *r)
{
	return r->r_num_edges;
}

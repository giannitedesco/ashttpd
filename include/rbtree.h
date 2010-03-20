/*
 * This file is part of atg
 * Copyright (c) 2007 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
*/

#ifndef _RBTREE_HEADER_INCLDUED_
#define _RBTREE_HEADER_INCLUDED_

struct rbtree {
	struct rb_node *rbt_root;
};

struct rb_node {
	/** Parent node in the tree. */
	struct rb_node *rb_parent;
#define CHILD_LEFT 0
#define CHILD_RIGHT 1
	/** Child nodes. */
	struct rb_node *rb_child[2];
#define COLOR_RED 0
#define COLOR_BLACK 1
	/** The red-black tree colour, may be COLOR_RED or COLOR_BLACK. */
	uint32_t rb_color;
};

_private struct rb_node *node_first(struct rbtree *t);
_private struct rb_node *node_last(struct rbtree *t);
_private struct rb_node *node_next(struct rb_node *n);
_private struct rb_node *node_prev(struct rb_node *n);
_private void rbtree_insert_rebalance(struct rbtree *s,
				struct rb_node *n);
_private void rbtree_delete_node(struct rbtree *s,
				struct rb_node *n);

/**
 * rbtree_entry - get the struct for this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_struct within the struct.
 */
#define rbtree_entry(ptr, type, member) \
	container_of(ptr, type, member)

#endif /* _RBTREE_HEADER_INCLUDED_ */

/*
 * This file is part of atg
 * Copyright (c) 2007 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
*/
#include <compiler.h>
#include <stdlib.h>
#include <stdint.h>
#include <rbtree.h>

/* For any given node, find the previous or next node */
static struct rb_node *node_prev_next(struct rb_node *n, int prev)
{
	if ( n == NULL )
		return NULL;

	if ( n->rb_child[prev ^ 1] ) {
		n = n->rb_child[prev ^ 1];
		while( n->rb_child[prev ^ 0] )
			n = n->rb_child[prev ^ 0];
		return n;
	}else{
		while(n->rb_parent && n != n->rb_parent->rb_child[prev ^ 0] )
			n = n->rb_parent;

		return n->rb_parent;
	}
}

static struct rb_node *node_first_last(struct rbtree *t, int first)
{
	struct rb_node *n, *ret;
	for(ret = n = t->rbt_root; n; ret = n, n = n->rb_child[first ^ 1])
		/* nothing */;
	return ret;
}

struct rb_node *node_first(struct rbtree *t)
{
	return node_first_last(t, 1);
}

struct rb_node *node_last(struct rbtree *t)
{
	return node_first_last(t, 0);
}

struct rb_node *node_next(struct rb_node *n)
{
	return node_prev_next(n, 0);
}
struct rb_node *node_prev(struct rb_node *n)
{
	return node_prev_next(n, 1);
}

/* Here we handle left/right rotations (the 2 are symmetrical) which are
 * sometimes needed to rebalance the tree after modifications
*/
static void do_rotate(struct rbtree *s, struct rb_node *n, int side)
{
	struct rb_node *opp = n->rb_child[1 ^ side];

	if ( (n->rb_child[1 ^ side] = opp->rb_child[0 ^ side]) )
		opp->rb_child[0 ^ side]->rb_parent = n;
	opp->rb_child[0 ^ side] = n;

	if ( (opp->rb_parent = n->rb_parent) ) {
		if ( n == n->rb_parent->rb_child[0 ^ side] ) {
			n->rb_parent->rb_child[0 ^ side] = opp;
		}else{
			n->rb_parent->rb_child[1 ^ side] = opp;
		}
	}else{
		s->rbt_root = opp;
	}
	n->rb_parent = opp;
}

/* Re-balance the tree after an insertion */
void rbtree_insert_rebalance(struct rbtree *s, struct rb_node *n)
{
	struct rb_node *parent, *gparent, *uncle;
	int side;

	while ( (parent = n->rb_parent) ) {

		/* Recursion termination, the tree is balanced */
		if ( parent->rb_color == COLOR_BLACK )
			break;

		/* When your structures have symmetry, your code can
		 * be half the size!
		 */
		gparent = parent->rb_parent;
		side = (parent == gparent->rb_child[1]);
		uncle = gparent->rb_child[1 ^ side];

		/* Check to see if we can live with just recoloring */
		if ( uncle && (uncle->rb_color == COLOR_RED) ) {
			gparent->rb_color = COLOR_RED;
			parent->rb_color = COLOR_BLACK;
			uncle->rb_color = COLOR_BLACK;
			n = gparent;
			continue;
		}

		/* Check to see if we need to do double rotation */
		if ( n == parent->rb_child[1 ^ side] ) {
			struct rb_node *t;

			do_rotate(s, parent, 0 ^ side);
			t = parent;
			parent = n;
			n = t;
		}

		/* If not, we do a single rotation */
		parent->rb_color = COLOR_BLACK;
		gparent->rb_color = COLOR_RED;
		do_rotate(s, gparent, 1 ^ side);
	}

	s->rbt_root->rb_color = COLOR_BLACK;
}

/* Re-balance a tree after deletion, probably the most complex bit... */
static void delete_rebalance(struct rbtree *s,
				struct rb_node *n, struct rb_node *parent)
{
	struct rb_node *other;
	int side;
	
	while ( ((n == NULL) || n->rb_color == COLOR_BLACK) ) {
		if ( n == s->rbt_root)
			break;

		side = (parent->rb_child[1] == n);

		other = parent->rb_child[1 ^ side];

		if ( other->rb_color == COLOR_RED ) {
			other->rb_color = COLOR_BLACK;
			parent->rb_color = COLOR_RED;
			do_rotate(s, parent, 0 ^ side);
			other = parent->rb_child[1 ^ side];
		}

		if ( ((other->rb_child[0 ^ side] == NULL) ||
			(other->rb_child[0 ^ side]->rb_color == COLOR_BLACK)) &&
			((other->rb_child[1 ^ side] == NULL) ||
			(other->rb_child[1 ^ side]->rb_color == COLOR_BLACK)) ) {
			other->rb_color = COLOR_RED;
			n = parent;
			parent = n->rb_parent;
		}else{
			if ( (other->rb_child[1 ^ side] == NULL) ||
			(other->rb_child[1 ^ side]->rb_color == COLOR_BLACK) ) {
				struct rb_node *opp;

				if ( (opp = other->rb_child[0 ^ side]) )
					opp->rb_color = COLOR_BLACK;

				other->rb_color = COLOR_RED;
				do_rotate(s, other, 0 ^ side);
				other = parent->rb_child[1 ^ side];
			}

			other->rb_color = parent->rb_color;
			parent->rb_color = COLOR_BLACK;
			if ( other->rb_child[1 ^ side] )
				other->rb_child[1 ^ side]->rb_color = COLOR_BLACK;
			do_rotate(s, parent, 0 ^ side);
			n = s->rbt_root;
			break;
		}
	}

	if ( n )
		n->rb_color = COLOR_BLACK;
}

void rbtree_delete_node(struct rbtree *s, struct rb_node *n)
{
	struct rb_node *child, *parent;
	int color;

	if ( n->rb_child[0] && n->rb_child[1] ) {
		struct rb_node *old = n, *lm;

		/* If we have 2 children, go right, and then find the leftmost
		 * node in that subtree, this is the one to swap in to replace
		 * our deleted node
		 */
		n = n->rb_child[1];
		while ( (lm = n->rb_child[0]) != NULL )
			n = lm;

		child = n->rb_child[1];
		parent = n->rb_parent;
		color = n->rb_color;

		if ( child )
			child->rb_parent = parent;

		if ( parent ) {
			if ( parent->rb_child[0] == n )
				parent->rb_child[0] = child;
			else
				parent->rb_child[1] = child;
		}else
			s->rbt_root = child;

		if ( n->rb_parent == old )
			parent = n;

		n->rb_parent = old->rb_parent;
		n->rb_color = old->rb_color;
		n->rb_child[0] = old->rb_child[0];
		n->rb_child[1] = old->rb_child[1];

		if ( old->rb_parent ) {
			if ( old->rb_parent->rb_child[0] == old )
				old->rb_parent->rb_child[0] = n;
			else
				old->rb_parent->rb_child[1] = n;
		}else
			s->rbt_root = n;

		old->rb_child[0]->rb_parent = n;
		if ( old->rb_child[1] )
			old->rb_child[1]->rb_parent = n;

		goto rebalance;
	}

	if ( n->rb_child[0] == NULL ) {
		child = n->rb_child[1];
	}else if ( n->rb_child[1] == NULL ) {
		child = n->rb_child[0];
	}

	parent = n->rb_parent;
	color = n->rb_color;

	if ( child )
		child->rb_parent = parent;

	if ( parent ) {
		if ( parent->rb_child[0] == n )
			parent->rb_child[0] = child;
		else
			parent->rb_child[1] = child;
	}else
		s->rbt_root = child;

rebalance:
	if ( color == COLOR_BLACK )
		delete_rebalance(s, child, parent);
}

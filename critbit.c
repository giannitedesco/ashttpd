/* crit-bit tree, originally taken from djb's qhasm
 * modified by jason davies to be a generic library
 * modified by gianni tedesco 2013 to be a map rather than set
*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <critbit.h>

struct _cb_node {
	union {
		struct {
			struct _cb_node *child[2];
			uint32_t byte;
			uint8_t otherbits;
		}internal;
		struct {
			char *key;
			void *val;
		}leaf;
	}u;
	uint8_t is_leaf;
};

int cb_contains(struct cb_tree * t, const char *u, void **ppv)
{
	const size_t ulen = strlen(u);
	struct _cb_node *q;
	int direction;

	if (NULL == t->root)
		return 0;

	for(q = t->root; !q->is_leaf; q = q->u.internal.child[direction]) {
		uint8_t c = 0;

		if (q->u.internal.byte < ulen)
			c = u[q->u.internal.byte];
		direction = (1 + (q->u.internal.otherbits | c)) >> 8;
	}

	if ( strcmp(u, q->u.leaf.key) )
		return 0;

	if ( ppv )
		*ppv = q->u.leaf.val;

	return 1;
}

int cb_insert(struct cb_tree *t, const char *u, void ***pppv)
{
	const size_t ulen = strlen(u);
	struct _cb_node *q;
	uint32_t newotherbits;
	uint32_t newbyte;
	int direction;

	/* deal with empty tree */
	if (NULL == t->root) {
		struct _cb_node *n;

		n = malloc(sizeof(*n));
		if ( NULL == n )
			return 0;

		n->is_leaf = 1;
		n->u.leaf.key = strdup(u);
		if ( NULL == n->u.leaf.key ) {
			free(n);
			return 0;
		}
		t->root = n;
		n->u.leaf.val = NULL;
		if ( pppv )
			*pppv = &n->u.leaf.val;
		return 2;
	}

	for(q = t->root; !q->is_leaf; q = q->u.internal.child[direction]) {
		uint8_t c = 0;

		if (q->u.internal.byte < ulen)
			c = u[q->u.internal.byte];

		direction = (1 + (q->u.internal.otherbits | c)) >> 8;
	}

	for (newbyte = 0; newbyte < ulen; ++newbyte) {
		if (q->u.leaf.key[newbyte] != u[newbyte]) {
			newotherbits = q->u.leaf.key[newbyte] ^ u[newbyte];
			goto different_byte_found;
		}
	}

	if (q->u.leaf.key[newbyte] != 0) {
		newotherbits = q->u.leaf.key[newbyte];
		goto different_byte_found;
	}

	if ( pppv )
		*pppv = &q->u.leaf.val;
	return 1;

different_byte_found:
	newotherbits |= newotherbits >> 1;
	newotherbits |= newotherbits >> 2;
	newotherbits |= newotherbits >> 4;
	newotherbits = (newotherbits & ~(newotherbits >> 1)) ^ 255;
	uint8_t c = q->u.leaf.key[newbyte];
	int newdirection = (1 + (newotherbits | c)) >> 8;

	struct _cb_node *newnode, *newleaf;
	newnode = malloc(sizeof(*newnode));
	if ( NULL == newnode )
		return 0;
	newleaf = malloc(sizeof(*newnode));
	if ( NULL == newleaf ) {
		free(newnode);
		return 0;
	}

	newleaf->is_leaf = 1;
	newleaf->u.leaf.key = strdup(u);
	newleaf->u.leaf.val = NULL;
	if ( NULL == newleaf->u.leaf.key ) {
		free(newleaf);
		free(newnode);
		return 0;
	}

	newnode->u.internal.byte = newbyte;
	newnode->u.internal.otherbits = newotherbits;
	newnode->u.internal.child[1 - newdirection] = newleaf;
	newnode->is_leaf = 0;

	struct _cb_node **wherep = &t->root;
	for (;;) {
		q = *wherep;
		if ( q->is_leaf )
			break;
		if (q->u.internal.byte > newbyte)
			break;
		if (q->u.internal.byte == newbyte &&
				q->u.internal.otherbits > newotherbits)
			break;
		uint8_t c = 0;
		if (q->u.internal.byte < ulen)
			c = u[q->u.internal.byte];
		const int direction = (1 + (q->u.internal.otherbits | c)) >> 8;
		wherep = &q->u.internal.child[direction];
	}

	newnode->u.internal.child[newdirection] = *wherep;
	*wherep = newnode;
	if ( pppv )
		*pppv = &newleaf->u.leaf.val;

	return 2;
}

int cb_delete(struct cb_tree * t, const char *u, void **ppv)
{
	const size_t ulen = strlen(u);
	struct _cb_node *p, *q;
	struct _cb_node **wherep = &t->root;
	struct _cb_node **whereq = 0;
	int direction = 0;

	if (NULL == t->root)
		return 0;

	for(p = t->root, q = NULL; !p->is_leaf; ) {
		uint8_t c = 0;
		whereq = wherep;
		q = p;
		if (q->u.internal.byte < ulen)
			c = u[q->u.internal.byte];
		direction = (1 + (q->u.internal.otherbits | c)) >> 8;
		wherep = q->u.internal.child + direction;
		p = *wherep;
	}

	if (0 != strcmp(u, p->u.leaf.key))
		return 0;

	if ( ppv )
		*ppv = p->u.leaf.val;
	free(p->u.leaf.key);
	free(p);

	if (!whereq) {
		t->root = NULL;
		return 1;
	}

	*whereq = q->u.internal.child[1 - direction];
	free(q);
	return 1;
}

static void traverse(struct _cb_node *n, void (*cb)(void *priv))
{
	if (n->is_leaf) {
		if ( cb )
			(*cb)(n->u.leaf.val);
		free(n->u.leaf.key);
	}else{
		traverse(n->u.internal.child[0], cb);
		traverse(n->u.internal.child[1], cb);
	}
	free(n);
}

void cb_free(struct cb_tree *t, void (*cb)(void *priv))
{
	if (t->root) {
		traverse(t->root, cb);
		t->root = NULL;
	}
}

#if 0
static int
allprefixed_traverse(uint8_t * top,
		     int (*handle) (const char *, void *), void *arg)
{
	if (1 & (intptr_t) top) {
		_cb_node *q = (void *)(top - 1);
		int direction;

		for (direction = 0; direction < 2; ++direction)
			switch (allprefixed_traverse
				(q->child[direction], handle, arg)) {
			case 1:
				break;
			case 0:
				return 0;
			default:
				return -1;
			}
		return 1;
	}

	return handle((const char *)top, arg);	/*:27 */
}

int
cb_allprefixed(cb_tree * t, const char *prefix,
		     int (*handle) (const char *, void *), void *arg)
{
	const uint8_t *ubytes = (void *)prefix;
	const size_t ulen = strlen(prefix);
	uint8_t *p = t->root;
	uint8_t *top = p;
	size_t i;

	if (!p)
		return 1;

	while (1 & (intptr_t) p) {
		_cb_node *q = (void *)(p - 1);
		uint8_t c = 0;
		if (q->u.internal.byte < ulen)
			c = ubytes[q->u.internal.byte];
		const int direction = (1 + (q->u.internal.otherbits | c)) >> 8;
		p = q->child[direction];
		if (q->u.internal.byte < ulen)
			top = p;
	}

	for (i = 0; i < ulen; ++i) {
		if (p[i] != ubytes[i])
			return 1;
	}

	return allprefixed_traverse(top, handle, arg);
}
#endif

#if MAIN
int main(int argc, char **argv)
{
	unsigned int i;
	char buf[8192];
	struct cb_tree tree = {NULL,};

	for(i = 0; fgets(buf, sizeof(buf), stdin); i++ ) {
		char *ptr;
		ptr = strchr(buf, '\r');
		if ( NULL == ptr )
			ptr = strchr(buf, '\n');
		if ( NULL == ptr ) {
			fprintf(stderr, "line too long\n");
			continue;
		}
		*ptr = '\0';

		cb_insert(&tree, buf);
	}

	printf("%u items inserted\n", i);

	rewind(stdin);
	for(i = 0; fgets(buf, sizeof(buf), stdin); i++ ) {
		char *ptr;
		ptr = strchr(buf, '\r');
		if ( NULL == ptr )
			ptr = strchr(buf, '\n');
		if ( NULL == ptr ) {
			fprintf(stderr, "line too long\n");
			continue;
		}
		*ptr = '\0';

		if ( cb_contains(&tree, buf) )
			cb_delete(&tree, buf);
	}
	printf("%u items removed\n", i);

//	cb_free(&tree);
	return EXIT_SUCCESS;
}
#endif

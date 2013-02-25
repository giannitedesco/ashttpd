#ifndef _CRITBIT_H
#define _CRITBIT_H

struct cb_tree {
	struct _cb_node *root;
};

int cb_contains(struct cb_tree *t, const char *u, void **ppv);
int cb_insert(struct cb_tree *t, const char *u, void ***pppv);
int cb_delete(struct cb_tree *t, const char *u, void **ppv);
void cb_free(struct cb_tree *t, void (*cb)(void *priv));

#endif /* _CRITBIT_H */

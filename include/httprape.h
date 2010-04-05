#ifndef _HTTPRAPE_H
#define _HTTPRAPE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <compiler.h>
#include <list.h>
#include <nbio.h>
#include <vec.h>
#include <os.h>

struct mnode {
	struct ro_vec	n_uri;
	unsigned int	n_num_sub;
	unsigned int	n_num_edges;
	unsigned int	n_edge_prob_bits;
	unsigned int 	n_edge_prob_max;
	struct ro_vec	*n_ancillary;
	const struct medge *n_edges;
};

struct medge {
	unsigned int	e_prob_max;
	const struct mnode	*e_node;
};

extern const struct mnode *markov_root;

#endif /* _HTTPRAPE_H */

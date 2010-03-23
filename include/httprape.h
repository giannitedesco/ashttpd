#ifndef _ASHTTPD_H
#define _ASHTTPD_H

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

struct node {
	struct ro_vec	n_uri;
	unsigned int	n_num_sub;
	unsigned int	n_num_edges;
	struct ro_vec	*n_sub;
	struct edge	*n_edges;
};

struct edge {
	uint16_t	e_prob_min;
	uint16_t	e_prob_max;
	struct node	e_node;
};

#endif /* _ASHTTPD_H */

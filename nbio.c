/*
 * This file is part of Firestorm NIDS
 * Copyright (c) 2004 Gianni Tedesco
 * Released under the terms of the GNU GPL version 2
 *
 * Functions:
 *  o eventloop_find() - Find an eventloop plugin by name
 *  o eventloop_add() - Register an eventloop plugin
 *  o eventloop_load() - Load all nbio plugins
 *  o nbio_init() - Initialise nbio
 *  o nbio_fini() - Deinitialise nbio
 *  o nbio_pump() - Pump events
 *  o nbio_add() - Register an fd with read/write/error callbacks
 *  o nbio_del() - Remove an fd
*/
#include <compiler.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <list.h>
#include <assert.h>
#include <nbio.h>

static struct eventloop *ev_list;

#define NBIO_DELETED 0x8000

struct eventloop *eventloop_find(const char *name)
{
	struct eventloop *e;

	for(e=ev_list; e; e=e->next)
		if ( !strcmp(e->name, name) )
			break;

	return e;
}

void eventloop_add(struct eventloop *e)
{
	if ( eventloop_find(e->name) ) {
		fprintf(stderr, "eventloop: '%s' eventloop is "
			"already registered\n", e->name);
		return;
	}

	e->next = ev_list;
	ev_list = e;
}

int nbio_init(struct iothread *t, const char *plugin)
{
	if ( NULL == plugin ) {
		if ( NULL == ev_list ) {
			fprintf(stderr, "nbio: No eventloop plugins\n");
			return 0;
		}
		t->plugin = ev_list;
	}else{
		t->plugin = eventloop_find(plugin);
	}

	if ( NULL == t->plugin )
		return 0;

	for ( ; !t->plugin->init(t); t->plugin = t->plugin->next ) {
		if ( plugin || NULL == t->plugin->next )
			return 0;
	}

	INIT_LIST_HEAD(&t->active);
	INIT_LIST_HEAD(&t->inactive);
	INIT_LIST_HEAD(&t->deleted);
	return 1;
}

void nbio_fini(struct iothread *t)
{
	struct nbio *n, *tmp;

	list_for_each_entry_safe(n, tmp, &t->inactive, list) {
		list_move_tail(&n->list, &t->active);
		t->plugin->active(t, n);
	}

	list_for_each_entry_safe(n, tmp, &t->deleted, list)
		list_move_tail(&n->list, &t->active);

	list_for_each_entry_safe(n, tmp, &t->active, list) {
		list_del(&n->list);
		n->ops->dtor(t, n);
	}

	t->plugin->fini(t);
}

void nbio_pump(struct iothread *t, int mto)
{
	struct nbio *n, *tmp;
	struct nbio *d, *tmp2;

	while ( !list_empty(&t->active) ) {
		list_for_each_entry_safe(n, tmp, &t->active, list) {
			assert(NBIO_DELETED != n->mask);

			if ( n->flags & NBIO_ERROR ) {
				n->mask = NBIO_DELETED;
				n->flags = 0;
				list_move_tail(&n->list, &t->deleted);
				continue;
			}

			if ( (n->flags & n->mask) & NBIO_READ )
				n->ops->read(t, n);
			if ( (n->flags & n->mask) & NBIO_WRITE )
				n->ops->write(t, n);
		}
	}

	list_for_each_entry_safe(d, tmp2, &t->deleted, list) {
		list_del(&d->list);
		d->ops->dtor(t, d);
	}

	if ( !list_empty(&t->inactive) )
		t->plugin->pump(t, mto);
}

void nbio_add(struct iothread *t, struct nbio *n, unsigned short wait)
{
	INIT_LIST_HEAD(&n->list);
	nbio_set_wait(t, n, wait);
}

void nbio_del(struct iothread *t, struct nbio *n)
{
	t->plugin->active(t, n);
	n->mask = NBIO_DELETED;
	n->flags = 0;
	list_move_tail(&n->list, &t->deleted);
}

void nbio_inactive(struct iothread *t, struct nbio *n)
{
	list_move_tail(&n->list, &t->inactive);
	t->plugin->inactive(t, n);
}

void nbio_set_wait(struct iothread *t, struct nbio *io, unsigned short wait)
{
	wait &= NBIO_WAIT;
	io->mask = io->flags = wait;
	t->plugin->active(t, io);
	if ( wait == 0 ) {
		/* We can safely go on the inactive list if we are careful
		 * to first set plugin->active() - this allows us to ignore
		 * any events on the FD for now (useful if we cant do anything
		 * with it until another pending i/o has completed)
		 */
		list_move_tail(&io->list, &t->inactive);
	}else{
		list_move_tail(&io->list, &t->active);
	}
}

unsigned short nbio_get_wait(struct nbio *io)
{
	return io->mask & NBIO_WAIT;
}

static void __attribute__((constructor)) _ctor(void)
{
	_eventloop_poll_ctor();
	_eventloop_epoll_ctor();
}

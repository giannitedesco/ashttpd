#ifndef _NBIO_INOTIFY_H
#define _NBIO_INOTIFY_H

typedef struct _nbnotify *nbnotify_t;

_private nbnotify_t nbio_inotify_new(struct iothread *t);
_private int nbio_inotify_watch_dir(nbnotify_t n, const char *dir);

#endif /* _NBIO_INOTIFY_H */

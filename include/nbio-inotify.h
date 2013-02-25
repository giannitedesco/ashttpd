#ifndef _NBIO_INOTIFY_H
#define _NBIO_INOTIFY_H

typedef struct _nbnotify *nbnotify_t;

struct watch_ops {
	void (*create)(void *priv, const char *name, unsigned isdir);
	void (*delete)(void *priv, const char *name, unsigned isdir);
	void (*moved_from)(void *priv, const char *name, unsigned isdir);
	void (*moved_to)(void *priv, const char *name, unsigned isdir);
	void (*move_self)(void *priv);
	void (*delete_self)(void *priv);
	void (*dtor)(void *priv);
};

_private nbnotify_t nbio_inotify_new(struct iothread *t);
_private int nbio_inotify_watch_dir(nbnotify_t n, const char *dir,
					const struct watch_ops *ops,
					void *priv);

#endif /* _NBIO_INOTIFY_H */

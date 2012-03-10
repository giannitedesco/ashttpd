#include <compiler.h>

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <assert.h>

#include <magic.h>

#include <list.h>
#include <hgang.h>
#include <strpool.h>
#include <fobuf.h>
#include <os.h>
#include <vec.h>
#include "trie.h"

#define WRITE_FILES	1
#define BUFFER_SIZE	(1U << 20U)

static const char *cmd = "mkroot";
static int dotfiles; /* whether to include dot files */

struct mime_type {
	struct list_head m_list;
	const char *m_type;
};

#define OBJ_TYPE_FILE		0
#define OBJ_TYPE_REDIRECT	1
struct object {
	struct list_head o_list;
	gidx_oid_t o_oid;
	unsigned int o_type;
	union {
		struct {
			struct mime_type *type;
			uint64_t size;
			char *path;
		} file;
		struct {
			char *uri;
		} redirect;
	}o_u;
};

struct uri {
	struct list_head u_list;
	struct object *u_obj;
	char *u_uri;
};

struct webroot {
	struct list_head r_uri;
	struct list_head r_redirect;
	struct list_head r_file;
	struct list_head r_mime_type;
	hgang_t r_uri_mem;
	hgang_t r_obj_mem;
	hgang_t r_mime_mem;
	strpool_t r_str_mem;
	const char *r_base;
	trie_t r_trie;
	magic_t r_magic;
	uint64_t r_files_sz;
	unsigned int r_num_uri;
	unsigned int r_num_redirect;
	unsigned int r_num_file;
};

static char *path_splice(const char *dir, const char *path)
{
	size_t olen;
	char *ret;

	olen = snprintf(NULL, 0, "%s/%s", dir, path);

	ret = malloc(olen + 1);
	if ( NULL == ret ) {
		fprintf(stderr, "%s: calloc: %s\n", cmd, os_err());
		return NULL;
	}

	if ( path[0] == '/' )
		snprintf(ret, olen + 1, "%s%s", dir, path);
	else
		snprintf(ret, olen + 1, "%s/%s", dir, path);

	return ret;
}

static struct webroot *webroot_new(const char *base)
{
	struct webroot *r;

	r = calloc(1, sizeof(*r));
	if ( NULL == r)
		goto out;

	r->r_uri_mem = hgang_new(sizeof(struct uri), 0);
	if ( NULL == r->r_uri_mem )
		goto out_free;

	r->r_obj_mem = hgang_new(sizeof(struct object), 0);
	if ( NULL == r->r_obj_mem )
		goto out_free_uri;

	r->r_mime_mem = hgang_new(sizeof(struct mime_type), 0);
	if ( NULL == r->r_mime_mem )
		goto out_free_obj;

	r->r_str_mem = strpool_new(0);
	if ( NULL == r->r_str_mem )
		goto out_free_mime;

	r->r_magic = magic_open(MAGIC_MIME_TYPE|MAGIC_MIME_ENCODING);
	if ( NULL == r->r_magic )
		goto out_free_str;

	if ( magic_load(r->r_magic, NULL) )
		goto out_free_magic;

	INIT_LIST_HEAD(&r->r_uri);
	INIT_LIST_HEAD(&r->r_redirect);
	INIT_LIST_HEAD(&r->r_file);
	INIT_LIST_HEAD(&r->r_mime_type);
	r->r_base = base;
	goto out;

out_free_magic:
	magic_close(r->r_magic);
out_free_str:
	strpool_free(r->r_str_mem);
out_free_mime:
	hgang_free(r->r_mime_mem);
out_free_obj:
	hgang_free(r->r_obj_mem);
out_free_uri:
	hgang_free(r->r_uri_mem);
out_free:
	free(r);
	r = NULL;
out:
	return r;
}

static void webroot_free(struct webroot *r)
{
	if ( r ) {
		hgang_free(r->r_uri_mem);
		hgang_free(r->r_obj_mem);
		hgang_free(r->r_mime_mem);
		strpool_free(r->r_str_mem);
		magic_close(r->r_magic);
		trie_free(r->r_trie);
		free(r);
	}
}

static int ucmp(const void *A, const void *B)
{
	const struct uri * const *a = A;
	const struct uri * const *b = B;
	return strcmp((*a)->u_uri, (*b)->u_uri);
}

static int sort_uris(struct webroot *r)
{
	struct uri *u, *tmp, **s;
	unsigned int i = 0;

	s = malloc(r->r_num_uri * sizeof(*s));
	if ( NULL == s )
		return 0;

	list_for_each_entry_safe(u, tmp, &r->r_uri, u_list) {
		s[i++] = u;
		list_del(&u->u_list);
	}
	assert(i == r->r_num_uri);
	assert(list_empty(&r->r_uri));

	qsort(s, r->r_num_uri, sizeof(*s), ucmp);

	for(i = 0; i < r->r_num_uri; i++) {
		list_add_tail(&s[i]->u_list, &r->r_uri);
	}

	free(s);
	return 1;
}

static int sort_objects(struct webroot *r)
{
	struct object *obj;
	gidx_oid_t oid = 0;

	list_for_each_entry(obj, &r->r_redirect, o_list) {
		obj->o_oid = oid++;
	}

	list_for_each_entry(obj, &r->r_file, o_list) {
		obj->o_oid = oid++;
	}

	return 1;
}

static int webroot_prep(struct webroot *r)
{
	struct trie_entry *ent;
	struct object *obj;
	unsigned int i = 0;
	struct uri *u;
	int ret = 0;
	trie_t t;

	if ( !sort_uris(r) )
		goto out;

	if ( !sort_objects(r) )
		goto out;

	ent = malloc(sizeof(*ent) * r->r_num_uri);
	if ( NULL == ent )
		goto out;

	list_for_each_entry(u, &r->r_uri, u_list) {
		ent[i].t_str.v_ptr = (const uint8_t *)u->u_uri;
		ent[i].t_str.v_len = strlen(u->u_uri);
		ent[i].t_oid = u->u_obj->o_oid;
		i++;
	}

	t = trie_new(ent, r->r_num_uri);
	if ( NULL == t )
		goto out_free;

	printf("%s: index: trie=%"PRId64" bytes, strtab=%"PRId64" bytes\n",
		cmd, trie_trie_size(t), trie_strtab_size(t));
	r->r_trie = t;

	r->r_files_sz = 0;
	list_for_each_entry(obj, &r->r_file, o_list) {
		r->r_files_sz += obj->o_u.file.size;
	}

	ret = 1;
out_free:
	free(ent);
out:
	return ret;
}

static int write_file(struct object *f, fobuf_t out)
{
	uint8_t buf[BUFFER_SIZE];
	ssize_t ret;
	int rc = 0;
	int fd;

	fd = open(f->o_u.file.path, O_RDONLY);
	if ( fd < 0 ) {
		fprintf(stderr, "%s: %s: open: %s\n",
			cmd, f->o_u.file.path, os_err());
		goto out;
	}

again:
	ret = read(fd, buf, sizeof(buf));
	if ( ret < 0 ) {
		fprintf(stderr, "%s: %s: read: %s\n",
			cmd, f->o_u.file.path, os_err());
		goto out_close;
	}

	if ( ret && !fobuf_write(out, buf, ret) ) {
		fprintf(stderr, "%s: %s: write: %s\n",
			cmd, f->o_u.file.path, os_err());
		goto out_close;
	}

	if ( ret )
		goto again;

	rc = 1;
out_close:
	fd_close(fd);
out:
	return rc;
}

static int write_files(struct webroot *r, fobuf_t out)
{
	struct object *obj;
	list_for_each_entry(obj, &r->r_file, o_list) {
		if ( !write_file(obj, out) )
			return 0;
	}
	return 1;
}

static int write_mimetab(struct webroot *r, fobuf_t out)
{
	struct mime_type *m;

	list_for_each_entry(m, &r->r_mime_type, m_list) {
		if ( !fobuf_write(out, m->m_type, strlen(m->m_type)) )
			return 0;
	}

	return 1;
}

static int webroot_write(struct webroot *r, fobuf_t out)
{
	/* TODO: write header */
	if ( !trie_write_trie(r->r_trie, out) )
		return 0;
	if ( !trie_write_strtab(r->r_trie, out) )
		return 0;
	/* TODO: write object descriptions */
	if ( !write_mimetab(r, out) )
		return 0;

#if WRITE_FILES
	printf("%s: Writing files\n", cmd);
	if ( !write_files(r, out) )
		return 0;
#endif

	return 1;
}

static char *webroot_lookup(struct webroot *r, const char *path)
{
	return path_splice(r->r_base, path);
}

static char *webroot_strdup(struct webroot *r, const char *str)
{
	size_t len = strlen(str) + 1;
	char *ret;

	ret = strpool_alloc(r->r_str_mem, len);
	if ( NULL == ret )
		return ret;

	memcpy(ret, str, len);
	return ret;
}

static struct uri *webroot_link(struct webroot *r,
				const char *link, struct object *target)
{
	struct uri *uri;

	if ( !strlen(link) )
		link = "/";

	uri = hgang_alloc0(r->r_uri_mem);
	if ( NULL == uri )
		return NULL;

	uri->u_uri = webroot_strdup(r, link);
	if ( NULL == r ) {
		hgang_return(r->r_uri_mem, uri);
		return NULL;
	}

	uri->u_obj = target;
	list_add_tail(&uri->u_list, &r->r_uri);
	r->r_num_uri++;
	return uri;
}

static uint64_t webroot_output_size(struct webroot *r)
{
	return r->r_files_sz +
		trie_trie_size(r->r_trie) +
		trie_strtab_size(r->r_trie);
}

static struct mime_type *mime_add(struct webroot *r, const char *mime)
{
	struct mime_type *m;

	list_for_each_entry(m, &r->r_mime_type, m_list) {
		if ( !strcmp(m->m_type, mime) )
			return m;
	}

	m = hgang_alloc0(r->r_mime_mem);
	if ( NULL == m )
		return NULL;

	m->m_type = webroot_strdup(r, mime);
	if ( NULL == m->m_type ) {
		hgang_return(r->r_mime_mem, m);
		return NULL;
	}

	list_add(&m->m_list, &r->r_mime_type);

	return m;
}

static struct object *obj_redirect(struct webroot *r, const char *path)
{
	struct object *obj;

	obj = hgang_alloc0(r->r_obj_mem);
	if ( NULL == obj )
		return NULL;

	obj->o_type = OBJ_TYPE_REDIRECT;

	obj->o_u.redirect.uri = webroot_strdup(r, path);
	if ( NULL == obj->o_u.redirect.uri ) {
		hgang_return(r->r_obj_mem, obj);
		return NULL;
	}

	list_add_tail(&obj->o_list, &r->r_redirect);
	r->r_num_redirect++;

	return obj;
}

/* TODO: detect hardlinks */
static struct object *obj_file(struct webroot *r,
				const char *path,
				uint64_t size)
{
	struct mime_type *m;
	struct object *obj;
	const char *mime;

	obj = hgang_alloc0(r->r_obj_mem);
	if ( NULL == obj )
		return NULL;

	obj->o_type = OBJ_TYPE_FILE;

	mime = magic_file(r->r_magic, path);
	m = mime_add(r, mime);
	if ( NULL == m ) {
		hgang_return(r->r_obj_mem, obj);
		return NULL;
	}

	obj->o_u.file.path = webroot_strdup(r, path);
	if ( NULL == obj->o_u.file.path ) {
		hgang_return(r->r_obj_mem, obj);
		return NULL;
	}

	obj->o_u.file.size = size;
	obj->o_u.file.type = m;

	list_add_tail(&obj->o_list, &r->r_file);
	r->r_num_file++;

	return obj;
}

static int scan_item(struct webroot *r, const char *dir);

static int scan_dir(struct webroot *r, const char *dir, const char *path)
{
	struct dirent *e;
	int ret = 0;
	DIR *d;

	d = opendir(path);
	if ( NULL == d ) {
		fprintf(stderr, "%s: %s: opendir: %s\n", cmd, path, os_err());
		goto out;
	}

	while( (e = readdir(d)) ) {
		char *uri;
		if ( !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") )
			continue;
		if ( !dotfiles && e->d_name[0] == '.' )
			continue;

		uri = path_splice(dir, e->d_name);
		ret = scan_item(r, uri);
		free(uri);
		if ( !ret )
			goto out;
	}

	closedir(d);
	ret = 1;
out:
	return ret;
}

static int scan_item(struct webroot *r, const char *u)
{
	struct object *obj;
	struct stat st;
	char *path;
	int ret = 0;

	path = webroot_lookup(r, u);
	if ( NULL == path )
		goto out;

	if ( lstat(path, &st) ) {
		fprintf(stderr, "%s: %s: stat: %s\n", cmd, path, os_err());
		goto out;
	}

	if ( S_ISDIR(st.st_mode) ) {
		char *dpath;

		dpath = path_splice(u, "/");
		if ( NULL == dpath )
			goto out_free;

		obj = obj_redirect(r, dpath);
		free(dpath);
		if ( NULL == obj )
			goto out_free;

		if ( !scan_dir(r, u, path) )
			goto out_free;

		/* TODO: add index page */
	}else if ( S_ISREG(st.st_mode) ) {
		obj = obj_file(r, path, st.st_size);
		if ( NULL == obj )
			goto out_free;
	}else if ( S_ISLNK(st.st_mode) ) {
		/* TODO: Add redirect */
		ret = 1;
		goto out_free;
	}else {
		printf("%s: unhandled type: %s\n", cmd, path);
		ret = 1;
		goto out_free;
	}

	if ( NULL == webroot_link(r, u, obj) )
		goto out_free;

	ret = 1;
out_free:
	free(path);
out:
	return ret;
}

static int do_mkroot(const char *dir, const char *outfn)
{
	struct webroot *r;
	fobuf_t out;
	int ret = 0;
	int fd;

	r = webroot_new(dir);
	if ( NULL == r ) {
		fprintf(stderr, "%s: webroot_new: %s\n", cmd, os_err());
		goto out;
	}

	printf("%s: scanning %s\n", cmd, dir);
	if ( !scan_item(r, "") )
		goto out_free;

	if ( !webroot_prep(r) )
		goto out_free;

	printf("%s: num_uri=%u num_redirect=%u num_file=%u\n",
		cmd, r->r_num_uri, r->r_num_redirect, r->r_num_file);

	fd = open(outfn, O_WRONLY|O_CREAT|O_TRUNC, 0600);
	if ( fd < 0 ) {
		fprintf(stderr, "%s: %s: open: %s\n", cmd, outfn, os_err());
		goto out_free;
	}

#if WRITE_FILES
	if ( posix_fallocate(fd, 0, webroot_output_size(r)) ) {
		fprintf(stderr, "%s: %s: fallocate: %s\n",
			cmd, outfn, os_err());
		goto out_close;
	}
#endif

	out = fobuf_new(fd, BUFFER_SIZE);
	if ( NULL == out )
		goto out_close;

	if ( !webroot_write(r, out) ) {
		fobuf_abort(out);
		goto out;
	}

	if ( !fobuf_close(out) )
		goto out_close;

	ret = 1;

out_close:
	fd_close(fd);
out_free:
	webroot_free(r);
out:
	return ret;
}

static _noreturn void usage(const char *msg, int e)
{
	fprintf(stderr, "%s: Usage\n", cmd);
	fprintf(stderr, "\t%s [dir] [output]\n", cmd);
	exit(e);
}

static void rstrip_slashes(char *path)
{
	int i, len = strlen(path);
	for(len = strlen(path), i = len - 1; i >= 0; --i) {
		if ( path[i] != '/' )
			return;
		path[i] = '\0';
	}
}

int main(int argc, char **argv)
{
	const char *dir, *outfn;

	if ( argc )
		cmd = argv[0];

	if ( argc < 3 )
		usage(NULL, EXIT_FAILURE);

	rstrip_slashes(argv[1]);
	dir = argv[1];
	outfn = argv[2];

	if ( !do_mkroot(dir, outfn) ) {
		fprintf(stderr, "%s: FAILED\n", cmd);
		return EXIT_FAILURE;
	}

	printf("%s: OK\n", cmd);
	return EXIT_SUCCESS;
}

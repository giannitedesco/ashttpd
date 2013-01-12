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
#include <webroot-format.h>
#include "trie.h"

#define WRITE_FILES	1
#define BUFFER_SIZE	(1U << 20U)

#if 0
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
static const char * const index_pages[] = {
	"index.html",
	"default.html",
};

static const char *cmd = "mkroot";
static int dotfiles; /* whether to include dot files */

struct mime_type {
	struct list_head m_list;
	const char *m_type;
	uint32_t m_strtab_off;
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
			uint64_t off;
			dev_t dev;
			ino_t ino;
		} file;
		struct {
			char *uri;
			uint32_t strtab_off;
			int code;
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
	struct trie_entry *r_trie_ent;
	magic_t r_magic;
	uint64_t r_files_sz;
	unsigned int r_num_uri;
	unsigned int r_num_redirect;
	unsigned int r_num_file;
	unsigned int r_mimetab_sz;
	unsigned int r_redirtab_sz;
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
		free(r->r_trie_ent);
		trie_free(r->r_trie);
		free(r);
	}
}

static int ucmp(const void *A, const void *B)
{
	const struct uri * const *a = A;
	const struct uri * const *b = B;
#if 0
	size_t min, i;
	int ret;

	min = (strlen((*a)->u_uri) < strlen((*b)->u_uri)) ? strlen((*a)->u_uri) : strlen((*b)->u_uri);
	ret = strlen((*a)->u_uri) - strlen((*b)->u_uri);

	for(i = 0; i < min; i++) {
		int ret;

		ret = (*a)->u_uri[i] - (*b)->u_uri[i];
		if ( ret )
			return ret;
	}
	return ret;
#else
	return strcmp((*a)->u_uri, (*b)->u_uri);
#endif
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
	struct mime_type *m;
	struct trie_entry *ent;
	struct object *obj;
	unsigned int i = 0;
	struct uri *u;
	uint64_t off;
	int ret = 0;
	trie_t t;

	if ( !sort_uris(r) )
		goto out;

	if ( !sort_objects(r) )
		goto out;

	ent = malloc(sizeof(*ent) * r->r_num_uri);
	if ( NULL == ent )
		goto out;

	/* Create the trie index */
	list_for_each_entry(u, &r->r_uri, u_list) {
		ent[i].t_str.v_ptr = (const uint8_t *)u->u_uri;
		ent[i].t_str.v_len = strlen(u->u_uri);
		ent[i].t_oid = u->u_obj->o_oid;
//		printf(" %d: %.*s (%d)\n", i,
//			(int)ent[i].t_str.v_len,
//			ent[i].t_str.v_ptr,
//			ent[i].t_oid);
		i++;
	}

	t = trie_new(ent, r->r_num_uri);
	if ( NULL == t )
		goto out_free;

	printf("%s: index: trie=%"PRId64" bytes, strtab=%"PRId64" bytes\n",
		cmd, trie_trie_size(t), trie_strtab_size(t));
	r->r_trie = t;
	r->r_trie_ent = ent;

	/* Layout mime and redirect string tables */
	off = sizeof(struct webroot_hdr) +
		trie_trie_size(r->r_trie) +
		sizeof(struct webroot_redirect) * r->r_num_redirect +
		sizeof(struct webroot_file) * r->r_num_file +
		trie_strtab_size(r->r_trie);

	list_for_each_entry(m, &r->r_mime_type, m_list) {
		m->m_strtab_off = off;
		off += strlen(m->m_type);
		r->r_mimetab_sz += strlen(m->m_type);
	}

	r->r_redirtab_sz = 0;
	list_for_each_entry(obj, &r->r_redirect, o_list) {
		obj->o_u.redirect.strtab_off = off;
		if ( obj->o_u.redirect.uri ) {
			off += strlen(obj->o_u.redirect.uri);
			r->r_redirtab_sz += strlen(obj->o_u.redirect.uri);
		}
	}

	/* Calculate files size */
	r->r_files_sz = 0;
	list_for_each_entry(obj, &r->r_file, o_list) {
		obj->o_u.file.off = off;
		off += obj->o_u.file.size;
		r->r_files_sz += obj->o_u.file.size;
	}

	/* success */
	ret = 1;
	goto out;

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

static int write_redirtab(struct webroot *r, fobuf_t out)
{
	struct object *obj;

	list_for_each_entry(obj, &r->r_redirect, o_list) {
		if ( NULL == obj->o_u.redirect.uri )
			continue;
		if ( !fobuf_write(out, obj->o_u.redirect.uri,
					strlen(obj->o_u.redirect.uri)) )
			return 0;
	}

	return 1;
}

static int write_header(struct webroot *r, fobuf_t out)
{
	struct webroot_hdr hdr;

	hdr.h_num_edges = trie_num_edges(r->r_trie);
	hdr.h_num_redirect = r->r_num_redirect;
	hdr.h_num_file = r->r_num_file;
	hdr.h_strtab_sz = trie_strtab_size(r->r_trie) +
				r->r_mimetab_sz +
				r->r_redirtab_sz;
	hdr.h_magic = WEBROOT_MAGIC;
	hdr.h_vers = WEBROOT_CURRENT_VER;

	/* provides simple way to map all index data */
	hdr.h_files_begin = sizeof(struct webroot_hdr) +
		trie_trie_size(r->r_trie) +
		sizeof(struct webroot_redirect) * r->r_num_redirect +
		sizeof(struct webroot_file) * r->r_num_file +
		trie_strtab_size(r->r_trie) +
		r->r_mimetab_sz +
		r->r_redirtab_sz;

	return fobuf_write(out, &hdr, sizeof(hdr));
}

static int write_redirect_objs(struct webroot *r, fobuf_t out)
{
	struct webroot_redirect wr;
	struct object *obj;

	list_for_each_entry(obj, &r->r_redirect, o_list) {
		if ( obj->o_u.redirect.uri ) {
			wr.r_off = obj->o_u.redirect.strtab_off;
			wr.r_len = strlen(obj->o_u.redirect.uri);
		}else{
			wr.r_off = WEBROOT_INVALID_REDIRECT;
			wr.r_len = obj->o_u.redirect.code;
		}
		if ( !fobuf_write(out, &wr, sizeof(wr)) )
			return 0;
	}

	return 1;
}

static int write_file_objs(struct webroot *r, fobuf_t out)
{
	struct webroot_file wf;
	struct object *obj;

	list_for_each_entry(obj, &r->r_file, o_list) {
		wf.f_off = obj->o_u.file.off;
		wf.f_len = obj->o_u.file.size;
		wf.f_type = obj->o_u.file.type->m_strtab_off;
		wf.f_type_len = strlen(obj->o_u.file.type->m_type);
		if ( !fobuf_write(out, &wf, sizeof(wf)) )
			return 0;
	}

	return 1;
}

static int webroot_write(struct webroot *r, fobuf_t out)
{
	if ( !write_header(r, out) )
		return 0;
	if ( !trie_write_trie(r->r_trie, out) )
		return 0;
	if ( !write_redirect_objs(r, out) )
		return 0;
	if ( !write_file_objs(r, out) )
		return 0;
	if ( !trie_write_strtab(r->r_trie, out) )
		return 0;
	if ( !write_mimetab(r, out) )
		return 0;
	if ( !write_redirtab(r, out) )
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
	return sizeof(struct webroot_hdr) +
		trie_trie_size(r->r_trie) +
		sizeof(struct webroot_redirect) * r->r_num_redirect +
		sizeof(struct webroot_file) * r->r_num_file +
		trie_strtab_size(r->r_trie) +
		r->r_mimetab_sz +
		r->r_redirtab_sz +
		r->r_files_sz;
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

static struct object *obj_code(struct webroot *r, int code)
{
	struct object *obj;

	obj = hgang_alloc0(r->r_obj_mem);
	if ( NULL == obj )
		return NULL;

	obj->o_type = OBJ_TYPE_REDIRECT;
	obj->o_u.redirect.code = code;
	list_add_tail(&obj->o_list, &r->r_redirect);
	r->r_num_redirect++;

	return obj;
}

static struct object *obj_file(struct webroot *r,
				const char *path,
				dev_t dev,
				ino_t ino,
				uint64_t size)
{
	struct mime_type *m;
	struct object *obj;
	const char *mime;

	/* TODO: use a hash table, benchmarked at causing 200ms
	 * slowdown for 5,000 files. Dwarfed by I/O.
	 */
	list_for_each_entry(obj, &r->r_file, o_list) {
		if ( dev == obj->o_u.file.dev &&
			ino == obj->o_u.file.ino ) {
			printf("hit %s\n", path);
			return obj;
		}
	}

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
	obj->o_u.file.dev = dev;
	obj->o_u.file.ino = ino;

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

	/* TODO: sort entries by inode number for performance */
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

static int add_index(struct webroot *r, const char *u)
{
	unsigned int i;
	struct object *obj = NULL;

	dprintf("indexing %s\n", u);
	for(i = 0; i < ARRAY_SIZE(index_pages); i++) {
		struct stat st;
		char *uri, *ipath;

		uri = path_splice(u, index_pages[i]);
		if ( NULL == uri )
			return 0;

		ipath = webroot_lookup(r, uri);
		free(uri);
		if ( NULL == ipath )
			return 0;

		if ( stat(ipath, &st) ) {
			if ( errno == ENOENT ) {
				free(ipath);
				continue;
			}
			fprintf(stderr, "%s: %s: stat: %s\n",
				cmd, ipath, os_err());
			free(ipath);
			return 0;
		}

		if ( S_ISREG(st.st_mode) ) {
			dprintf("index: %s -> %s\n", u, ipath);
			obj = obj_file(r, ipath, st.st_dev,
					st.st_ino, st.st_size);
			free(ipath);
			if ( NULL == obj )
				return 0;
			break;
		}else{
			free(ipath);
			continue;
		}
	}

	if ( NULL == obj ) {
		obj = obj_code(r, 403 /* forbidden */);
	}
	if ( NULL == obj )
		return 0;

	if ( NULL == webroot_link(r, u, obj) )
		return 0;

	return 1;
}

static int scan_item(struct webroot *r, const char *u)
{
	struct object *obj = NULL;
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

		if ( !add_index(r, dpath) ) {
			free(dpath);
			goto out_free;
		}

		if ( strlen(u) ) {
			//dprintf("link '%s' -> '%s'\n", u, dpath);
			obj = obj_redirect(r, dpath);

			free(dpath);
			if ( NULL == obj )
				goto out_free;
		}

		if ( !scan_dir(r, u, path) )
			goto out_free;

	}else if ( S_ISREG(st.st_mode) ) {
		obj = obj_file(r, path, st.st_dev,
				st.st_ino, st.st_size);
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

	if ( obj && NULL == webroot_link(r, u, obj) )
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
	printf("%s: fallocate: %"PRId64" bytes\n", cmd, webroot_output_size(r));
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

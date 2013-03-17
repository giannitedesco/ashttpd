#include <compiler.h>

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <assert.h>
#include <stdarg.h>

#include <magic.h>

#include <list.h>
#include <hgang.h>
#include <strpool.h>
#include <fobuf.h>
#include <os.h>
#include <vec.h>
#include <webroot-format.h>
#include "trie.h"
#include "sha1.h"

#define WRITE_FILES	1
#define BUFFER_SIZE	(1U << 20U)
#define SYMBUF		(16U << 10U) /* readlink buffer */

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

static const char *tmpchunks_pattern = "/tmp/ashttpd.mkroot.XXXXXX";
static const char *cmd = "mkroot";
static int dotfiles; /* whether to include dot files */
static int indexdirs = 1;

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
			union {
				char *path;
				uint64_t tmpoff;
			}u;
			uint64_t size;
			uint64_t off;
			dev_t dev;
			ino_t ino;
			time_t mtime;
#define FILE_PATH	0
#define FILE_TMPCHUNK	1
			unsigned int content;
			uint8_t digest[WEBROOT_DIGEST_LEN];
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
	fobuf_t r_tmpchunks;
	const char *r_base;
	trie_t r_trie;
	struct trie_entry *r_trie_ent;
	magic_t r_magic;
	uint64_t r_files_sz;
	uint64_t r_tmpoff;
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

	ret = malloc(olen + 2);
	if ( NULL == ret ) {
		fprintf(stderr, "%s: calloc: %s\n", cmd, os_err());
		return NULL;
	}

	if ( (*dir && dir[strlen(dir) - 1] == '/') || path[0] == '/' )
		snprintf(ret, olen + 1, "%s%s", dir, path);
	else
		snprintf(ret, olen + 1, "%s/%s", dir, path);

	return ret;
}

static struct webroot *webroot_new(const char *base)
{
	struct webroot *r;
	char buf[strlen(tmpchunks_pattern) + 1];
	int fd;

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

	r->r_magic = magic_open(MAGIC_MIME_TYPE);
	if ( NULL == r->r_magic )
		goto out_free_str;

	if ( magic_load(r->r_magic, NULL) )
		goto out_free_magic;

	snprintf(buf, sizeof(buf), "%s", tmpchunks_pattern);
	fd = mkstemp(buf);
	if ( fd < 0 ) {
		fprintf(stderr, "%s: mkstemp: %s\n", cmd, os_err());
		goto out_free_magic;
	}
	printf("%s: Using %s for temp chunks\n", cmd, buf);
	if ( unlink(buf) ) {
		/* not fatal */
		fprintf(stderr, "%s: %s: unlink %s\n", cmd, buf, os_err());
	}

	r->r_tmpchunks = fobuf_new(fd, 0);
	if ( NULL == r->r_tmpchunks )
		goto out_close;

	INIT_LIST_HEAD(&r->r_uri);
	INIT_LIST_HEAD(&r->r_redirect);
	INIT_LIST_HEAD(&r->r_file);
	INIT_LIST_HEAD(&r->r_mime_type);
	r->r_base = base;
	goto out;

out_close:
	close(fd);
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
		/* fobuf likes to fsync stuff for safety
		 * but this is just a tmpfile so let's
		 * just nuke it quickly. Assumes previous
		 * code did the right things.
		 */
		close(fobuf_fd(r->r_tmpchunks));
		fobuf_abort(r->r_tmpchunks);

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

	printf("%s: index: trie=%"PRId64" bytes\n",
		cmd, trie_trie_size(t));
	r->r_trie = t;
	r->r_trie_ent = ent;

	/* Layout mime and redirect string tables */
	off = sizeof(struct webroot_hdr) +
		trie_trie_size(r->r_trie) +
		sizeof(struct webroot_redirect) * r->r_num_redirect +
		sizeof(struct webroot_file) * r->r_num_file;

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

/* the assumption here being that the webroot digest field is the same size
 * as a SHA1 hash.
*/
static void etag(uint8_t digest[WEBROOT_DIGEST_LEN], uint8_t sha[20])
{
	memcpy(digest, sha, 20);
}

static int write_file(struct webroot *r, struct object *f, fobuf_t out)
{
	uint8_t buf[BUFFER_SIZE];
	blk_SHA_CTX ctx;
	uint8_t sha[20];
	ssize_t ret;
	uint64_t len;
	int rc = 0;
	int fd;

	if ( f->o_u.file.content == FILE_PATH ) {
		fd = open(f->o_u.file.u.path, O_RDONLY);
		if ( fd < 0 ) {
			fprintf(stderr, "%s: %s: open: %s\n",
				cmd, f->o_u.file.u.path, os_err());
			goto out;
		}
	}else if ( f->o_u.file.content == FILE_TMPCHUNK ) {
		fd = fobuf_fd(r->r_tmpchunks);
		if ( lseek(fd, f->o_u.file.u.tmpoff, SEEK_SET) < 0 ) {
			fprintf(stderr, "%s: lseek: %s\n", cmd, os_err());
			return 0;
		}
	}else{
		abort();
	}

	blk_SHA1_Init(&ctx);
	len = f->o_u.file.size;
again:
	ret = read(fd, buf, (len > sizeof(buf)) ? sizeof(buf) : len);
	if ( ret < 0 ) {
		fprintf(stderr, "%s: %s: read: %s\n",
			cmd, f->o_u.file.u.path, os_err());
		goto out_close;
	}

	if ( ret && !fobuf_write(out, buf, ret) ) {
		fprintf(stderr, "%s: %s: write: %s\n",
			cmd, f->o_u.file.u.path, os_err());
		goto out_close;
	}

	blk_SHA1_Update(&ctx, buf, ret);
	len -= (size_t)ret;

	if ( ret )
		goto again;

	blk_SHA1_Final(sha, &ctx);
	etag(f->o_u.file.digest, sha);
	if ( !len ) {
		rc = 1;
	}else{
		/* should never happen to tmpchunks */
		assert(f->o_u.file.content == FILE_PATH);
		fprintf(stderr, "%s: %s size was modified during scan\n",
			cmd, f->o_u.file.u.path);
	}

out_close:
	if ( f->o_u.file.content == FILE_PATH ) {
		close(fd);
	}
out:
	return rc;
}

static int write_files(struct webroot *r, fobuf_t out)
{
	struct object *obj;

	/* make sure the tmpchunks data is actually flushed
	 * from buffers and in to the file, since the fobuf
	 * cache has no way of doing coherancy because we're
	 * reading with read(2) behind its back
	 */
	if ( !fobuf_flush(r->r_tmpchunks) )
		return 0;

	list_for_each_entry(obj, &r->r_file, o_list) {
		if ( !write_file(r, obj, out) )
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
	hdr.h_strtab_sz = r->r_mimetab_sz +
				r->r_redirtab_sz;
	hdr.h_magic = WEBROOT_MAGIC;
	hdr.h_vers = WEBROOT_CURRENT_VER;

	/* provides simple way to map all index data */
	hdr.h_files_begin = sizeof(struct webroot_hdr) +
		trie_trie_size(r->r_trie) +
		sizeof(struct webroot_redirect) * r->r_num_redirect +
		sizeof(struct webroot_file) * r->r_num_file +
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

static uint32_t modified(time_t mtime)
{
	return mtime;
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
		wf.f_modified = modified(obj->o_u.file.mtime);
		memcpy(wf.f_digest, obj->o_u.file.digest, WEBROOT_DIGEST_LEN);
		if ( !fobuf_write(out, &wf, sizeof(wf)) )
			return 0;
	}

	return 1;
}

static int write_etags(struct webroot *r, fobuf_t out)
{
	int fd;
	off_t off;

	/* flush internal buffers, no fsync */
	if ( !fobuf_flush(out) )
		return 0;

	/* seek back to start of file objects */
	fd = fobuf_fd(out);
	off = sizeof(struct webroot_hdr) +
		trie_trie_size(r->r_trie) +
		sizeof(struct webroot_redirect) * r->r_num_redirect;
	if ( lseek(fd, off, SEEK_SET) < 0 ) {
		fprintf(stderr, "%s: lseek: %s\n", cmd, os_err());
		return 0;
	}

	return write_file_objs(r, out);
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
	if ( !write_mimetab(r, out) )
		return 0;
	if ( !write_redirtab(r, out) )
		return 0;

#if WRITE_FILES
	printf("%s: Writing files\n", cmd);
	if ( !write_files(r, out) )
		return 0;
#endif

	if ( !write_etags(r, out) )
		return 0;

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
				dev_t dev,
				ino_t ino,
				time_t mtime,
				uint64_t size)
{
	struct object *obj;

	/* TODO: use a hash table, benchmarked at causing 200ms
	 * slowdown for 5,000 files. Dwarfed by I/O and SHA1.
	 */
	list_for_each_entry(obj, &r->r_file, o_list) {
		if ( dev == obj->o_u.file.dev &&
			ino == obj->o_u.file.ino ) {
			return obj;
		}
	}

	obj = hgang_alloc0(r->r_obj_mem);
	if ( NULL == obj )
		return NULL;

	obj->o_type = OBJ_TYPE_FILE;

	obj->o_u.file.size = size;
	obj->o_u.file.dev = dev;
	obj->o_u.file.ino = ino;
	obj->o_u.file.mtime = mtime;
	obj->o_u.file.content = FILE_PATH;

	list_add_tail(&obj->o_list, &r->r_file);
	r->r_num_file++;

	return obj;
}

static struct object *obj_file_path(struct webroot *r,
					const char *path,
					dev_t dev,
					ino_t ino,
					time_t mtime,
					uint64_t size)
{
	struct object *obj;
	struct mime_type *m;
	const char *mime;

	obj = obj_file(r, dev, ino, mtime, size);
	if ( NULL == obj )
		return NULL;

	obj->o_u.file.content = FILE_PATH;

	obj->o_u.file.u.path = webroot_strdup(r, path);
	if ( NULL == obj->o_u.file.u.path ) {
		list_del(&obj->o_list);
		hgang_return(r->r_obj_mem, obj);
		return NULL;
	}

	mime = magic_file(r->r_magic, path);
	m = mime_add(r, mime);
	if ( NULL == m ) {
		list_del(&obj->o_list);
		hgang_return(r->r_obj_mem, obj);
		return NULL;
	}
	obj->o_u.file.type = m;

	return obj;
}

static struct object *obj_file_tmpchunks(struct webroot *r,
					const char *mime,
					dev_t dev,
					ino_t ino,
					time_t mtime)
{
	struct object *obj;
	struct mime_type *m;

	obj = obj_file(r, dev, ino, mtime, 0);
	if ( NULL == obj )
		return NULL;

	obj->o_u.file.content = FILE_TMPCHUNK;
	obj->o_u.file.u.tmpoff = r->r_tmpoff;

	m = mime_add(r, mime);
	if ( NULL == m ) {
		list_del(&obj->o_list);
		hgang_return(r->r_obj_mem, obj);
		return NULL;
	}
	obj->o_u.file.type = m;

	return obj;
}
static int scan_item(struct webroot *r, const char *dir, const char *u);

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
		ret = scan_item(r, dir, uri);
		free(uri);
		if ( !ret )
			goto out;
	}

	closedir(d);
	ret = 1;
out:
	return ret;
}

static int try_index(struct webroot *r, const char *u,
			const char *page, struct object **obj)
{
	struct stat st;
	char *uri, *ipath;

	uri = path_splice(u, page);
	if ( NULL == uri )
		return 0;

	ipath = webroot_lookup(r, uri);
	free(uri);
	if ( NULL == ipath )
		return 0;

	if ( stat(ipath, &st) ) {
		if ( errno == ENOENT ) {
			free(ipath);
			*obj = NULL;
			return 1;
		}
		fprintf(stderr, "%s: %s: stat: %s\n",
			cmd, ipath, os_err());
		free(ipath);
		return 0;
	}

	if ( S_ISREG(st.st_mode) ) {
		dprintf("index: %s -> %s\n", u, ipath);
		*obj = obj_file_path(r, ipath, st.st_dev, st.st_ino,
				st.st_mtime, st.st_size);
		free(ipath);
		if ( NULL == *obj )
			return 0;

		/* found it, OK */
		return 1;
	}

	/* didn't find it but OK */
	free(ipath);
	*obj = NULL;
	return 1;
}

_printf(2, 3) static void tc_printf(struct webroot *r, const char *fmt, ...)
{
	static char *abuf;
	static size_t abuflen;
	int len;
	va_list va;
	char *new;

	if ( NULL == r->r_tmpchunks ) {
		/* callers MUST check that this didn't happen */
		return;
	}

again:
	va_start(va, fmt);

	len = vsnprintf(abuf, abuflen, fmt, va);
	if ( len < 0 ) /* bug in old glibc */
		len = 0;
	if ( (size_t)len < abuflen )
		goto done;

	new = realloc(abuf, len + 1);
	if ( new == NULL )
		goto done;

	abuf = new;
	abuflen = len + 1;
	goto again;

done:
	/* if writes fail, we just get rid of all tmpchunks,
	 * caller must check and abort
	*/
	if ( !fobuf_write(r->r_tmpchunks, abuf, (size_t)len) ) {
		int fd;
		fprintf(stderr, "%s: fobuf_write: %s\n", cmd, os_err());
		fd = fobuf_fd(r->r_tmpchunks);
		fobuf_abort(r->r_tmpchunks);
		close(fd);
		r->r_tmpchunks = NULL;
	}else{
		r->r_tmpoff += (size_t)len;
	}
	va_end(va);
}

static struct object *index_dir(struct webroot *r, struct stat *st,
				const char *path, const char *u)
{
	struct object *obj;
	struct dirent *e;
	DIR *d;

	printf("%s: indexing %s\n", cmd, path);

	obj = obj_file_tmpchunks(r, "text/html", st->st_dev, st->st_ino,
				 st->st_mtime);
	if ( NULL == obj )
		return NULL;

	tc_printf(r, "<!DOCTYPE HTML PUBLIC "
				"\"-//W3C//DTD HTML 3.2 Final//EN\">");
	tc_printf(r, "<html>\n\t<head><title>Index of %s</title>\t</head>\n",
			u);
	tc_printf(r, "<body><h1>Index of %s</h1>\n", u);
	tc_printf(r, "</body>\n</html>");

	d = opendir(path);
	if ( NULL == d ) {
		fprintf(stderr, "%s: %s: opendir: %s\n", cmd, path, os_err());
		return NULL;
	}

	while( (e = readdir(d)) ) {
		char *uri;
		if ( !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") )
			continue;
		if ( !dotfiles && e->d_name[0] == '.' )
			continue;

		uri = path_splice(u, e->d_name);
		tc_printf(r, "<a href=\"%s\">%s</a><br>\n", uri, e->d_name);
		free(uri);
	}

	closedir(d);

	if ( NULL == r->r_tmpchunks )
		return NULL;
	obj->o_u.file.size = r->r_tmpoff - obj->o_u.file.u.tmpoff;
	return obj;
}

static int add_index(struct webroot *r, struct stat *st,
			const char *path, const char *u)
{
	unsigned int i;
	struct object *obj = NULL;

	dprintf("indexing %s\n", u);
	for(i = 0; NULL == obj && i < ARRAY_SIZE(index_pages); i++) {
		if ( !try_index(r, u, index_pages[i], &obj) )
			return 0;
	}

	if ( NULL == obj ) {
		if ( indexdirs ) {
			obj = index_dir(r, st, path, u);
		}else{
			obj = obj_code(r, 403 /* forbidden */);
		}
	}
	if ( NULL == obj )
		return 0;

	if ( NULL == webroot_link(r, u, obj) )
		return 0;

	return 1;
}

static int scan_item(struct webroot *r, const char *dir, const char *u)
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

		if ( !add_index(r, &st, path, dpath) ) {
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
		obj = obj_file_path(r, path, st.st_dev, st.st_ino,
				st.st_mtime, st.st_size);
		if ( NULL == obj )
			goto out_free;
	}else if ( S_ISLNK(st.st_mode) ) {
		char *lpath;
		char buf[SYMBUF];
		ssize_t ret;

		ret = readlink(path, buf, sizeof(buf));
		if ( ret < 0 ) {
			fprintf(stderr, "%s: %s: readlink: %s\n",
				cmd, path, os_err());
			goto out;
		}

		lpath = path_splice(dir, buf);
		if ( NULL == lpath )
			goto out_free;

		dprintf("symlink %s -> %s\n", u, lpath);
		obj = obj_redirect(r, lpath);
		free(lpath);
		if ( NULL == obj )
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
	if ( !scan_item(r, "", "") )
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

	printf("%s: syncing %s\n", cmd, outfn);
	if ( !fobuf_close(out) )
		goto out_close;

	ret = 1;

out_close:
	close(fd);
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

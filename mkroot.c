#include <compiler.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <assert.h>

#include <list.h>
#include <hgang.h>
#include <strpool.h>

static const char *cmd = "mkroot";
static int dotfiles; /* whether to include dot files */

#define OBJ_TYPE_FILE		0
#define OBJ_TYPE_REDIRECT	1
struct object {
	struct list_head o_list;
	unsigned int o_type;
	union {
		struct {
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
	struct list_head r_obj;
	hgang_t r_uri_mem;
	hgang_t r_obj_mem;
	strpool_t r_str_mem;
	const char *r_base;
};

static const char *os_err(void)
{
	return strerror(errno);
}

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

	r->r_str_mem = strpool_new(0);
	if ( NULL == r->r_str_mem )
		goto out_free_obj;

	INIT_LIST_HEAD(&r->r_uri);
	INIT_LIST_HEAD(&r->r_obj);
	r->r_base = base;
	goto out;

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
		strpool_free(r->r_str_mem);
		free(r);
	}
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
	return uri;
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

	list_add_tail(&obj->o_list, &r->r_obj);

	return obj;
}

static struct object *obj_file(struct webroot *r,
				const char *path,
				uint64_t size)
{
	struct object *obj;

	obj = hgang_alloc0(r->r_obj_mem);
	if ( NULL == obj )
		return NULL;

	obj->o_type = OBJ_TYPE_FILE;

	obj->o_u.file.path = webroot_strdup(r, path);
	if ( NULL == obj->o_u.file.path ) {
		hgang_return(r->r_obj_mem, obj);
		return NULL;
	}

	obj->o_u.file.size = size;

	list_add_tail(&obj->o_list, &r->r_obj);

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
	int ret = 0;

	r = webroot_new(dir);
	if ( NULL == r ) {
		fprintf(stderr, "%s: webroot_new: %s\n", cmd, os_err());
		goto out;
	}

	if ( scan_item(r, "") )
		ret = 1;

//out_free:
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

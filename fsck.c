#include <ashttpd.h>
#include <webroot-format.h>
#include <os.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#if 1
#define dprintf printf
#else
#define dprintf(x...) do {} while(0)
#endif

#include "webroot-common.h"

static const char *cmd = "fsckroot";

static size_t do_dump(FILE *f, struct _webroot *r,
			const struct trie_dedge *re,
			unsigned int num_edges)
{
	unsigned int i;
	size_t ret = 0;

	for(i = 0; i < num_edges; i++) {
		unsigned int j;
		uint32_t edges_idx, strtab_ofs;
		const uint8_t *ptr;
		uint8_t res[2];
		size_t tmp;

		edges_idx = trie_edges_index(re + i);
		strtab_ofs = trie_strtab_ofs(re + i);
		if ( string_is_resident(re + i) ) {
			res[0] = strtab_ofs & 0xff;
			res[1] = (strtab_ofs >> 8) & 0xff;
			res[2] = (strtab_ofs >> 16) & 0xff;
			ptr = res;
		}else{
			ptr = r->r_strtab + strtab_ofs;
		}

		fprintf(f, "\t\"n_%p\" [shape=rectangle label=\"%.*s\"];\n",
			re + i, (int)re[i].re_strtab_len, ptr);
		tmp = do_dump(f, r, r->r_trie + edges_idx,
			re[i].re_num_edges) + re[i].re_strtab_len;
		if ( tmp > ret )
			ret = tmp;
		for(j = 0; j < re[i].re_num_edges; j++) {
			fprintf(f, "\t\"n_%p\" -> \"n_%p\"\n",
				re + i, r->r_trie + edges_idx + j);
		}
	}

	return ret;
}

static size_t dump_root(struct _webroot *r, const char *outfn)
{
	size_t max_len;
	FILE *f;

	printf("%s: dumping to %s\n", cmd, outfn);

	f = fopen(outfn, "w");
	fprintf(f, "strict digraph \"Radix trie\" {\n");
	fprintf(f, "\tgraph[rankdir=LR];\n");
	fprintf(f, "\tnode[shape=ellipse, style=filled, "
			"fillcolor=transparent];\n");
	//fprintf(f, "\tedge[fontsize=6];\n");
	max_len = do_dump(f, r, r->r_trie, 1);
	fprintf(f, "}\n");
	fclose(f);
	return max_len;
}

static void print_obj(struct _webroot *r, const struct trie_dedge *re,
			const char *uri, size_t uri_len)
{
	printf("%.*s\n", (int)uri_len, uri);
	printf(" - OID: %u (%s)\n",
		re->re_oid,
		(re->re_oid < r->r_num_redirect) ? "redir" : "file");
	if ( re->re_oid < r->r_num_redirect ) {
		const struct webroot_redirect *redir;

		redir = r->r_redir + re->re_oid;
		printf(" - redirect to %.*s\n",
			(int)redir->r_len,
			(char *)r->r_map + redir->r_off);
	}else{
		const struct webroot_file *file;

		file = r->r_file + (re->re_oid - r->r_num_redirect);

		printf(" - mime type: %.*s\n",
			(int)file->f_type_len,
			r->r_map + file->f_type);
		printf(" - off/len = 0x%llx 0x%llx\n",
			file->f_off, file->f_len);
	}
}

static void do_deets(struct _webroot *r, char *uri,
			const struct trie_dedge *re,
			unsigned int num_edges,
			char *buf)
{
	unsigned int i;

	for(i = 0; i < num_edges; i++) {
		uint32_t edges_idx, strtab_ofs;
		const uint8_t *ptr;
		uint8_t res[2];

		edges_idx = trie_edges_index(re + i);
		strtab_ofs = trie_strtab_ofs(re + i);
		if ( string_is_resident(re + i) ) {
			res[0] = strtab_ofs & 0xff;
			res[1] = (strtab_ofs >> 8) & 0xff;
			res[2] = (strtab_ofs >> 16) & 0xff;
			ptr = res;
		}else{
			ptr = r->r_strtab + strtab_ofs;
		}

		memcpy(buf, ptr, re[i].re_strtab_len);
		if ( re[i].re_oid != GIDX_INVALID_OID ) {
			print_obj(r, &re[i], uri,
				((buf + re[i].re_strtab_len) - uri));
		}
		do_deets(r, uri, r->r_trie + edges_idx,
			re[i].re_num_edges,
			buf + re[i].re_strtab_len);
	}
}

static void print_deets(struct _webroot *r, char *buf)
{
	do_deets(r, buf, r->r_trie, 1, buf);
}

static int do_fsck(const char *fn)
{
	struct _webroot *r;
	size_t max_len;
	char *buf;

	r = webroot_open(fn);
	if ( NULL == r )
		return 0;
	printf("%s: opened %s\n", cmd, fn);

	max_len = dump_root(r, "fsck.dot");
	printf("%s: max uri length is %zu\n", cmd,  max_len);

	buf = malloc(max_len);
	print_deets(r, buf);
	free(buf);

	webroot_close(r);
	return 1;
}

int main(int argc, char **argv)
{
	if ( argc > 0 )
		cmd = argv[0];

	if ( argc < 2 ) {
		fprintf(stderr, "%s: Usage\n", cmd);
		fprintf(stderr, "\t%s [filename]\n", cmd);
		return EXIT_FAILURE;
	}

	if ( !do_fsck(argv[1]) )
		return EXIT_FAILURE;

	printf("%s: OK\n", cmd);
	return EXIT_SUCCESS;
}

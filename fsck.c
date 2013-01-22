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

static void calc_hist(struct _webroot *r,
			const struct trie_dedge *re,
			unsigned int num_edges,
			unsigned int *hist)
{
	unsigned int i;

	for(i = 0; i < num_edges; i++) {
		uint32_t edges_idx;

		edges_idx = re[i].re_edges_idx;
		calc_hist(r, r->r_trie + edges_idx,
			re[i].re_num_edges, hist);
		hist[re[i].re_strlen]++;
	}
}

static size_t do_dump(FILE *f, struct _webroot *r,
			const struct trie_dedge *re,
			unsigned int num_edges)
{
	unsigned int i;
	size_t ret = 0;

	for(i = 0; i < num_edges; i++) {
		unsigned int j;
		uint32_t edges_idx;
		size_t tmp;

		edges_idx = re[i].re_edges_idx;

		fprintf(f, "\t\"n_%p\" [shape=rectangle label=\"%.*s\"];\n",
			re + i, (int)re[i].re_strlen, re[i].re_str);
		tmp = do_dump(f, r, r->r_trie + edges_idx,
			re[i].re_num_edges) + re[i].re_strlen;
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
		if ( redir->r_off == WEBROOT_INVALID_REDIRECT ) {
			printf(" - redirect to code %d\n",
				(int)redir->r_len);
		}else{
			printf(" - redirect to %.*s\n",
				(int)redir->r_len,
				(char *)r->r_map + redir->r_off);
		}
	}else{
		const struct webroot_file *file;

		file = r->r_file + (re->re_oid - r->r_num_redirect);

		printf(" - mime type: %.*s\n",
			(int)file->f_type_len,
			(char *)r->r_map + file->f_type);
		printf(" - off/len = 0x%"PRIx64" 0x%"PRIx64"\n",
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
		uint32_t edges_idx;

		edges_idx = re[i].re_edges_idx;

		memcpy(buf, re[i].re_str, re[i].re_strlen);
		if ( re[i].re_oid != GIDX_INVALID_OID ) {
			print_obj(r, &re[i], uri,
				((buf + re[i].re_strlen) - uri));
		}
		do_deets(r, uri, r->r_trie + edges_idx,
			re[i].re_num_edges,
			buf + re[i].re_strlen);
	}
}

static void print_deets(struct _webroot *r, char *buf)
{
	do_deets(r, buf, r->r_trie, 1, buf);
}

static void dump_hist(unsigned int *hist, size_t n)
{
	unsigned int total, sofar;
	size_t i;

	for(total = i = 0; i < n; i++) {
		total += hist[i];
	}

	printf("Trie edge histogram (val, cnt):\n");
	printf("Total nodes %u\n", total);
	for(sofar = i = 0; i < n; i++) {
		if ( !hist[i] )
			continue;

		sofar += hist[i];
		printf("  %zu: %u (%.3f%%)\n",
			i, hist[i],
			((float)sofar/(float)total) * 100.0);
	}
}

static int do_fsck(const char *fn)
{
	unsigned int *hist;
	struct _webroot *r;
	size_t max_len;
	char *buf;

	r = webroot_open(fn);
	if ( NULL == r )
		return 0;
	printf("%s: opened %s\n", cmd, fn);

	max_len = dump_root(r, "fsck.dot");
	hist = calloc(max_len, sizeof(*hist));
	calc_hist(r, r->r_trie, 1, hist);
	printf("%s: max uri length is %zu\n", cmd,  max_len);
	printf("%s: index map size %zu\n", cmd, r->r_map_sz);

	buf = malloc(max_len);
	print_deets(r, buf);
	free(buf);

	dump_hist(hist, max_len);
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

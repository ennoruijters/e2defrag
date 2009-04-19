#include <stdio.h>
#include "e2defrag.h"
#include "extree.h"

void dump_trees(struct defrag_ctx *c)
{
	struct rb_node *n = rb_first(&c->free_tree_by_size);
	while (n) {
		struct free_extent *f = rb_entry(n, struct free_extent,size_rb);
		printf("F: %llu-%llu\n", f->start_block, f->end_block);
		n = rb_next(n);
	}
	n = rb_first(&c->extents_by_block);
	while (n) {
		struct data_extent *f = rb_entry(n,struct data_extent,block_rb);
		printf("U: %llu-%llu(%llu) of %u\n", f->start_block,
		       f->end_block, f->start_logical, f->inode_nr);
		n = rb_next(n);
	}
}

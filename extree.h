/*
 * Extent tree management functions
 */
#ifndef EXTREE_H
#define EXTREE_H
#include <assert.h>
#include "rbtree.h"

static inline void insert_data_extent(struct defrag_ctx *c,
                                      struct data_extent *e)
{
	struct rb_node **p = &c->extent_tree.rb_node;
	struct rb_node *parent = NULL;

	while (*p) {
		struct data_extent *extent;

		parent = *p;
		extent = rb_entry(parent, struct data_extent, node);

#ifndef NDEBUG
		if (e->start_block == extent->start_block) {
			printf("Assertion failed: e->start_block != "
			       "extent->start_block at %s:%d\n",
			       __FILE__,__LINE__);
			printf("Block: %llu\n", e->start_block);
			printf("Inodes: %u (in tree) and %u\n",
			       extent->inode_nr, e->inode_nr);
			printf("Logicals: %llu (in tree) and %llu\n",
			       extent->start_logical, e->start_logical);
			printf("End blocks: %llu and %llu\n",
			       extent->end_block, e->end_block);
			if (e == extent)
				printf("Duplicate insertion\n");
			exit(1);
		}
#endif
		if (e->start_block < extent->start_block)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&e->node, parent, p);
	rb_insert_color(&e->node, &c->extent_tree);
}

static inline void insert_free_extent(struct defrag_ctx *c,
                                      struct free_extent *e)
{
	struct rb_node **p = &c->free_tree.rb_node;
	struct rb_node *parent = NULL;
	blk64_t num_blocks = e->end_block - e->start_block;

	while (*p) {
		struct free_extent *extent;

		parent = *p;
		extent = rb_entry(parent, struct free_extent, node);
		blk64_t p_num_blocks = extent->end_block - extent->start_block;

		if (num_blocks < p_num_blocks)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&e->node, parent, p);
	rb_insert_color(&e->node, &c->free_tree);
}

static inline struct data_extent *containing_data_extent(struct defrag_ctx *c,
                                                         blk64_t block)
{
	struct rb_node *ret = c->extent_tree.rb_node;
	while (ret) {
		struct data_extent *e;
		e = rb_entry(ret, struct data_extent, node);
		if (block > e->start_block && block < e->end_block)
			return e;
		if (block > e->start_block)
			ret = ret->rb_right;
		else
			ret = ret->rb_left;
	}
	return NULL;
}

#endif

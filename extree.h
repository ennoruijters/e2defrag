/*
 * Extent tree management functions
 */
#ifndef EXTREE_H
#define EXTREE_H
#include <assert.h>
#include "rbtree.h"

#ifndef offsetof
#ifdef __GNUC__
#define offsetof __builtin_offsetof
#else
#define offsetof(type, field) (&((type *)0)->field)
#endif
#endif

static inline void rb_remove_data_extent(struct defrag_ctx *c,
                                         struct data_extent *e)
{
	rb_erase(&e->block_rb, &c->extents_by_block);
	rb_erase(&e->size_rb, &c->extents_by_size);
}

static inline void rb_remove_free_extent(struct defrag_ctx *c,
                                         struct free_extent *e)
{
	rb_erase(&e->block_rb, &c->free_tree_by_block);
	rb_erase(&e->size_rb, &c->free_tree_by_size);
}

static inline void insert_data_extent_by_block(struct defrag_ctx *c,
                                               struct data_extent *e)
{
	struct rb_node **p = &c->extents_by_block.rb_node;
	struct rb_node *parent = NULL;

	while (*p) {
		struct data_extent *extent;

		parent = *p;
		extent = rb_entry(parent, struct data_extent, block_rb);

		assert(e->start_block != extent->start_block);
		if (e->start_block < extent->start_block)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&e->block_rb, parent, p);
	rb_insert_color(&e->block_rb, &c->extents_by_block);
}

static inline void insert_data_extent(struct defrag_ctx *c,
                                      struct data_extent *e)
{
	struct rb_node **p = &c->extents_by_size.rb_node;
	struct rb_node *parent = NULL;
	insert_data_extent_by_block(c, e);
	while (*p) {
		struct data_extent *extent;

		parent = *p;
		extent = rb_entry(parent, struct data_extent, size_rb);

		if (e->end_block - e->start_block
		                      < extent->end_block - extent->start_block)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&e->size_rb, parent, p);
	rb_insert_color(&e->size_rb, &c->extents_by_size);
}

static inline void insert_free_extent(struct defrag_ctx *c,
                                      struct free_extent *e)
{
	struct rb_node **p = &c->free_tree_by_size.rb_node;
	struct rb_node *parent = NULL;
	blk64_t num_blocks = e->end_block - e->start_block;

	while (*p) {
		struct free_extent *extent;

		parent = *p;
		extent = rb_entry(parent, struct free_extent, size_rb);
		blk64_t p_num_blocks = extent->end_block - extent->start_block;

		if (num_blocks < p_num_blocks)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&e->size_rb, parent, p);
	rb_insert_color(&e->size_rb, &c->free_tree_by_size);

	p = &c->free_tree_by_block.rb_node;
	parent = NULL;

	while (*p) {
		struct free_extent *extent;

		parent = *p;
		extent = rb_entry(parent, struct free_extent, size_rb);

		if (extent->start_block < e->start_block)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&e->block_rb, parent, p);
	rb_insert_color(&e->block_rb, &c->free_tree_by_block);
}

static inline struct data_extent *containing_data_extent(struct defrag_ctx *c,
                                                         blk64_t block)
{
	struct rb_node *ret = c->extents_by_block.rb_node;
	while (ret) {
		struct data_extent *e;
		e = rb_entry(ret, struct data_extent, block_rb);
		if (block >= e->start_block && block <= e->end_block)
			return e;
		if (block > e->start_block)
			ret = ret->rb_right;
		else
			ret = ret->rb_left;
	}
	return NULL;
}

static inline struct free_extent *containing_free_extent(struct defrag_ctx *c,
                                                         blk64_t block)
{
	struct rb_node *ret = c->free_tree_by_block.rb_node;
	while (ret) {
		struct free_extent *e;
		e = rb_entry(ret, struct free_extent, block_rb);
		if (block >= e->start_block && block <= e->end_block)
			return e;
		if (block < e->start_block)
			ret = ret->rb_right;
		else
			ret = ret->rb_left;
	}
	return NULL;
}

#endif

/*
    Copyright 2009 Enno Ruijters

    This program is free software; you can redistribute it and/or
    modify it under the terms of version 2 of the GNU General
    Public License as published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

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

static inline void rb_remove_data_alloc(struct defrag_ctx *c,
                                        struct allocation *alloc)
{
	int i;
	for (i = 0; i < alloc->extent_count; i++)
		rb_remove_data_extent(c, &alloc->extents[i]);
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

static inline void insert_data_alloc(struct defrag_ctx *c,
                                     struct allocation *alloc)
{
	int i;
	for (i = 0; i < alloc->extent_count; i++)
		insert_data_extent(c, &alloc->extents[i]);
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
		extent = rb_entry(parent, struct free_extent, block_rb);

		if (extent->start_block > e->start_block)
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
		if (block > e->start_block)
			ret = ret->rb_right;
		else
			ret = ret->rb_left;
	}
	return NULL;
}

static inline struct free_extent *free_extent_after(struct defrag_ctx *c,
                                                    blk64_t block)
{
	struct free_extent *ret = NULL;
	struct rb_node *current = c->free_tree_by_block.rb_node;
	while (current) {
		struct free_extent *e;
		e = rb_entry(current, struct free_extent, block_rb);
		if (block > e->start_block) {
			current = current->rb_right;
		} else {
			if (ret == NULL || ret->start_block > e->start_block)
				ret = e;
			current = current->rb_left;
		}
	}
	return ret;
}

#endif

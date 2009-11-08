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

#include <malloc.h>
#include <errno.h>
#include <obstack.h>
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include "e2defrag.h"
#include "extree.h"

static int allocate_space(struct defrag_ctx *c,
                          blk64_t start, e2_blkcnt_t numblocks)
{
	struct free_extent *extent;
	extent = containing_free_extent(c, start);
	if (!extent || extent->end_block < start + numblocks - 1) {
		errno = ENOSPC;
		return -1;
	}
	if (start == extent->start_block) {
		rb_remove_free_extent(c, extent);
		extent->start_block += numblocks;
		if (extent->end_block < extent->start_block)
			free(extent);
		else
			insert_free_extent(c, extent);
	} else if (extent->end_block == start + numblocks - 1) {
		rb_remove_free_extent(c, extent);
		extent->end_block = start - 1;
		insert_free_extent(c, extent);
	} else {
		struct free_extent *new_extent;
		new_extent = malloc(sizeof *new_extent);
		if (new_extent == NULL)
			return -1;
		new_extent->start_block = extent->start_block;
		new_extent->end_block = start - 1;
		rb_remove_free_extent(c, extent);
		extent->start_block = start + numblocks;
		/* end_block unchanged */
		insert_free_extent(c, extent);
		insert_free_extent(c, new_extent);
	}
	mark_blocks_used(c, start, numblocks);
	return 0;
}

/* extent should not be in the free extent list.
 * extent may be freed if merged.
 * The returned structure will not be in the free extent list.
 */
static struct free_extent *try_merge(struct defrag_ctx *c,
                                     struct free_extent *extent)
{
	struct free_extent *other_extent;

	other_extent = containing_free_extent(c, extent->start_block - 1);
	if (other_extent) {
		rb_remove_free_extent(c, other_extent);
		other_extent->end_block = extent->end_block;
		free(extent);
		extent = other_extent;
	}
	other_extent = containing_free_extent(c, extent->end_block + 1);
	if (other_extent) {
		rb_remove_free_extent(c, other_extent);
		other_extent->start_block = extent->start_block;
		free(extent);
		extent = other_extent;
	}
	return extent;
}

int deallocate_space(struct defrag_ctx *c, blk64_t start, e2_blkcnt_t numblocks)
{
	struct free_extent *extent;

	if ((extent = containing_free_extent(c, start - 1)) != NULL) {
		rb_remove_free_extent(c, extent);
		extent->end_block = start + numblocks - 1;
		extent = try_merge(c, extent);
		if (extent == NULL)
			return -1;
	} else if ((extent = containing_free_extent(c, start + numblocks))) {
		rb_remove_free_extent(c, extent);
		extent->start_block = start;
		extent = try_merge(c, extent);
		if (extent == NULL)
			return -1;
	} else {
		extent = malloc(sizeof *extent);
		if (extent == NULL)
			return -1;
		extent->start_block = start;
		extent->end_block = start + numblocks - 1;
	}
	insert_free_extent(c, extent);
	mark_blocks_unused(c, start, numblocks);
	return 0;
}

/* Note: also frees the allocation, even on errors */
int deallocate_blocks(struct defrag_ctx *c, struct allocation *space)
{
	int i, ret = 0;
	for (i = 0; i < space->extent_count; i++) {
		e2_blkcnt_t num_blocks = 1 + space->extents[i].end_block
		                         - space->extents[i].start_block;
		if (ret >= 0)
			ret = deallocate_space(c, space->extents[i].start_block,
		                               num_blocks);
		else
			deallocate_space(c, space->extents[i].start_block,
		                         num_blocks);
	}
	free(space);
	return ret < 0 ? ret : 0;
}

static void optimize_allocation(struct rb_node **nodes,
                                e2_blkcnt_t *num_allocated,
                                e2_blkcnt_t num_blocks, int num_extents)
{
	int i;

	for (i = num_extents - 1; i >= 0; i--) {
		struct rb_node *next_node;
		struct free_extent *cur_extent, *next_extent;
		e2_blkcnt_t cur_count, next_count;

		next_node = rb_prev(nodes[i]);
		if (!next_node
		    || (i != num_extents - 1 && next_node == nodes[i+1]))
			continue;
		next_extent = rb_entry(next_node, struct free_extent, size_rb);
		cur_extent = rb_entry(nodes[i], struct free_extent, size_rb);
		next_count = next_extent->end_block - next_extent->start_block;
		next_count += 1;
		cur_count = cur_extent->end_block - cur_extent->start_block + 1;
		while (*num_allocated - cur_count + next_count > num_blocks) {
			*num_allocated -= cur_count;
			*num_allocated += next_count;
			nodes[i] = next_node;
			cur_count = next_count;
			cur_extent = next_extent;
			next_node = rb_prev(next_node);
			if (!next_node
			    || (i < num_extents - 1 && next_node == nodes[i+1]))
				break;
			next_extent = rb_entry(next_node, struct free_extent,
			                       size_rb);
			next_count = next_extent->end_block + 1
			             - next_extent->start_block;
		}
	}
	return;
}

static struct rb_node **simple_allocator(struct defrag_ctx *c,
                                         e2_blkcnt_t num_blocks,
                                         e2_blkcnt_t *allocated_blocks,
                                         int *extents)
{
	struct obstack stack;
	struct rb_node *n = rb_last(&c->free_tree_by_size);
	struct rb_node **ret;
	size_t size;

	obstack_init(&stack);
	*allocated_blocks = 0;
	*extents = 0;

	while (num_blocks) {
		struct free_extent *biggest;
		e2_blkcnt_t biggest_num_blocks;
		if (!n) {
			errno = ENOSPC;
			obstack_free(&stack, NULL);
			return NULL;
		}
		biggest = rb_entry(n, struct free_extent, size_rb);
		biggest_num_blocks = biggest->end_block - biggest->start_block;
		biggest_num_blocks += 1;
		obstack_grow(&stack, &n, sizeof(n));
		*extents += 1;
		*allocated_blocks += biggest_num_blocks;
		if (biggest_num_blocks > num_blocks) {
			num_blocks = 0;
		} else {
			num_blocks -= biggest_num_blocks;
			n = rb_prev(n);
		}
	}
	size = obstack_object_size(&stack);
	ret = malloc(size);
	if (ret)
		memcpy(ret, obstack_finish(&stack), size);
	obstack_free(&stack, NULL);
	return ret;
}

int allocate(struct defrag_ctx *c, struct allocation *space)
{
	int i;
	struct data_extent *extent;
	for (i = 0; i < space->extent_count; i++) {
		e2_blkcnt_t num_blocks;
		int tmp;
		extent = &space->extents[i];
		num_blocks = extent->end_block - extent->start_block + 1;
		tmp = allocate_space(c, extent->start_block, num_blocks);
		if (tmp < 0)
			goto out_dealloc;
	}
	return 0;

out_dealloc:
	for (i = i - 1; i >= 0; i--) {
		e2_blkcnt_t num_blocks;
		extent = &space->extents[i];
		num_blocks = extent->end_block - extent->start_block + 1;
		deallocate_space(c, extent->start_block, num_blocks);
	}
	return -1;
}

struct allocation *get_blocks(struct defrag_ctx *c, e2_blkcnt_t num_blocks,
                              ext2_ino_t inode_nr, blk64_t first_logical)
{
	struct allocation *ret;
	struct rb_node **nodes;
	e2_blkcnt_t num_allocated;
	int num_extents, i;

	nodes = simple_allocator(c, num_blocks, &num_allocated, &num_extents);
	if (nodes == NULL)
		return NULL;

	optimize_allocation(nodes, &num_allocated, num_blocks, num_extents);
	ret = malloc(sizeof(struct allocation)
	             + num_extents * sizeof(struct data_extent));
	if (!ret) {
		free(nodes);
		return NULL;
	}

	ret->block_count = num_blocks;
	ret->extent_count = num_extents;
	for (i = 0; i < num_extents; i++) {
		struct free_extent *extent;
		e2_blkcnt_t extent_num_blocks;
		extent = rb_entry(nodes[i], struct free_extent, size_rb);
		ret->extents[i].start_block = extent->start_block;
		ret->extents[i].start_logical = first_logical;
		ret->extents[i].end_block = extent->start_block - 1;
		ret->extents[i].inode_nr = inode_nr;
		ret->extents[i].uninit = 0;
		extent_num_blocks = extent->end_block - extent->start_block + 1;
		if (extent_num_blocks > num_blocks) {
			ret->extents[i].end_block += num_blocks;
			num_blocks = 0;
		} else {
			ret->extents[i].end_block += extent_num_blocks;
			num_blocks -= extent_num_blocks;
			first_logical += extent_num_blocks;
		}
	}
	free(nodes);
	return ret;
}

struct allocation *get_range_allocation(blk64_t start_block,
                                        e2_blkcnt_t num_blocks,
                                        blk64_t start_logical)
{
	struct allocation *ret = malloc(sizeof(struct allocation)
	                                + sizeof(struct data_extent));
	if (!ret)
		return NULL;
	ret->block_count = num_blocks;
	ret->extent_count = 1;
	ret->extents[0].start_block = start_block;
	ret->extents[0].end_block = start_block + num_blocks - 1;
	ret->extents[0].start_logical = start_logical;
	return ret;
}

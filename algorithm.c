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

#include <obstack.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include "e2defrag.h"
#include "extree.h"

/* Please note that the algorithm below is quite horribly inefficient, and
   it would almost certainly be faster to just start from the root and
   perform a binary search, than the current algorithm of performing a linear
   search from the (ideally far away) hint. */
static struct free_extent *find_free_extent(struct defrag_ctx *c,
                                            e2_blkcnt_t min_size,
                                            e2_blkcnt_t max_size,
                                            struct rb_node *hint)
{
	struct free_extent *target;
	struct rb_node *next;
	e2_blkcnt_t next_size = 0;
	if (!hint)
		hint = rb_first(&c->free_tree_by_size);
	if (!hint) {
		errno = ENOSPC;
		return NULL;
	}
	next = hint;

	while (next_size < min_size && next_size <= max_size) {
		/* Find bigger extents */
		hint = next;
		next = rb_prev(hint);
		if (!next)
			break;
		target = rb_entry(next, struct free_extent, size_rb);
		next_size = target->end_block - target->start_block + 1;
	}
	next_size = min_size + 1;
	next = hint;
	while (next_size >= min_size) {
		/* Find smaller extents */
		hint = next;
		next = rb_prev(hint);
		if (!next)
			break;
		target = rb_entry(next, struct free_extent, size_rb);
		next_size = target->end_block - target->start_block + 1;
	}
	target = rb_entry(hint, struct free_extent, size_rb);
	next_size = target->end_block - target->start_block + 1;
	if (next_size < min_size || next_size > max_size) {
		errno = ENOSPC;
		return NULL;
	}
	return target;
}

static e2_blkcnt_t real_extent_count(struct allocation *alloc)
{
	e2_blkcnt_t ret = 1;
	int i;

	for (i = 1; i < alloc->extent_count; i++) {
		if (alloc->extents[i].start_block !=
		    alloc->extents[i - 1].end_block + 1)
		{
			ret++;
		}
	}
	return ret;
}

/* Returns whether the given allocation has more extents than is strictly
 * needed on the disk. This function is slightly pessimistic (return true
 * for some allocations that are not really fragmented)
 */
static int is_fragmented(struct defrag_ctx *c, struct allocation *alloc)
{
	e2_blkcnt_t flex_bg_size = 1, min_extents;

	if (EXT2_HAS_INCOMPAT_FEATURE(&c->sb, EXT4_FEATURE_INCOMPAT_FLEX_BG))
		flex_bg_size = 1 << (c->sb.s_log_groups_per_flex);

	flex_bg_size *= c->sb.s_blocks_per_group;
	min_extents = (alloc->block_count + flex_bg_size - 1) / flex_bg_size;
	if (alloc->extent_count <= min_extents)
		return 0;

	if (real_extent_count(alloc) <= min_extents)
		return 0;
	return 1;
}

static int try_pack_extent(struct defrag_ctx *c, struct data_extent *data,
                           struct free_extent *away_from)
{
	struct free_extent *target;
	struct allocation *new_alloc;
	struct data_extent *before;
	blk64_t new_start;
	e2_blkcnt_t min_size, max_size;
	int ret;

	before = containing_data_extent(c, data->start_block - 1);
	if (before && before->inode_nr == data->inode_nr)
		return ENOSPC; /* TODO: proper multi-extent moves */
	min_size = data->end_block - data->start_block + 1;
	/* Don't add 1 to max_size (or rather, subtract one from the final
	   result, so we actually gain something by moving */
	max_size = min_size + away_from->end_block - away_from->start_block;

	target = find_free_extent(c, min_size, max_size, &away_from->size_rb);
	if (!target)
		return -1;
	if (target == away_from) {
		/* There cannot be any smaller extents, so we only try
		   the bigger ones. */
		struct rb_node *prev_node;
		prev_node = rb_prev(&target->size_rb);
		if (!prev_node) {
			errno = ENOSPC;
			return -1;
		}
		target = rb_entry(prev_node, struct free_extent, size_rb);
		if (target->end_block - target->start_block + 1 > max_size) {
			/* Can't be smaller then min_size, or find_free_block
			   wouldn't have returned the smaller target */
			errno = ENOSPC;
			return -1;
		}
	}
	new_start = target->start_block;
	if (global_settings.interactive) {
		printf("Moving extent starting at %llu (inode %u, %llu blocks)"
		       " to %llu\n",
		       data->start_block, data->inode_nr, 
		       data->end_block - data->start_block + 1, new_start);
	}
	new_alloc = malloc(sizeof(struct allocation) + sizeof(struct data_extent));
	if (!new_alloc)
		return -1;
	new_alloc->block_count = data->end_block - data->start_block + 1;
	new_alloc->extent_count = 1;
	new_alloc->extents[0].start_block = target->start_block;
	new_alloc->extents[0].end_block = target->start_block;
	new_alloc->extents[0].end_block += data->end_block - data->start_block;
	new_alloc->extents[0].start_logical = data->start_logical;
	new_alloc->extents[0].inode_nr = data->inode_nr;
	new_alloc->extents[0].uninit = data->uninit;
	ret = allocate(c, new_alloc);
	if (ret) {
		free(new_alloc);
		return -1;
	}
	if (is_metadata(c, data))
		ret = move_metadata_extent(c, data, new_alloc);
	else
		ret = move_data_extent(c, data, new_alloc);
	free(new_alloc);
	return ret;
}

int consolidate_free_space(struct defrag_ctx *c)
{
	struct rb_node *n;
	struct free_extent *free_extent;
	n = rb_last(&c->free_tree_by_size);
	if (!n) {
		if (global_settings.interactive) {
			/* TODO: Shouldn't this be a generic error message? */
			printf("No free space on disk\n");
		}
		errno = ENOSPC;
		return 1;
	}
	free_extent = rb_entry(n, struct free_extent, size_rb);
	do {
		struct data_extent *extent_before, *extent_after;
		extent_before =
		        containing_data_extent(c, free_extent->start_block - 1);
		if (extent_before) {
			int ret;
			ret = try_pack_extent(c, extent_before, free_extent);
			if (ret >= 0 || (ret < 0 && errno != ENOSPC))
				return ret;
		}
		extent_after =
		        containing_data_extent(c, free_extent->start_block - 1);
		if (extent_after) {
			int ret;
			ret = try_pack_extent(c, extent_after, free_extent);
			if (ret >= 0 || (ret < 0 && errno != ENOSPC))
				return ret;
		}
		n = rb_prev(n);
		if (n)
			free_extent = rb_entry(n, struct free_extent, size_rb);
		else
			free_extent = NULL;
	} while (free_extent);
	/*
	 * If we got out of the loop, then we have not found anything good
	 * to move.
	 */
	if (global_settings.interactive)
		printf("Could not find any way to consolidate free space\n");
	errno = ENOSPC;
	return 1;
}

/*
 * Try to improve the fragmentation status of the file by moving one single
 * extent to a position immediatly following the position of the extent
 * before it.
 */
int try_improve_inode(struct defrag_ctx *c, ext2_ino_t inode_nr)
{
	struct inode *inode = c->inodes[inode_nr];
	struct allocation *data = inode->data;
	int i, tmp, ret = 0;

	if (!is_fragmented(c, data))
		return 0;
	for (i = 1; i < data->extent_count; i++) {
		struct data_extent *cur_extent = &data->extents[i];
		struct data_extent *prev_extent = &data->extents[i - 1];
		struct free_extent *target;
		target = containing_free_extent(c, prev_extent->end_block + 1);
		if (target
		    && target->end_block - target->start_block
		       >= cur_extent->end_block - cur_extent->start_block)
		{
			struct allocation *target_alloc;
			e2_blkcnt_t num_blocks;
			num_blocks = cur_extent->end_block
				             - cur_extent->start_block + 1;
			target_alloc = get_range_allocation(
				                     prev_extent->end_block + 1,
				                     num_blocks,
				                     cur_extent->start_logical);
			if (!target_alloc)
				return -1;
			if (global_settings.interactive) {
				printf("Moving extent from %llu to %llu (%llu)\n",
				       cur_extent->start_block,
				       target_alloc->extents[0].start_block,
				       cur_extent->end_block
				               - cur_extent->start_block + 1);
			}
			target_alloc->extents[0].inode_nr = inode_nr;
			target_alloc->extents[0].uninit = cur_extent->uninit;
			tmp = allocate(c, target_alloc);
			if (tmp < 0) {
				free(target_alloc);
				return tmp;
			}
			tmp = move_data_extent(c, cur_extent, target_alloc);
			if (tmp < 0) {
				free(target_alloc);
				return tmp;
			}
			data = inode->data;
			i--; /* To allow for merged extents */
			ret++;
		}
	}
	return ret;
}

/* Very naive algorithm for now: Just try to find a combination of free
 * extent big enough to fit the whole file, but consisting of fewer extents
 * than the current one.
 * Returns 0 for success, 1 for no opportunity, -1 for error.
 */
int do_one_inode(struct defrag_ctx *c, ext2_ino_t inode_nr)
{
	struct inode *inode;
	struct allocation *target;
	int ret = 0, answer = 0;

	inode = c->inodes[inode_nr];
	errno = 0;
	target = get_blocks(c, inode->data->block_count, inode_nr, 0);
	if (!target) {
		if (errno)
			return -1;
		else
			return 1;
	}
	if (target->extent_count >= inode->data->extent_count) {
		if (global_settings.interactive)
			printf("No better placement possible: best new placement has %llu fragments\n", target->extent_count);
		free(target);
		if (ret < 0)
			return ret;
		else
			return 1;
	}
	if (target->extent_count < 2)
		answer = 'y';
	while (!answer && global_settings.interactive) {
		printf("Possible allocation has %llu fragments. Continue? ",
		       target->extent_count);
		answer = getchar();
		if (answer != 'y' && answer != 'Y' && answer != 'n' &&
		    answer != 'N' && answer != EOF)
			answer = 0;
		while (ret != '\n')
			ret = getchar();
	}
	if (!global_settings.interactive || answer == 'y' || answer == 'Y') {
		ret = allocate(c, target);
		if (ret < 0) {
			free(target);
			return ret;
		}
		ret = copy_data(c, inode->data, &target);
		if (!ret) {
			ret = deallocate_blocks(c, inode->data);
			inode->data = target;
			if (!ret)
				ret = write_inode_metadata(c, inode);
			else
				write_inode_metadata(c, inode);
		} else {
			deallocate_blocks(c, target);
		}
	}
	return ret;
}

/* Very stupid algorithm: Start by defragmenting every file starting at inode
   0 until no more inodes can be defragmented, then consolidate the free space
   as much as possible and start over. When nothing more can be done, it
   terminates. */
int do_whole_disk(struct defrag_ctx *c)
{
	ext2_ino_t i;
	int ret;
	char changed, optimal;
	do {
		changed = 0;
		optimal = 1;
		for (i = 0; i < ext2_inodes_on_disk(&c->sb); i++) {
			struct inode *inode = c->inodes[i];
			if (!inode)
				continue;
			if (is_fragmented(c, inode->data)) {
				ret = do_one_inode(c, i);
				if (ret < 0)
					return ret;
				else if (ret == 0)
					changed = 1;
				/* ret == 1 means could not improve */
				if (is_fragmented(c, inode->data))
					optimal = 0;
			}
			if (inode->metadata &&
			    is_fragmented(c, inode->metadata))
			{
				ret = write_inode_metadata(c, inode);
				if (ret < 0)
					return ret;
				changed = 1;
				/* TODO: Too coupled with the algorithm.
				   Will break on improved algorithm later.
				   */
				if (is_fragmented(c, inode->metadata))
					optimal = 0;
			}
		}
		if (!optimal) {
			ret = consolidate_free_space(c);
			if (ret < 0)
				return ret;
			else if (ret == 0)
				changed = 1;
		}
	} while (changed && !optimal);
	return 0;
}

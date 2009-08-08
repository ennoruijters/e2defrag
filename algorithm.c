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

static int try_pack_extent(struct defrag_ctx *c, struct data_extent *data,
                           struct free_extent *away_from, int silent)
{
	struct free_extent *target;
	struct allocation *new_alloc;
	blk64_t new_start;
	e2_blkcnt_t min_size, max_size;
	int ret;

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
	if (!silent) {
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
	new_alloc->extents[0].sparse = NULL;
	new_alloc->extents[0].inode_nr = data->inode_nr;
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

int consolidate_free_space(struct defrag_ctx *c, int silent)
{
	struct rb_node *n;
	struct free_extent *free_extent;
	n = rb_last(&c->free_tree_by_size);
	if (!n) {
		if (!silent)
			printf("No free space on disk\n");
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
			ret = try_pack_extent(c, extent_before, free_extent,
			                      silent);
			if (ret >= 0 || (ret < 0 && errno != ENOSPC))
				return ret;
		}
		extent_after =
		        containing_data_extent(c, free_extent->start_block - 1);
		if (extent_after) {
			int ret;
			ret = try_pack_extent(c, extent_after, free_extent,
			                      silent);
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
	if (!silent)
		printf("Could not find any way to consolidate free space\n");
	errno = ENOSPC;
	return 1;
}

/* Very naive algorithm for now: Just try to find a combination of free
 * extent big enough to fit the whole file, but consisting of fewer extents
 * than the current one.
 * Returns 0 for success, 1 for no opportunity, -1 for error.
 */
int do_one_inode(struct defrag_ctx *c, ext2_ino_t inode_nr, int silent)
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
		if (!silent)
			printf("No better placement possible: best new placement has %llu fragments\n", target->extent_count);
		free(target);
		if (ret < 0)
			return ret;
		else
			return 1;
	}
	while (!answer) {
		printf("Possible allocation has %llu fragments. Continue? ",
		       target->extent_count);
		answer = getchar();
		if (answer != 'y' && answer != 'Y' && answer != 'n' &&
		    answer != 'N' && answer != EOF)
			answer = 0;
		while (ret != '\n')
			ret = getchar();
	}
	ret = allocate(c, target);
	if (ret < 0) {
		free(target);
		return ret;
	}
	if (answer == 'y' || answer == 'Y') {
		ret = copy_data(c, inode->data, target);
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

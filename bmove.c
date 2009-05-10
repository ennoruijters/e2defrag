/* File for performing block moves */

#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdio.h>
#include "e2defrag.h"
#include "extree.h"

static int transfer_pipe[2] = {-1, -1};

static int __move_block_range(struct defrag_ctx *c, blk64_t from, blk64_t to,
                              size_t nr_blocks)
{
	int ret, size;
	loff_t from_offset, to_offset;

	if (transfer_pipe[0] < 0) {
		ret = pipe(transfer_pipe);
		if (ret)
			return ret;
	}
	from_offset = from * EXT2_BLOCK_SIZE(&c->sb);
	to_offset = to * EXT2_BLOCK_SIZE(&c->sb);
	size = EXT2_BLOCK_SIZE(&c->sb) * nr_blocks;
	while (size > 0) {
		int to_write;
		ret = splice(c->fd, &from_offset, transfer_pipe[1], NULL,
		             size, SPLICE_F_MOVE);
		if (ret < 0)
			return ret;
		else
			size -= ret;
		to_write = ret;
		while (to_write > 0) {
			ret = splice(transfer_pipe[0], NULL, c->fd, &to_offset,
			             to_write, SPLICE_F_MOVE);
			if (ret < 0)
				return ret;
			else
				to_write -= ret;
		}
	}
	return 0;
}

int move_file_extent(struct defrag_ctx *c, struct inode *i,
                     blk64_t logical_start, blk64_t new_start)
{
	struct data_extent *extent_to_copy = NULL;
	blk64_t old_start;
	int j, ret;
	e2_blkcnt_t blk_cnt;
	for (j = 0; j < i->extent_count; j++) {
		if (i->extents[j].start_logical == logical_start) {
			extent_to_copy = &i->extents[j];
			break;
		}
	}
	if (!extent_to_copy) {
		errno = EINVAL;
		return -1;
	}
	blk_cnt = extent_to_copy->end_block - extent_to_copy->start_block + 1;
	ret = allocate_space(c, new_start, blk_cnt);
	if (ret)
		return -1;
	ret = __move_block_range(c, extent_to_copy->start_block, new_start,
	                         blk_cnt);
	if (!ret)
		ret = fdatasync(c->fd);
	if (ret) {
		deallocate_space(c, new_start, blk_cnt);
		return -1;
	}
	old_start = extent_to_copy->start_block;
	extent_to_copy->start_block = new_start;
	extent_to_copy->end_block = extent_to_copy->start_block + blk_cnt - 1;
	ret = write_extent_metadata(c, extent_to_copy);
	if (!ret) {
		ret = deallocate_space(c, old_start, blk_cnt);
		if (!ret) {
			ret = try_extent_merge(c, i, extent_to_copy);
			if (!ret) {
				rb_erase(&extent_to_copy->block_rb,
				         &c->extents_by_block);
				insert_data_extent_by_block(c, extent_to_copy);
				/* Extent size has not changed */
			}
			return ret;
		}
	}
	if (ret) {
		/* TODO: graceful error handling */
	}
	return ret;
}

/* Move 'numblocks' blocks of the file with inode 'inode_nr' to block number
 * 'dest', starting from the logical block 'from'
 */
int move_file_range(struct defrag_ctx *c, ext2_ino_t inode_nr, blk64_t from,
                    e2_blkcnt_t numblocks, blk64_t dest)
{
	struct inode *inode = c->inodes[inode_nr];
	struct free_extent *free_extent;
	int extent_nr, ret;
	blk64_t start;

	assert(!from);
	start = get_physical_block(inode, from, &extent_nr);
	if (inode->extents[extent_nr].start_block != start) {
		ret = split_extent(c, inode, &inode->extents[extent_nr], start);
		inode = c->inodes[inode_nr];
		extent_nr++;
	}
	free_extent = containing_free_extent(c, dest);
	while (numblocks && extent_nr < inode->extent_count) {
		struct data_extent *extent = &inode->extents[extent_nr];
		blk64_t end_block;
		if (!free_extent) {
			printf("A malfunction has occured: tried to write past "
			       "end of disk\n");
			errno = ENOENT;
			return -1;
		}
		if (numblocks < extent->end_block - extent->start_block + 1) {
			blk64_t new_end_block = extent->start_block + numblocks;
			ret = split_extent(c, inode, extent, new_end_block);
			if (ret < 0)
				return ret;
			inode = c->inodes[inode_nr]; /* might have changed */
			extent = &inode->extents[extent_nr];
		}
		end_block = dest + (extent->end_block - extent->start_block);
		numblocks -= extent->end_block - extent->start_block + 1;
		if (end_block > free_extent->end_block) {
			blk64_t new_end_block = extent->start_block
			                      + (free_extent->end_block - dest);
			ret = split_extent(c, inode, extent, new_end_block);
			if (ret < 0)
				return ret;
			inode = c->inodes[inode_nr]; /* might have changed */
			extent = &inode->extents[extent_nr];
		}
		ret = move_file_extent(c, inode, extent->start_logical, dest);
		if (ret < 0)
			return ret;
		extent_nr -= ret;
		inode = c->inodes[inode_nr]; /* might have changed */
		extent = &inode->extents[extent_nr];
		dest = extent->end_block + 1;
		++extent_nr;
		if (extent_nr < inode->extent_count) {
			free_extent = free_extent_after(c, dest);
			if (free_extent) {
				dest = free_extent->start_block;
			}
		}
	}
	return 0;
}

int move_file_data(struct defrag_ctx *c, ext2_ino_t inode_nr, blk64_t dest)
{
	struct inode *inode = c->inodes[inode_nr];
	return move_file_range(c, inode_nr, 0, inode->block_count, dest);
}

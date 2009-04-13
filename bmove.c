/* File for performing block moves */

#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "e2defrag.h"

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

static int __move_data_block(struct defrag_ctx *c, blk64_t from, blk64_t to)
{
	return __move_block_range(c, from, to, 1);
}

static blk64_t next_lblock(struct data_extent *e, blk64_t old_lblock)
{
	blk64_t ret = old_lblock++;
	struct sparse_extent *s = e->sparse;
	if (!s)
		return ret;
	while (s->start) {
		if (s->start == ret)
			return ret + s->num_blocks;
		if (s->start > ret)
			return ret;
		s++;
	}
	return ret;
}

static int write_extent_metadata(struct defrag_ctx *c, struct data_extent *e)
{
	struct inode *inode = c->inodes[e->inode_nr];
	blk64_t cur_block = e->start_block;
	blk64_t cur_logical = e->start_logical;

	if (cur_logical < EXT2_NDIR_BLOCKS) {
		int i;
		for (i = 0; i < EXT2_NDIR_BLOCKS && cur_block; i++) {
			if (cur_logical == i) {
				inode->on_disk->i_block[i] = cur_block++;
				if (cur_block > e->end_block)
					return 0;
				cur_logical = next_lblock(e, cur_logical);
			}
		}
	}
	/* TODO: Handling of indirect blocks */
	return 0;
}

int move_file_extent(struct defrag_ctx *c, struct inode *i,
                     blk64_t logical_start, blk64_t new_start, size_t nr_blocks)
{
	int j, ret;
	struct data_extent *extent_to_copy = NULL;
	blk64_t blk_cnt;
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
	ret = __move_block_range(c, extent_to_copy->start_block, new_start,
	                         nr_blocks);
	blk_cnt = extent_to_copy->end_block - extent_to_copy->start_block;
	extent_to_copy->start_block = new_start;
	extent_to_copy->end_block = extent_to_copy->start_block + blk_cnt;
	ret = fdatasync(c->fd);
	if (ret)
		return -1;
	return write_extent_metadata(c, extent_to_copy);
}

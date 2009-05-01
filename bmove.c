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

static int is_sparse(struct data_extent *e, blk64_t lblock)
{
	struct sparse_extent *s = e->sparse;
	if (!s)
		return 0;
	while (s->start) {
		if (s->start >= lblock && s->start + s->num_blocks <= lblock)
			return 1;
		if (s->start + s->num_blocks > lblock)
			return 0;
		s++;
	}
	return 0;
}

static int write_ind_metadata(struct defrag_ctx *c, struct data_extent *e,
                              __u32 ind_block, __u32 *cur_logical,
                              __u32 *cur_block)
{
	__u32 offset = *cur_logical;
	__u32 ind_blocks = EXT2_ADDR_PER_BLOCK(&c->sb);
	__u32 blocks_per_ind = 1 + ind_blocks;
	__u32 blocks_per_dind = 1 + ind_blocks * blocks_per_ind;
	__u32 buffer[EXT2_ADDR_PER_BLOCK(&c->sb)];
	int ret;
	char to_sync = 0;

	if (ind_block == 0) {
		*cur_logical += EXT2_ADDR_PER_BLOCK(&c->sb);
		return 0;
	}
	if (offset > EXT2_TIND_LBLOCK(&c->sb))
		offset -= EXT2_TIND_LBLOCK(&c->sb) + 3;
	else if (offset > EXT2_DIND_LBLOCK(&c->sb))
		offset -= EXT2_DIND_LBLOCK(&c->sb) + 2;
	else
		offset -= EXT2_IND_LBLOCK(&c->sb) + 1;
	offset = offset % blocks_per_dind;
	offset = offset % blocks_per_ind;
	ret = read_block(c, buffer, ind_block);
	if (ret)
		return -1;
	while (offset < EXT2_ADDR_PER_BLOCK(&c->sb)
	       && *cur_block <= e->end_block) {
		__u32 new_block;
		if (is_sparse(e, *cur_logical))
			new_block = 0;
		else
			new_block = (*cur_block)++;
		(*cur_logical)++;
		if (buffer[offset] != new_block) {
			to_sync = 1;
			buffer[offset] = new_block;
		}
		offset++;
	}
	if (to_sync) {
		ret = write_block(c, buffer, ind_block);
		return ret;
	}
	return 0;
}

static int write_dind_metadata(struct defrag_ctx *c, struct data_extent *e,
                              __u32 dind_block, __u32 *cur_logical,
                              __u32 *cur_block)
{
	__u32 offset = *cur_logical;
	__u32 ind_blocks = EXT2_ADDR_PER_BLOCK(&c->sb);
	__u32 blocks_per_ind = 1 + ind_blocks;
	__u32 blocks_per_dind = 1 + ind_blocks * blocks_per_ind;
	__u32 buffer[EXT2_ADDR_PER_BLOCK(&c->sb)];
	int ret;
	char to_sync = 0;

	if (dind_block == 0) {
		*cur_logical += blocks_per_dind;
		return 0;
	}
	if (offset > EXT2_TIND_LBLOCK(&c->sb))
		offset -= EXT2_TIND_LBLOCK(&c->sb) + 2;
	else
		offset -= EXT2_DIND_LBLOCK(&c->sb) + 1;
	offset = offset % blocks_per_dind;
	ret = read_block(c, buffer, dind_block);
	if (ret)
		return -1;
	if (offset % blocks_per_ind) {
		offset = offset / blocks_per_ind;
		ret = write_ind_metadata(c, e, buffer[offset], cur_logical,
		                         cur_block);
		if (ret)
			return ret;
		offset++;
		/* Advance to next offset */
	} else {
		offset = offset / blocks_per_ind;
	}
	while (offset < EXT2_ADDR_PER_BLOCK(&c->sb)
	                                        && *cur_block <= e->end_block) {
		__u32 new_block;
		if (is_sparse(e, *cur_logical))
			new_block = 0;
		else
			new_block = (*cur_block)++;
		(*cur_logical)++;
		if (new_block) {
			ret = write_ind_metadata(c, e, new_block,
			                         cur_logical, cur_block);
			if (ret)
				return ret;
		} else {
			*cur_logical += ind_blocks;
		}
		if (buffer[offset] != new_block) {
			to_sync = 1;
			buffer[offset] = new_block;
		}
		offset++;
	}
	if (to_sync) {
		ret = write_block(c, buffer, dind_block);
		return ret;
	}
	return 0;
}

static int write_tind_metadata(struct defrag_ctx *c, struct data_extent *e,
                              __u32 tind_block, __u32 *cur_logical,
                              __u32 *cur_block)
{
	__u32 offset = *cur_logical;
	__u32 ind_blocks = EXT2_ADDR_PER_BLOCK(&c->sb);
	__u32 blocks_per_ind = 1 + ind_blocks;
	__u32 blocks_per_dind = 1 + ind_blocks * blocks_per_ind;
	__u32 blocks_per_tind = 1 + ind_blocks * blocks_per_dind;
	__u32 buffer[EXT2_ADDR_PER_BLOCK(&c->sb)];
	int ret;
	char to_sync = 0;

	if (tind_block == 0) {
		*cur_logical += blocks_per_tind;
		return 0;
	}
	ret = read_block(c, buffer, tind_block);
	if (ret)
		return -1;
	offset -= EXT2_TIND_LBLOCK(&c->sb) + 1;
	offset = offset % blocks_per_tind;
	if (offset % blocks_per_dind) {
		offset = offset / blocks_per_dind;
		ret = write_dind_metadata(c, e, buffer[offset], cur_logical,
		                         cur_block);
		if (ret)
			return ret;
		offset++;
	} else {
		offset = offset / blocks_per_dind;
	}
	while (offset < EXT2_ADDR_PER_BLOCK(&c->sb)
	                                        && *cur_block <= e->end_block) {
		__u32 new_block;
		if (is_sparse(e, *cur_logical))
			new_block = 0;
		else
			new_block = (*cur_block)++;
		(*cur_logical)++;
		if (new_block) {
			ret = write_dind_metadata(c, e, new_block,
			                          cur_logical, cur_block);
			if (ret)
				return ret;
		} else {
			*cur_logical += blocks_per_dind - 1;
		}
		if (buffer[offset] != new_block) {
			to_sync = 1;
			buffer[offset] = new_block;
		}
		offset++;
	}
	if (to_sync) {
		ret = write_block(c, buffer, tind_block);
		return ret;
	}
	return 0;
}

static int write_extent_metadata(struct defrag_ctx *c, struct data_extent *e)
{
	struct inode *inode = c->inodes[e->inode_nr];
	__u32 cur_block = e->start_block;
	__u32 cur_logical = e->start_logical;
	__u32 new_block;
	int sync_inode = 0;

	/* Direct blocks */
	for (; cur_logical < EXT2_IND_LBLOCK(&c->sb); cur_logical++) {
		if (!is_sparse(e, cur_logical) && cur_block <= e->end_block)
			new_block = cur_block++;
		else
			new_block = 0;
		if (inode->on_disk->i_block[cur_logical] != new_block) {
			inode->on_disk->i_block[cur_logical] = new_block;
			sync_inode = 1;
		}
	}

	/* Singly indirect blocks */
	if (cur_logical == EXT2_IND_LBLOCK(&c->sb)) {
		if (is_sparse(e, cur_logical) || cur_block > e->end_block)
			new_block = 0;
		else
			new_block = cur_block++;
		cur_logical++;
	} else {
		new_block = inode->on_disk->i_block[EXT2_IND_BLOCK];
	}
	if (cur_logical > EXT2_IND_LBLOCK(&c->sb)
	    && cur_logical < EXT2_DIND_LBLOCK(&c->sb)) {
		write_ind_metadata(c, e, new_block, &cur_logical, &cur_block);
	}
	if (inode->on_disk->i_block[EXT2_IND_BLOCK] != new_block) {
		inode->on_disk->i_block[EXT2_IND_BLOCK] = new_block;
		sync_inode = 1;
	}

	/* Doubly indirect blocks */
	if (cur_logical == EXT2_DIND_LBLOCK(&c->sb)) {
		if (is_sparse(e, cur_logical) || cur_block > e->end_block)
			new_block = 0;
		else
			new_block = cur_block++;
		cur_logical++;
	} else {
		new_block = inode->on_disk->i_block[EXT2_DIND_BLOCK];
	}
	if (cur_logical > EXT2_DIND_LBLOCK(&c->sb)
	    && cur_logical < EXT2_TIND_LBLOCK(&c->sb)) {
		write_dind_metadata(c, e, new_block, &cur_logical, &cur_block);
	}
	if (inode->on_disk->i_block[EXT2_DIND_BLOCK] != new_block) {
		inode->on_disk->i_block[EXT2_DIND_BLOCK] = new_block;
		sync_inode = 1;
	}

	/* Triply indirect blocks */
	if (cur_logical == EXT2_TIND_LBLOCK(&c->sb)) {
		if (is_sparse(e, cur_logical) || cur_block > e->end_block)
			new_block = 0;
		else
			new_block = cur_block++;
		cur_logical++;
	} else {
		new_block = inode->on_disk->i_block[EXT2_TIND_BLOCK];
	}
	if (cur_logical > EXT2_TIND_LBLOCK(&c->sb)) {
		write_tind_metadata(c, e, new_block, &cur_logical, &cur_block);
	}
	if (inode->on_disk->i_block[EXT2_TIND_BLOCK] != new_block) {
		inode->on_disk->i_block[EXT2_TIND_BLOCK] = new_block;
		sync_inode = 1;
	}

	if (sync_inode)
		/* Assumes the inode is completely within one page */
		msync(PAGE_START(inode->on_disk), getpagesize(), MS_SYNC);
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

int move_file_data(struct defrag_ctx *c, ext2_ino_t inode_nr, blk64_t dest)
{
	struct inode *inode = c->inodes[inode_nr];
	int extent_nr, ret;
	blk64_t next_dest = dest;

	for (extent_nr = 0; extent_nr < inode->extent_count; extent_nr++) {
		struct data_extent *extent = &inode->extents[extent_nr];
		struct free_extent *free_extent, *next_extent;
		blk64_t end_block;
		dest = next_dest;
		if (!dest) {
			printf("A malfunction has occured: tried to write past "
			       "end of disk\n");
			errno = ENOENT;
			return -1;
		}
		free_extent = containing_free_extent(c, dest);
		if (!free_extent) {
			errno = EINVAL;
			return -1;
		}
		if (rb_next(&free_extent->block_rb) != NULL) {
			next_extent = rb_entry(rb_next(&free_extent->block_rb),
			                       struct free_extent, block_rb);
			next_dest = next_extent->start_block;
		} else {
			next_dest = 0;
		}
		end_block = dest + (extent->end_block - extent->start_block);
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
	}
	return 0;
}

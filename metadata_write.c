/* File for writing back the metadata of modified inodes */

#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdio.h>
#include "e2defrag.h"
#include "extree.h"

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

static int write_direct_mapping(struct defrag_ctx *c, struct data_extent *e)
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

int write_extent_metadata(struct defrag_ctx *c, struct data_extent *e)
{
	struct inode *inode = c->inodes[e->inode_nr];

	if (inode->uses_extents) {
		errno = EINVAL;
		return -1;
	} else {
		return write_direct_mapping(c, e);
	}
}

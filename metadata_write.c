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

/* File for writing back the metadata of modified inodes */

#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <obstack.h>
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include <stdio.h>
#include "e2defrag.h"
#include "extree.h"

#define EE_BLOCK_SET(extent, block) \
	((extent)->ee_start = ((block) & 0xFFFFFFFF), \
	 (extent)->ee_start_hi = ((block) >> 32))

#define EI_LEAF_SET(index, block) \
	((index)->ei_leaf = ((block) & 0xFFFFFFFF), \
	 (index)->ei_leaf_hi = ((block) >> 32))

#define EE_BLOCK(extent) \
	((extent)->ee_start + (((blk64_t) ((extent)->ee_start_hi)) >> 32))

#define EI_BLOCK(idx) \
	((idx)->ei_leaf + (((blk64_t) ((idx)->ei_leaf_hi)) >> 32))

#define EXT_PER_BLOCK(sb) \
	((EXT2_BLOCK_SIZE(sb) - sizeof(struct ext3_extent_header)) \
	 / sizeof(struct ext3_extent))
#define EXTENT_LEN(extent) \
	((extent)->end_block - (extent)->start_block + 1)

static int is_sparse(struct inode *inode, blk64_t lblock)
{
	struct sparse_extent *s = inode->sparse;
	if (!s)
		return 0;
	while (s - inode->sparse < inode->num_sparse) {
		if (s->start <= lblock && s->start + s->num_blocks-1 >= lblock)
			return 1;
		if (s->start + s->num_blocks - 1 > lblock)
			return 0;
		s++;
	}
	return 0;
}

static int write_ind_metadata(struct defrag_ctx *c, struct data_extent *e,
                              __u32 ind_block, __u32 *cur_logical,
                              __u32 *cur_block)
{
	struct inode *inode = c->inodes[e->inode_nr];
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
		if (is_sparse(inode, *cur_logical))
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
	struct inode *inode = c->inodes[e->inode_nr];
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
		if (is_sparse(inode, *cur_logical))
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
	struct inode *inode = c->inodes[e->inode_nr];
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
		if (is_sparse(inode, *cur_logical))
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

static int write_direct_mapping(struct defrag_ctx *c, struct data_extent *e,
                                journal_trans_t *trans)
{
	/* Transaction safety note: The new indirect blocks do not need to
	 * be in a transaction, because the ordering guarantee ensures they
	 * will be on-disk before the inode points to them. Because the
	 * metadata blocks are included in the data allocation, they also
	 * do not need to be protected from other transactions overwriting
	 * them.
	 *
	 * We don't need to journal_ensure_unprotected on the new blocks,
	 * because they are part of the data allocation (so whoever put them
	 * there should have ensured they are not protected).
	 */
	struct inode *inode = c->inodes[e->inode_nr];
	__u32 cur_block = e->start_block;
	__u32 cur_logical = e->start_logical;
	__u32 new_block;
	int sync_inode = 0;

	/* Direct blocks */
	for (cur_logical = e->start_logical;
	     cur_logical < EXT2_IND_LBLOCK(&c->sb) && cur_block <= e->end_block;
	     cur_logical++) {
		if (!is_sparse(inode, cur_logical))
			new_block = cur_block++;
		else
			new_block = 0;
		if (inode->on_disk.i_block[cur_logical] != new_block) {
			inode->on_disk.i_block[cur_logical] = new_block;
			sync_inode = 1;
		}
	}
	if (cur_block > e->end_block)
		goto out;

	/* Singly indirect blocks */
	if (cur_logical == EXT2_IND_LBLOCK(&c->sb)) {
		if (is_sparse(inode, cur_logical))
			new_block = 0;
		else
			new_block = cur_block++;
		cur_logical++;
	} else {
		new_block = inode->on_disk.i_block[EXT2_IND_BLOCK];
	}
	if (cur_logical > EXT2_IND_LBLOCK(&c->sb)
	    && cur_logical < EXT2_DIND_LBLOCK(&c->sb)) {
		write_ind_metadata(c, e, new_block, &cur_logical, &cur_block);
	}
	if (inode->on_disk.i_block[EXT2_IND_BLOCK] != new_block) {
		inode->on_disk.i_block[EXT2_IND_BLOCK] = new_block;
		sync_inode = 1;
	}
	if (cur_block > e->end_block)
		goto out;

	/* Doubly indirect blocks */
	if (cur_logical == EXT2_DIND_LBLOCK(&c->sb)) {
		if (is_sparse(inode, cur_logical) || cur_block > e->end_block)
			new_block = 0;
		else
			new_block = cur_block++;
		cur_logical++;
	} else {
		new_block = inode->on_disk.i_block[EXT2_DIND_BLOCK];
	}
	if (cur_logical > EXT2_DIND_LBLOCK(&c->sb)
	    && cur_logical < EXT2_TIND_LBLOCK(&c->sb)) {
		write_dind_metadata(c, e, new_block, &cur_logical, &cur_block);
	}
	if (inode->on_disk.i_block[EXT2_DIND_BLOCK] != new_block) {
		inode->on_disk.i_block[EXT2_DIND_BLOCK] = new_block;
		sync_inode = 1;
	}
	if (cur_block > e->end_block)
		goto out;

	/* Triply indirect blocks */
	if (cur_logical == EXT2_TIND_LBLOCK(&c->sb)) {
		if (is_sparse(inode, cur_logical) || cur_block > e->end_block)
			new_block = 0;
		else
			new_block = cur_block++;
		cur_logical++;
	} else {
		new_block = inode->on_disk.i_block[EXT2_TIND_BLOCK];
	}
	if (cur_logical > EXT2_TIND_LBLOCK(&c->sb)) {
		write_tind_metadata(c, e, new_block, &cur_logical, &cur_block);
	}
	if (inode->on_disk.i_block[EXT2_TIND_BLOCK] != new_block) {
		inode->on_disk.i_block[EXT2_TIND_BLOCK] = new_block;
		sync_inode = 1;
	}

out:
	if (sync_inode)
		return write_inode(c, e->inode_nr, trans);
	return 0;
}

static void extent_to_ext3_extent(const struct inode *inode,
                                  const struct data_extent *_extent,
                                  struct obstack *mempool)
{
	struct data_extent extent = *_extent;
	const struct sparse_extent *sparse;
	int sparse_nr = 0;

	sparse = inode->sparse;
	while (sparse_nr < inode->num_sparse
	                                && sparse->start < extent.start_logical)
	{
		sparse++;
		sparse_nr++;
	}
	if (sparse_nr >= inode->num_sparse)
		sparse = NULL;

	while (extent.start_block <= extent.end_block) {
		struct ext3_extent new_extent;
		e2_blkcnt_t length;
		new_extent.ee_block = extent.start_logical;
		EE_BLOCK_SET(&new_extent, extent.start_block);
		length = extent.end_block - extent.start_block + 1;
		if (!extent.uninit && length > EXT_INIT_MAX_LEN)
			length = EXT_INIT_MAX_LEN;
		else if (extent.uninit && length > EXT_UNINIT_MAX_LEN)
			length = EXT_UNINIT_MAX_LEN;
		if (sparse && sparse->start < extent.start_logical + length)
			length = sparse->start - extent.start_logical;
		extent.start_block += length;
		extent.start_logical += length;
		if (sparse && sparse->start == extent.start_logical) {
			extent.start_logical += sparse->num_blocks;
			sparse_nr++;
			if (sparse_nr < inode->num_sparse)
				sparse++;
			else
				sparse = NULL;
		}
		if (!extent.uninit)
			new_extent.ee_len = length;
		else
			new_extent.ee_len = length + EXT_INIT_MAX_LEN;
		if (length)
			obstack_grow(mempool, &new_extent, sizeof(new_extent));
	}
}

static e2_blkcnt_t calc_num_indexes(struct defrag_ctx *c, e2_blkcnt_t nextents)
{
	e2_blkcnt_t ret = 0;
	assert(nextents > 4);

	nextents += EXT_PER_BLOCK(&c->sb) - 1;
	nextents /= EXT_PER_BLOCK(&c->sb);
	while (nextents > 4) {
		ret += nextents / EXT_PER_BLOCK(&c->sb);
		if (nextents % EXT_PER_BLOCK(&c->sb))
			ret += 1;
		nextents /= EXT_PER_BLOCK(&c->sb);
	}
	return ret;
}

static int update_metadata_move(struct defrag_ctx *c, struct inode *inode,
                                blk64_t from, blk64_t to, __u32 logical,
				blk64_t at_block, journal_trans_t *transaction)
{
	int ret = 0;
	struct ext3_extent_header *header;
	struct ext3_extent_idx *idx;
	if (at_block == 0) {
		header = &inode->on_disk.extents.hdr;
	} else {
		header = malloc(EXT2_BLOCK_SIZE(&c->sb));
		ret = read_block(c, header, at_block);
		if (ret)
			goto out_noupdate;
	}
	if (!header->eh_depth) {
		errno = EINVAL;
		goto out_noupdate;
	}
	for (idx = EXT_FIRST_INDEX(header);
	     idx <= EXT_LAST_INDEX(header); idx++) {
		if (idx->ei_block > logical) {
			errno = EINVAL;
			goto out_noupdate;
		} if (idx->ei_block == logical && EI_BLOCK(idx) == from) {
			EI_LEAF_SET(idx, to);
			goto out_update;
		}
		if (idx + 1 > EXT_LAST_INDEX(header) ||
		    (idx + 1)->ei_block > logical) {
			ret = update_metadata_move(c, inode, from, to, logical,
			                           EI_BLOCK(idx), transaction);
			goto out_noupdate;
		}
	}
	errno = EINVAL;
	goto out_noupdate;

out_update:
	if (at_block) {
		ret = journal_write_block(transaction, at_block, header);
	} else {
		ext2_ino_t inode_nr;
		/* I think it's safe to assume this inode has data */
		inode_nr = inode->data->extents[0].inode_nr;
		ret = write_inode(c, inode_nr, transaction);
	}
out_noupdate:
	if (at_block)
		free(header);
	return ret;
}

static int move_metadata_block(struct defrag_ctx *c, struct inode *inode,
                               blk64_t from, blk64_t to, journal_trans_t *t)
{
	struct ext3_extent_header *header = malloc(EXT2_BLOCK_SIZE(&c->sb));
	/* Whether it's a leaf or an index doesn't matter for the logical
	   block address */
	struct ext3_extent *extents = (void *)(header + 1);
	__u32 logical_block;
	int ret;

	ret = read_block(c, header, from);
	if (ret)
		goto out_error;
	ret = journal_write_block(t, to, header);
	if (ret)
		goto out_error;
	logical_block = extents->ee_block;
	free(header);
	return update_metadata_move(c, inode, from, to, logical_block, 0, t);

out_error:
	free(header);
	return ret;
}

int move_metadata_extent(struct defrag_ctx *c, struct data_extent *extent,
                         struct allocation *target, journal_trans_t *trans)
{
	struct inode *inode = c->inodes[extent->inode_nr];
	blk64_t target_block, i;
	int ret;
	if (target->extent_count > 1) {
		errno = ENOSYS;
		return -1;
	}
	if (target->extents[0].end_block - target->extents[0].start_block !=
	    extent->end_block - extent->start_block)
	{
		errno = ENOSPC;
		return -1;
	}
	target_block = target->extents[0].start_block;
	for (i = extent->start_block; i != extent->end_block + 1; i++) {
		ret = move_metadata_block(c, inode, i, target_block, trans);
		if (ret)
			return ret;
	}
	ret = deallocate_space(c, extent->start_block,
	                       extent->end_block - extent->start_block + 1,
	                       trans);
	rb_remove_data_extent(c, extent);
	extent->end_block -= extent->start_block;
	extent->start_block = target->extents[0].start_block;
	extent->end_block += extent->start_block;
	insert_data_extent(c, extent);
	return 0;
}

static int write_extent_block(struct defrag_ctx *c, blk64_t block,
                              struct ext3_extent *data,
                              e2_blkcnt_t max_extents, int depth,
                              struct obstack *index_mempool)
{
	struct ext3_extent_header *header = calloc(1, EXT2_BLOCK_SIZE(&c->sb));
	struct ext3_extent_idx *extents = (void *)(header + 1);
	int ret;

	assert(depth < 3);
	if (header == NULL)
		return -1;
	header->eh_magic = EXT3_EXT_MAGIC;
	header->eh_entries = EXT_PER_BLOCK(&c->sb);
	if (header->eh_entries > max_extents)
		header->eh_entries = max_extents;
	header->eh_max = EXT_PER_BLOCK(&c->sb);
	header->eh_generation = 0;
	header->eh_depth = depth;
	memcpy(extents, data, header->eh_entries * sizeof(struct ext3_extent));
	ret = journal_ensure_unprotected(c, block, block);
	if (!ret)
		ret = write_block(c, header, block);
	if (ret >= 0) {
		EI_LEAF_SET(extents, block);
		extents->ei_block = data->ee_block;
		extents->ei_unused = 0;
		obstack_grow(index_mempool, extents, sizeof(*extents));
		ret = header->eh_entries;
	}
	free(header);
	return ret;
}

/* When returning, the final sequence of indexes in the growing object on
 * the mempool.
 */
static int writeout_extents(struct defrag_ctx *c, struct ext3_extent *leaves,
                            struct allocation *target, e2_blkcnt_t num_extents,
                            int *depth, struct obstack *index_mempool)
{
	struct data_extent *current_extent = target->extents;
	struct ext3_extent *current_leaf = leaves;
	blk64_t block;
	e2_blkcnt_t num_preceding_blocks = calc_num_indexes(c, num_extents);
	while (num_preceding_blocks > EXTENT_LEN(current_extent)) {
		num_preceding_blocks -= EXTENT_LEN(current_extent);
		current_extent++;
	}
	block = current_extent->start_block + num_preceding_blocks;
	while (current_leaf - leaves < num_extents) {
		e2_blkcnt_t extents_left;
		int ret;
		if (block > current_extent->end_block) {
			current_extent++;
			block = current_extent->start_block;
		}
		extents_left = num_extents - (current_leaf - leaves);
		ret = write_extent_block(c, block, current_leaf, extents_left,
		                         *depth, index_mempool);
		if (ret < 0)
			return ret;
		current_leaf += ret;
		block++;
	}
	*depth += 1;
	if (obstack_object_size(index_mempool) / sizeof(*leaves) > 4) {
		num_extents = obstack_object_size(index_mempool)
		                                   / sizeof(struct ext3_extent);
		leaves = obstack_finish(index_mempool);
		return writeout_extents(c, leaves, target, num_extents,
		                        depth, index_mempool);
	} else {
		return 0;
	}
}

static void update_inode_extents(struct inode *inode,
                                 struct ext3_extent *entries,
                                 e2_blkcnt_t num_entries, int depth)
{
	assert(num_entries <= 4);
	inode->on_disk.extents.hdr.eh_magic = EXT3_EXT_MAGIC;
	inode->on_disk.extents.hdr.eh_entries = num_entries;
	inode->on_disk.extents.hdr.eh_max = 4;
	inode->on_disk.extents.hdr.eh_depth = depth;
	inode->on_disk.extents.hdr.eh_generation = 0;
	memcpy(inode->on_disk.extents.extent, entries,
	       num_entries * sizeof(struct ext3_extent));
}

int write_extent_mapping(struct defrag_ctx *c, struct inode *inode,
                         journal_trans_t *trans)
{
	struct obstack mempool;
	struct ext3_extent *leaves;
	struct allocation *new_metadata_blocks, *old_metadata;
	e2_blkcnt_t num_extents, num_indexes, num_blocks;
	int i, ret, depth = 0;

	/* Make sure no other transaction yanks the old metadata out before
	 * the new metadata is in place
	 */
	ret = journal_protect_alloc(trans, inode->metadata);
	if (ret)
		return ret;
	obstack_init(&mempool);
	for (i = 0; i < inode->data->extent_count; i++)
		extent_to_ext3_extent(inode,inode->data->extents + i, &mempool);
	num_extents = obstack_object_size(&mempool) / sizeof(*leaves);
	leaves = obstack_finish(&mempool);
	if (num_extents > 4) {
		ext2_ino_t inode_nr = inode->data->extents[0].inode_nr;
		num_indexes = calc_num_indexes(c, num_extents);
		num_blocks = num_indexes + num_extents / EXT_PER_BLOCK(&c->sb);
		if (num_extents % EXT_PER_BLOCK(&c->sb))
			num_blocks++;
		new_metadata_blocks = get_blocks(c, num_blocks, inode_nr, 0);
		if (new_metadata_blocks == NULL)
			return -1;
		ret = allocate(c, new_metadata_blocks, trans);
		if (ret < 0)
			return -1;
		assert(depth == 0);
		/* The new extent blocks don't need journal protection, as they
		 * are protected by the data-metadata ordering guarantee.
		 */
		ret = writeout_extents(c, leaves, new_metadata_blocks,
		                       num_extents, &depth, &mempool);
		if (ret < 0) {
			deallocate_blocks(c, new_metadata_blocks, trans);
			goto error_out;
		}
		num_extents = obstack_object_size(&mempool) / sizeof(*leaves);
		leaves = obstack_finish(&mempool);
	} else {
		new_metadata_blocks = malloc(sizeof(*inode->metadata));
		if (!new_metadata_blocks) {
			ret = -1;
			goto error_out;
		}
		new_metadata_blocks->block_count = 0;
		new_metadata_blocks->extent_count = 0;
	}
	update_inode_extents(inode, leaves, num_extents, depth);
	old_metadata = inode->metadata;
	inode->metadata = new_metadata_blocks;
	if (old_metadata->block_count != inode->metadata->block_count) {
		uint64_t blocks;
		blocks = le32toh(c->sb.s_free_blocks_count);
		blocks += le64toh((uint64_t)c->sb.s_free_blocks_hi) << 32;
		blocks += old_metadata->block_count;
		blocks -= inode->metadata->block_count;
		c->sb.s_free_blocks_hi = htole32(blocks >> 32);
		c->sb.s_free_blocks_count = htole32(blocks);
	}
	ret = write_inode(c, inode->data->extents[0].inode_nr, trans);
	if (ret < 0) {
		inode->metadata = old_metadata;
		if (new_metadata_blocks)
			deallocate_blocks(c, new_metadata_blocks, trans);
		goto error_out;
	}
	if (old_metadata) {
		rb_remove_data_alloc(c, old_metadata);
		deallocate_blocks(c, old_metadata, trans);
	}
	if (new_metadata_blocks)
		insert_data_alloc(c, new_metadata_blocks);
	ret = 0;

error_out:
	obstack_free(&mempool, NULL);
	return ret;
}

int write_extent_metadata(struct defrag_ctx *c, struct data_extent *e,
                          journal_trans_t *trans)
{
	struct inode *inode = c->inodes[e->inode_nr];

	if (inode->metadata) { /* TODO: possibly redundant check, remove? */
		return write_extent_mapping(c, inode, trans);
	} else {
		return write_direct_mapping(c, e, trans);
	}
}

int write_inode_metadata(struct defrag_ctx *c, struct inode *inode,
                         journal_trans_t *t)
{
	if (inode->metadata) {
		return write_extent_mapping(c, inode, t);
	} else {
		int i, ret = 0;
		for (i = 0; i < inode->data->extent_count && !ret; i++) {
			ret = write_direct_mapping(c, &inode->data->extents[i],
			                           t);
		}
		return ret;
	}
}

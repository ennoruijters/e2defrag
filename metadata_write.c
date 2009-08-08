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

static int is_sparse(struct data_extent *e, blk64_t lblock)
{
	struct sparse_extent *s = e->sparse;
	if (!s)
		return 0;
	while (s->start) {
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
	for (cur_logical = e->start_logical;
	     cur_logical < EXT2_IND_LBLOCK(&c->sb) && cur_block <= e->end_block;
	     cur_logical++) {
		if (!is_sparse(e, cur_logical))
			new_block = cur_block++;
		else
			new_block = 0;
		if (inode->on_disk->i_block[cur_logical] != new_block) {
			inode->on_disk->i_block[cur_logical] = new_block;
			sync_inode = 1;
		}
	}
	if (cur_block > e->end_block)
		goto out;

	/* Singly indirect blocks */
	if (cur_logical == EXT2_IND_LBLOCK(&c->sb)) {
		if (is_sparse(e, cur_logical))
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
	if (cur_block > e->end_block)
		goto out;

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
	if (cur_block > e->end_block)
		goto out;

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

out:
	if (sync_inode)
		/* Assumes the inode is completely within one page */
		return msync(PAGE_START(inode->on_disk),getpagesize(), MS_SYNC);
	return 0;
}

static void extent_to_ext3_extent(struct data_extent *_extent,
                                  struct obstack *mempool)
{
	struct data_extent extent = *_extent;
	struct sparse_extent *sparse = extent.sparse;

	while (extent.start_block <= extent.end_block) {
		struct ext3_extent new_extent;
		e2_blkcnt_t length;
		new_extent.ee_block = extent.start_logical;
		EE_BLOCK_SET(&new_extent, extent.start_block);
		length = extent.end_block - extent.start_block + 1;
		if (length > EXT_INIT_MAX_LEN)
			length = EXT_INIT_MAX_LEN;
		if (sparse && sparse->start < extent.start_logical + length)
			length = sparse->start - extent.start_logical;
		extent.start_block += length;
		extent.start_logical += length;
		if (sparse && sparse->start == extent.start_logical) {
			extent.start_logical += sparse->num_blocks;
			sparse++;
			if (!sparse->num_blocks)
				sparse = NULL;
		}
		new_extent.ee_len = length;
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
				blk64_t at_block)
{
	int ret = 0;
	struct ext3_extent_header *header;
	struct ext3_extent_idx *idx;
	if (at_block == 0) {
		header = &inode->on_disk->extents.hdr;
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
			                           EI_BLOCK(idx));
			goto out_noupdate;
		}
	}
	errno = EINVAL;
	goto out_noupdate;

out_update:
	if (at_block) {
		ret = write_block(c, header, at_block);
	} else {
		ret = msync(PAGE_START(header), getpagesize(), MS_SYNC);
	}
out_noupdate:
	if (at_block)
		free(header);
	return ret;
}

static int move_metadata_block(struct defrag_ctx *c, struct inode *inode,
                               blk64_t from, blk64_t to)
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
	ret = write_block(c, header, to);
	if (ret)
		goto out_error;
	logical_block = extents->ee_block;
	free(header);
	return update_metadata_move(c, inode, from, to, logical_block, 0);

out_error:
	free(header);
	return ret;
}

int move_metadata_extent(struct defrag_ctx *c, struct data_extent *extent,
                         struct allocation *target)
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
	rb_remove_data_extent(c, extent);
	rb_remove_data_extent(c, &target->extents[0]);
	for (i = extent->start_block; i != extent->end_block + 1; i++) {
		ret = move_metadata_block(c, inode, i, target_block);
		if (ret)
			return ret;
	}
	ret = deallocate_space(c, extent->start_block,
	                       extent->end_block - extent->start_block + 1);
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
	struct ext3_extent_header *header = malloc(EXT2_BLOCK_SIZE(&c->sb));
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
	ret = write_block(c, header, block);
	if (ret >= 0) {
		EI_LEAF_SET(extents, block);
		extents->ei_block = data->ee_block;
		extents->ei_unused = 0;
		obstack_grow(index_mempool, extents, sizeof(*extents));
	}
	if (ret >= 0)
		ret = header->eh_entries;
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

int update_inode_extents(struct inode *inode, struct ext3_extent *entries,
                         e2_blkcnt_t num_entries, int depth)
{
	assert(num_entries <= 4);
	inode->on_disk->extents.hdr.eh_magic = EXT3_EXT_MAGIC;
	inode->on_disk->extents.hdr.eh_entries = num_entries;
	inode->on_disk->extents.hdr.eh_max = 4;
	inode->on_disk->extents.hdr.eh_depth = depth;
	inode->on_disk->extents.hdr.eh_generation = 0;
	memcpy(inode->on_disk->extents.extent, entries,
	       num_entries * sizeof(struct ext3_extent));

	return msync(PAGE_START(inode->on_disk), getpagesize(), MS_SYNC);
}

void update_inode_block_count(struct defrag_ctx *c, struct inode *inode,
                              struct allocation *new)
{
	e2_blkcnt_t old_num_blocks, new_num_blocks;
	ext2_ino_t inode_nr;
	uint32_t group_nr;
	char *map;
	struct ext2_inode *on_disk;
	if (inode->metadata->block_count == new->block_count)
		return;
	assert(inode->data->extent_count); /* Otherwise why would we be writing
	                                      metadata? */
	inode_nr = inode->data->extents[0].inode_nr - 1;
	group_nr = inode_nr / EXT2_INODES_PER_GROUP(&c->sb);
	map = c->bg_maps[group_nr].map_start;
	map += (inode_nr % EXT2_INODES_PER_GROUP(&c->sb)) * EXT2_INODE_SIZE(&c->sb);
	on_disk = (struct ext2_inode *)map;
	old_num_blocks = inode->metadata->block_count;
	old_num_blocks *= EXT2_BLOCK_SIZE(&c->sb) / 512;
	new_num_blocks = new->block_count * EXT2_BLOCK_SIZE(&c->sb) / 512;
	on_disk->i_blocks -= old_num_blocks;
	on_disk->i_blocks += new_num_blocks;
	/* No sync because the i_blocks field is not very important anyway */
	return;
}

int write_extent_mapping(struct defrag_ctx *c, struct inode *inode)
{
	struct obstack mempool;
	struct ext3_extent *leaves;
	struct allocation *new_metadata_blocks;
	e2_blkcnt_t num_extents, num_indexes, num_blocks;
	int i, ret, depth = 0;

	obstack_init(&mempool);
	for (i = 0; i < inode->data->extent_count; i++)
		extent_to_ext3_extent(inode->data->extents + i, &mempool);
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
		ret = allocate(c, new_metadata_blocks);
		if (ret < 0)
			return -1;
		assert(depth == 0);
		ret = writeout_extents(c, leaves, new_metadata_blocks,
		                       num_extents, &depth, &mempool);
		if (ret < 0) {
			deallocate_blocks(c, new_metadata_blocks);
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
	fdatasync(c->fd);
	ret = update_inode_extents(inode, leaves, num_extents, depth);
	if (ret < 0) {
		if (new_metadata_blocks)
			deallocate_blocks(c, new_metadata_blocks);
		goto error_out;
	}
	update_inode_block_count(c, inode, new_metadata_blocks);
	if (inode->metadata)
		deallocate_blocks(c, inode->metadata);
	inode->metadata = new_metadata_blocks;
	ret = 0;

error_out:
	obstack_free(&mempool, NULL);
	return ret;
}

int write_extent_metadata(struct defrag_ctx *c, struct data_extent *e)
{
	struct inode *inode = c->inodes[e->inode_nr];

	if (inode->metadata) { /* TODO: possibly redundant check, remove? */
		return write_extent_mapping(c, inode);
	} else {
		return write_direct_mapping(c, e);
	}
}

int write_inode_metadata(struct defrag_ctx *c, struct inode *inode)
{
	if (inode->metadata) {
		return write_extent_mapping(c, inode);
	} else {
		int i, ret = 0;
		for (i = 0; i < inode->data->extent_count && !ret; i++)
			ret = write_direct_mapping(c, &inode->data->extents[i]);
		return ret;
	}
}

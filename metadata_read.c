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

#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <obstack.h>
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include <malloc.h>
#include "e2defrag.h"
#include "extree.h"

const __u32 KNOWN_INODE_FLAGS_MASK = 0x000FFFFFU;
#define EXT2_SECTORS_PER_BLOCK(sb) (EXT2_BLOCK_SIZE(sb) / 512)

#define EE_BLOCK(extent) (((blk64_t)((extent)->ee_start_hi) << 16) + ((extent)->ee_start))
#define EI_BLOCK(extent) (((blk64_t)((extent)->ei_leaf_hi) << 16) + ((extent)->ei_leaf))

struct tmp_sparse {
	struct sparse_extent s;
	struct tmp_sparse *next;
};

struct tmp_extent {
	struct data_extent e;
	struct tmp_sparse **last_sparse;
	struct tmp_extent *next;
};

static void add_sparse(struct tmp_sparse **last_sparse, blk64_t first_block,
                       e2_blkcnt_t nblk, struct obstack *mempool)
{
	struct tmp_sparse *s = *last_sparse;
	if (s->s.start == 0) {
		s->s.num_blocks = nblk;
		s->s.start = first_block;
	} else if (first_block == s->s.start + s->s.num_blocks) {
		s->s.num_blocks += nblk;
	} else {
		s->next = obstack_alloc(mempool, sizeof(struct tmp_sparse));
		s->next->next = NULL;
		s->next->s.num_blocks = nblk;
		s->next->s.start = first_block;
		*last_sparse = s->next;
	}
}

static int do_blocks(struct tmp_extent *first_extent,
                     struct tmp_extent **last_extent,
                     struct obstack *mempool,
                     blk64_t block, blk64_t logical_block, e2_blkcnt_t nblk,
                     int uninit)
{
	struct tmp_extent *le = *last_extent;
	if (block == 0 && *last_extent == NULL) {
		return 0;
	} else if (*last_extent == NULL) {
		*last_extent = first_extent;
		le = *last_extent;
		le->e.start_block = block;
		le->e.end_block = block + nblk - 1;
		le->e.start_logical = logical_block;
		le->e.uninit = uninit;
		le->next = NULL;
		return 1;
	} else if (block != 0) {
		if (block != le->e.end_block + 1 || uninit != le->e.uninit) {
			le->next = obstack_alloc(mempool,
			                         sizeof(struct tmp_extent));
			*last_extent = le->next;
			le = *last_extent;
			le->next = NULL;
			le->e.start_block = block;
			le->e.start_logical = logical_block;
			le->e.uninit = uninit;
		}
		le->e.end_block = block + nblk - 1;
		return 1;
	} else { /* blocks[i] == 0 */
		add_sparse(first_extent->last_sparse, logical_block, 1,mempool);
		return 0;
	}
}

static int do_ind_block(struct defrag_ctx *c, struct tmp_extent *first_extent,
                       struct tmp_extent **last_extent, struct obstack *mempool,
                       blk64_t block, blk64_t logical_block, __u32 *nblocks)
{
	int count = 0, i;
	__u32 ind[EXT2_ADDR_PER_BLOCK(&c->sb)];
	if (block) {
		i = read_block(c, ind, block);
		if (i) {
			obstack_free(mempool, NULL);
			return -1;
		}
		for (i = 0;
		     i < EXT2_ADDR_PER_BLOCK(&c->sb) && *nblocks;
		     i++, logical_block++) {
			*nblocks -= do_blocks(first_extent, last_extent,
			                      mempool, ind[i], logical_block,1,
			                      0);
			count++;
		}
		return count;
	} else {
		int numblocks = EXT2_ADDR_PER_BLOCK(&c->sb);
		add_sparse(first_extent->last_sparse, logical_block, numblocks,
		           mempool);
		return numblocks;
	}
}

static long do_dind_block(struct defrag_ctx *c, struct tmp_extent *first_extent,
                       struct tmp_extent **last_extent, struct obstack *mempool,
                       blk64_t block, blk64_t logical_block, __u32 *nblocks)
{
	blk64_t old_logical_block = logical_block;
	int i;
	__u32 ind[EXT2_ADDR_PER_BLOCK(&c->sb)];
	if (block) {
		i = read_block(c, ind, block);
		if (i) {
			obstack_free(mempool, NULL);
			return -1;
		}
		for (i = 0; i < EXT2_ADDR_PER_BLOCK(&c->sb) && *nblocks; i++) {
			int tmp;
			*nblocks -= do_blocks(first_extent, last_extent,
			                      mempool, ind[i], logical_block,1,
			                      0);
			logical_block++;
			tmp = do_ind_block(c, first_extent, last_extent,
			                   mempool, ind[i], logical_block,
			                   nblocks);
			if (tmp >= 0) {
				logical_block += tmp;
			} else {
				obstack_free(mempool, NULL);
				return tmp;
			}
		}
	} else {
		e2_blkcnt_t numblocks = EXT2_ADDR_PER_BLOCK(&c->sb);
		numblocks = numblocks + (numblocks * numblocks);
		/* n indirect blocks, plus n*n data blocks */
		add_sparse(first_extent->last_sparse, logical_block, numblocks,
		           mempool);
		return numblocks;
	}
	return logical_block - old_logical_block;
}

static long do_tind_block(struct defrag_ctx *c, struct tmp_extent *first_extent,
                       struct tmp_extent **last_extent, struct obstack *mempool,
                       blk64_t block, blk64_t logical_block, __u32 *nblocks)
{
	blk64_t old_logical_block = logical_block;
	int i;
	__u32 ind[EXT2_ADDR_PER_BLOCK(&c->sb)];
	if (block) {
		i = read_block(c, ind, block);
		if (i) {
			obstack_free(mempool, NULL);
			return -1;
		}
		for (i = 0; i < EXT2_ADDR_PER_BLOCK(&c->sb) && *nblocks; i++) {
			long tmp;
			*nblocks -= do_blocks(first_extent, last_extent,
			                      mempool, ind[i], logical_block,1,
			                      0);
			logical_block++;
			tmp = do_dind_block(c, first_extent, last_extent,
			                    mempool, ind[i], logical_block,
			                    nblocks);
			if (tmp >= 0) {
				logical_block += tmp;
			} else {
				obstack_free(mempool, NULL);
				return tmp;
			}
		}
	} else {
		e2_blkcnt_t numblocks = EXT2_ADDR_PER_BLOCK(&c->sb);
		numblocks = numblocks
		            + (numblocks * numblocks)
			    + (numblocks * numblocks * numblocks);
		/* n doubly indirect blocks, plus n*n indirect blocks,
		 * plus n^3 data blocks */
		add_sparse(first_extent->last_sparse, logical_block, numblocks,
		           mempool);
		return numblocks;
	}
	return logical_block - old_logical_block;
}

static int set_inode_sparse(struct inode *inode, struct tmp_sparse *first)
{
	e2_blkcnt_t nr_sparse = 0, i;
	struct tmp_sparse *current = first;

	if (current->s.num_blocks == 0)
		current = NULL;
	while (current) {
		nr_sparse++;
		current = current->next;
	}
	inode->num_sparse = nr_sparse;
	if (nr_sparse == 0) {
		inode->sparse = NULL;
		return 0;
	}
	inode->sparse = malloc(sizeof(struct sparse_extent) * nr_sparse);
	if (!inode->sparse)
		return -1;
	current = first;
	for (i = 0; i < nr_sparse; i++, current = current->next)
		inode->sparse[i] = current->s;
	return 0;
}

static struct inode *make_inode_extents(struct defrag_ctx *c,
                                        struct tmp_extent *first_extent,
					struct tmp_sparse *first_sparse,
					ext2_ino_t inode_nr)
{
	struct tmp_extent *tmp = first_extent;
	struct inode *ret;
	e2_blkcnt_t i = 0;
	int retval;

	while (tmp != NULL) {
		i += 1;
		tmp = tmp->next;
	}
	ret = malloc(sizeof(struct inode));
	if (!ret)
		return NULL;
	ret->data = malloc(sizeof(struct allocation) +
	                   sizeof(struct data_extent) * i);
	if (!ret->data) {
		free(ret);
		return NULL;
	}
	if (first_sparse) {
		retval = set_inode_sparse(ret, first_sparse);
		if (retval) {
			free(ret->data);
			free(ret);
			return NULL;
		}
	}
	ret->data->block_count = 0;
	ret->data->extent_count = i;
	ret->metadata = NULL;
	for (i = 0, tmp = first_extent; tmp != NULL; i++, tmp = tmp->next) {
		ret->data->extents[i].start_block = tmp->e.start_block;
		ret->data->extents[i].end_block = tmp->e.end_block;
		ret->data->extents[i].inode_nr = inode_nr;
		ret->data->block_count +=
		                      tmp->e.end_block - tmp->e.start_block + 1;
		ret->data->extents[i].start_logical = tmp->e.start_logical;
		ret->data->extents[i].uninit = tmp->e.uninit;
		if (tmp->next != NULL) {
			blk64_t next_logical;
			next_logical = tmp->next->e.start_logical;
		}
		insert_data_extent(c, ret->data->extents + i);
	}
	return ret;
}

static int read_extent_leaf(struct tmp_extent *first_extent,
                            struct tmp_extent **last_extent,
                            struct obstack *mempool,
			    struct ext3_extent_header *header)
{
	struct ext3_extent *extents = (struct ext3_extent *)(header + 1);
	int i, ret;

	if (header->eh_magic != EXT3_EXT_MAGIC) {
		printf("Inode has unknown type of extents, ignoring.");
		return 0;
	}
	for (i = 0; i < header->eh_entries; i++) {
		uint16_t length = extents[i].ee_len;
		char uninit = 0;
		if (length > EXT_INIT_MAX_LEN) {
			uninit = 1;
			length -= EXT_INIT_MAX_LEN;
		}
		ret = do_blocks(first_extent, last_extent, mempool,
		                EE_BLOCK(&extents[i]), extents[i].ee_block,
		                length, uninit);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int read_extent_index(struct defrag_ctx *c,
                             struct tmp_extent *first_extent,
                             struct tmp_extent **last_extent,
                             struct obstack *mempool,
			     struct ext3_extent_header *header,
                             e2_blkcnt_t *metadata_only)
{
	struct ext3_extent_idx *extents = (struct ext3_extent_idx *)(header+1);
	int i, ret;
	if (header->eh_magic != EXT3_EXT_MAGIC) {
		printf("Inode has unknown type of extents, ignoring.");
		return 0;
	}
	for (i = 0; i < header->eh_entries; i++) {
		unsigned char buffer[EXT2_BLOCK_SIZE(&c->sb)];
		struct ext3_extent_header *new_header = (void *)buffer;
		if (metadata_only) {
			ret = do_blocks(first_extent, last_extent, mempool,
			                EI_BLOCK(&extents[i]),
			                (*metadata_only)++, 1, 0);
			if (ret < 0)
				return ret;
		}
		ret = read_block(c, buffer, EI_BLOCK(&extents[i]));
		if (ret < 0)
			return ret;
		if (new_header->eh_depth == 0 && !metadata_only)
			ret = read_extent_leaf(first_extent, last_extent,
			                       mempool, new_header);
		else if (new_header->eh_depth > 0) {
			ret = read_extent_index(c, first_extent, last_extent,
			                        mempool, new_header,
			                        metadata_only);
		}
		if (ret < 0)
			return ret;
	}
	return 0;
}
static int gen_inode_sparse(struct inode *inode)
{
	const struct data_extent tmp_extent = {
		.start_block = 0,
		.end_block = 0,
		.start_logical = -1
	};
	const struct data_extent *extent, *last_extent;
	struct sparse_extent *sparse;
	e2_blkcnt_t extent_nr = 0;
	int num_sparse = 0;

	last_extent = &tmp_extent;
	extent = inode->data->extents;
	while (extent_nr < inode->data->extent_count) {
		blk64_t last_end_logical;
		last_end_logical = last_extent->start_logical;
		last_end_logical += last_extent->end_block
		                                     - last_extent->start_block;
		if (extent->start_logical != last_end_logical + 1)
			num_sparse += 1;
		last_extent = extent;
		extent++;
		extent_nr++;
	}
	inode->num_sparse = num_sparse;
	if (!num_sparse)
		return 0;
	inode->sparse = malloc(num_sparse * sizeof(struct sparse_extent));
	if (!inode->sparse)
		return -1;
	sparse = inode->sparse;
	extent_nr = 0;
	extent = inode->data->extents;
	last_extent = &tmp_extent;
	while (extent_nr < inode->data->extent_count) {
		blk64_t last_end_logical;
		last_end_logical = last_extent->start_logical;
		last_end_logical += last_extent->end_block
		                                     - last_extent->start_block;
		if (extent->start_logical != last_end_logical + 1) {

			sparse->start = last_end_logical + 1;
			sparse->num_blocks = extent->start_logical
			                                        - sparse->start;
			sparse++;
		}
		last_extent = extent;
		extent++;
		extent_nr++;
	}
	return 0;
}

static struct inode *read_inode_extents(struct defrag_ctx *c,
                                        ext2_ino_t inode_nr,
                                        struct ext2_inode *inode)
{
	struct ext3_extent_header *header;
	struct tmp_extent first_extent = {
		.e = {0, 0, 0, 0},
		.last_sparse = NULL, /* Not used while reading */
		.next = NULL
	};
	struct tmp_extent *last_extent = NULL;
	struct inode *ret;
	struct obstack mempool;
	e2_blkcnt_t num_metadata_blocks = 0;
	header = (struct ext3_extent_header *) inode->i_block;

	obstack_init(&mempool);

	if (header->eh_depth == 0) {
		int tmp;
		tmp = read_extent_leaf(&first_extent, &last_extent,
		                       &mempool, header);
		if (tmp < 0)
			goto out_error;
		ret = make_inode_extents(c, &first_extent, NULL, inode_nr);
		if (ret) {
			ret->on_disk = (union on_disk_block *)inode->i_block;
			ret->metadata = malloc(sizeof(*ret->metadata));
			if (!ret->metadata) {
				free(ret);
				goto out_error;
			}
			ret->metadata->block_count = 0;
			ret->metadata->extent_count = 0;
		}
	} else {
		int tmp;
		tmp = read_extent_index(c, &first_extent, &last_extent,
		                        &mempool, header, NULL);
		if (tmp < 0)
			goto out_error;
		ret = make_inode_extents(c, &first_extent, NULL, inode_nr);
		if (ret) {
			e2_blkcnt_t num_extents = 0, i;
			ret->on_disk = (union on_disk_block *)inode->i_block;
			first_extent.e.start_block = 0;
			first_extent.e.end_block = 0;
			first_extent.e.start_logical = 0;
			first_extent.next = NULL;
			first_extent.last_sparse = NULL;
			last_extent = NULL;
			tmp = read_extent_index(c, &first_extent, &last_extent,
			                        &mempool, header,
			                        &num_metadata_blocks);
			if (tmp < 0) {
				free(ret);
				goto out_error;
			}
			last_extent = &first_extent;
			while (last_extent) {
				num_extents++;
				last_extent = last_extent->next;
			}
			ret->metadata = malloc(sizeof(*ret->metadata)
			                       + num_extents
			                          * sizeof(struct data_extent));
			if (!ret->metadata) {
				free(ret);
				goto out_error;
			}
			ret->metadata->block_count = num_metadata_blocks;
			ret->metadata->extent_count = num_extents;
			last_extent = &first_extent;
			for (i = 0; i < num_extents; i++) {
				ret->metadata->extents[i] = last_extent->e;
				ret->metadata->extents[i].inode_nr = inode_nr;
				last_extent = last_extent->next;
				insert_data_extent(c,ret->metadata->extents +i);
			}
		}
	}
	if (gen_inode_sparse(ret) < 0) {
		free(ret->data);
		free(ret->metadata);
		free(ret);
		goto out_error;
	}

	obstack_free(&mempool, NULL);
	return ret;

out_error:
	obstack_free(&mempool, NULL);
	return NULL;
}

static struct inode *read_inode_blocks(struct defrag_ctx *c,
                                       ext2_ino_t inode_nr,
                                       struct ext2_inode *inode)
{
	struct tmp_sparse first_sparse = {
		.s = {0, 0},
		.next = NULL,
	};
	struct tmp_sparse *last_sparse = &first_sparse;
	struct tmp_extent first_extent = {
		.last_sparse = &last_sparse,
		.next = NULL
	};
	struct tmp_extent *last_extent = NULL;
	struct inode *ret;
	struct obstack mempool;
	__u32 logical_block = 0;
	__u32 nblocks = inode->i_blocks / EXT2_SECTORS_PER_BLOCK(&c->sb);
	__u32 *blocks = inode->i_block;
	int i;

	obstack_init(&mempool);

	for (i = 0; i <= EXT2_NDIR_BLOCKS && nblocks; i++, logical_block++) {
		nblocks -= do_blocks(&first_extent, &last_extent, &mempool,
		                     blocks[i], logical_block, 1, 0);
	}
	if (nblocks) {
		long tmp;
		tmp = do_ind_block(c, &first_extent, &last_extent,
		                   &mempool, blocks[EXT2_IND_BLOCK],
		                   logical_block, &nblocks);
		if (tmp >= 0)
			logical_block += tmp;
		else
			return NULL;
	}
	if (nblocks) {
		long ret;

		ret =  do_blocks(&first_extent, &last_extent, &mempool,
		                 blocks[EXT2_DIND_BLOCK], logical_block, 1, 0);
		if (ret >= 0) {
			nblocks -= ret;
			logical_block += ret;
		} else {
			return NULL;
		}
	}
	if (nblocks) {
		long tmp;
		tmp = do_dind_block(c, &first_extent, &last_extent,
		                    &mempool, blocks[EXT2_DIND_BLOCK],
		                    logical_block, &nblocks);
		if (tmp >= 0)
			logical_block += tmp;
		else
			return NULL;
	}
	if (nblocks) {
		long ret;

		ret =  do_blocks(&first_extent, &last_extent, &mempool,
		                 blocks[EXT2_TIND_BLOCK], logical_block, 1, 0);
		if (ret >= 0) {
			nblocks -= ret;
			logical_block += ret;
		} else {
			return NULL;
		}
	}
	if (nblocks) {
		long tmp;
		tmp = do_tind_block(c, &first_extent, &last_extent,
		                    &mempool, blocks[EXT2_TIND_BLOCK],
		                    logical_block, &nblocks);
		if (tmp >= 0)
			logical_block += tmp;
		else
			return NULL;
	}
	ret = make_inode_extents(c, &first_extent, &first_sparse, inode_nr);
	if (ret)
		ret->on_disk = (union on_disk_block *)inode->i_block;
	obstack_free(&mempool, NULL);
	return ret;
}

long parse_inode(struct defrag_ctx *c, ext2_ino_t inode_nr,
                 struct ext2_inode *inode)
{
	if (inode->i_blocks == 0) {
		c->inodes[inode_nr] = malloc(sizeof(struct inode));
		if (!c->inodes[inode_nr])
			return -1;
		c->inodes[inode_nr]->data = malloc(sizeof(struct allocation));
		if (!c->inodes[inode_nr]->data) {
			free(c->inodes[inode_nr]);
			return -1;
		}
		c->inodes[inode_nr]->data->block_count = 0;
		c->inodes[inode_nr]->data->extent_count = 0;
		c->inodes[inode_nr]->on_disk =
			                  (union on_disk_block *)inode->i_block;
		c->inodes[inode_nr]->metadata = NULL;
		c->inodes[inode_nr]->num_sparse = 0;
		return 0;
	}
	if (inode_nr < EXT2_FIRST_INO(&c->sb)) {
		if (inode_nr != EXT2_ROOT_INO) {
			c->inodes[inode_nr] = NULL;
			return 0;
		}
	}
	if (inode->i_flags - (inode->i_flags & KNOWN_INODE_FLAGS_MASK)) {
		printf("Inode %u has unknown flags %x. Ignoring the inode\n",
		       inode_nr,
		       inode->i_flags & ~KNOWN_INODE_FLAGS_MASK);
		c->inodes[inode_nr] = NULL;
		return 0;
	}
	if (inode->i_flags & EXT4_EXTENTS_FL) {
		c->inodes[inode_nr] = read_inode_extents(c, inode_nr, inode);
		if (c->inodes[inode_nr] == NULL)
			return -1;
		else
			return 0;
	} else {
		e2_blkcnt_t blocks;
		blocks = inode->i_blocks / EXT2_SECTORS_PER_BLOCK(&c->sb);
		c->inodes[inode_nr] = read_inode_blocks(c, inode_nr, inode);
		if (c->inodes[inode_nr] != NULL)
			return 0;
		else
			return -1;
	}
}

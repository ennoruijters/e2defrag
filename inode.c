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

struct tmp_sparse {
	struct sparse_extent s;
	struct tmp_sparse *next;
};

struct tmp_extent {
	struct data_extent e;
	struct tmp_sparse *s;
	struct tmp_sparse *last_sparse;
	struct tmp_extent *next;
};

static struct sparse_extent *merge_sparse(struct sparse_extent *s1,
                                          struct sparse_extent *s2,
                                          blk64_t gap_start,
                                          e2_blkcnt_t gap_length,
                                          e2_blkcnt_t e1_blocks)
{
	struct sparse_extent *tmp = s1;
	int cnt = 0;

	errno = 0;

	if (gap_length == 0 && cnt == 0) {
		return s1;
	} else if (gap_length == 0 && s1 == NULL) {
		tmp = s2;
		while (tmp && tmp->num_blocks) {
			tmp->start += e1_blocks;
			tmp++;
		}
		return s2;
	}

	while (tmp && tmp->num_blocks) {
		cnt++;
		tmp++;
	}
	tmp = s2;
	while (tmp && tmp->num_blocks) {
		cnt++;
		tmp++;
	}
	if (gap_length)
		cnt++;
	if (cnt) {
		struct sparse_extent *new_sparse;
		size_t num_bytes = (cnt + 1) * sizeof(*new_sparse);
		new_sparse = realloc(s1, num_bytes);
		if (!new_sparse)
			return NULL; /* errno should be ENOMEM */
		s1 = new_sparse;
		while (s1->num_blocks)
			s1++;
		if (gap_length) {
			s1->start = gap_start;
			s1->num_blocks = gap_length;
			s1++;
		}
		tmp = s2;
		while (s2 && s2->num_blocks != 0) {
			s1->start = s2->start + e1_blocks;
			s1->num_blocks = s2->num_blocks;
			s1++;
			s2++;
		}
		free(tmp);
		s1->start = s1->num_blocks = 0;
		return new_sparse;
	} else {
		return NULL;
	}
}


static int merge_extents(struct inode *inode,
                         struct data_extent *extent1,
                         struct data_extent *extent2)
{
	struct sparse_extent *sparse = extent1->sparse;
	int pos, num;
	e2_blkcnt_t logical_gap;
	blk64_t e1_logical_blocks = extent1->end_block - extent1->start_block+1;
	blk64_t e1_logical_end;

	while (sparse && sparse->num_blocks != 0) {
		e1_logical_blocks += sparse->num_blocks;
		sparse++;
	}
	e1_logical_end = extent1->start_logical + e1_logical_blocks - 1;
	logical_gap = (extent2->start_logical - 1) - e1_logical_end;

	sparse = merge_sparse(extent1->sparse, extent2->sparse,
	                      e1_logical_end + 1, logical_gap,
			      e1_logical_blocks);
	if (sparse == NULL && errno != 0)
		return -1;
	extent1->sparse = sparse;
	extent1->end_block = extent2->end_block;

	pos = extent2 - inode->extents;
	num = inode->extent_count - pos - 1;
	memmove(extent2, extent2 + 1, num * sizeof(struct data_extent));
	inode->extent_count--;
	return 0;
}

static void inode_remove_from_trees(struct defrag_ctx *c, struct inode *inode)
{
	int i;
	for (i = 0; i < inode->extent_count; i++)
		rb_remove_data_extent(c, &inode->extents[i]);
}

int try_extent_merge(struct defrag_ctx *c,
                     struct inode *inode,
                     struct data_extent *extent)
{
	int oldcount = -1, count = 0, ret;
	char removed_from_tree = 0;

	while (oldcount != count) {
		int pos = extent - inode->extents;
		struct data_extent *prev = inode->extents + pos - 1;
		struct data_extent *next = inode->extents + pos + 1;
		oldcount = count;
		if (pos > 0 && prev->end_block == extent->start_block - 1) {
			if (!removed_from_tree) {
				inode_remove_from_trees(c, inode);
				removed_from_tree = 1;
			}
			ret = merge_extents(inode, prev, extent);
			if (ret < 0) {
				insert_data_extent(c, prev);
				break;
			}
			count++;
			extent = prev;
			pos -= 1;
		}

		if ((pos + 1) < inode->extent_count
		    && extent->end_block == next->start_block - 1) {
			if (!removed_from_tree) {
				inode_remove_from_trees(c, inode);
				removed_from_tree = 1;
			}
			ret = merge_extents(inode, extent, next);
			if (ret < 0)
				break;
			count++;
		}
	}

	if (count) {
		struct inode *new_inode;
		ext2_ino_t inode_nr = extent->inode_nr;
		size_t num_bytes = sizeof(*inode);
		num_bytes += inode->extent_count * sizeof(*inode->extents);

		new_inode = realloc(inode, num_bytes);
		/* merge_extents has already consilidated everything to the
		 * beginning of the inode object */
		if (new_inode) {
			c->inodes[inode_nr] = new_inode;
			inode = new_inode;
		}
	}

	if (removed_from_tree) {
		int i;
		for (i = 0; i < inode->extent_count; i++)
			insert_data_extent(c, &inode->extents[i]);
	}
	return count;
}

void add_sparse(struct tmp_extent *extent, blk64_t first_block,
               e2_blkcnt_t nblk, struct obstack *mempool)
{
	struct tmp_sparse *s = extent->last_sparse;
	if (!s) {
		s = obstack_alloc(mempool, sizeof(struct tmp_sparse));
		extent->s = extent->last_sparse = s;
		s->next = NULL;
		s->s.num_blocks = nblk;
		s->s.start = first_block;
	} else if (first_block == s->s.start + s->s.num_blocks) {
		s->s.num_blocks += nblk;
	} else {
		s->next = obstack_alloc(mempool, sizeof(struct tmp_sparse));
		s->next->next = NULL;
		s->next->s.num_blocks = nblk;
		s->next->s.start = first_block;
		extent->last_sparse = s->next;
	}
}

int do_block(struct tmp_extent *first_extent, struct tmp_extent **last_extent,
             struct obstack *mempool, blk64_t block, blk64_t logical_block)
{
	struct tmp_extent *le = *last_extent;
	if (block == 0 && *last_extent == NULL) {
		return 0;
	} else if (*last_extent == NULL) {
		*last_extent = first_extent;
		le = *last_extent;
		le->e.start_block = block;
		le->e.end_block = block;
		le->e.start_logical = logical_block;
		le->next = NULL;
		le->s = le->last_sparse = NULL;
		return 1;
	} else if (block != 0) {
		if (block != le->e.end_block + 1) {
			le->next = obstack_alloc(mempool,
			                         sizeof(struct tmp_extent));
			*last_extent = le->next;
			le = *last_extent;
			le->next = NULL;
			le->e.start_block = block;
			le->e.start_logical = logical_block;
			le->s = NULL;
			le->last_sparse = NULL;
		}
		le->e.end_block = block;
		return 1;
	} else { /* blocks[i] == 0 */
		add_sparse(le, block, 1, mempool);
		return 0;
	}
}

int do_ind_block(struct defrag_ctx *c, struct tmp_extent *first_extent,
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
			*nblocks -= do_block(first_extent, last_extent,
			                     mempool, ind[i],
			                     logical_block);
			count++;
		}
		return count;
	} else {
		int numblocks = EXT2_ADDR_PER_BLOCK(&c->sb);
		add_sparse(*last_extent, logical_block, numblocks, mempool);
		return numblocks;
	}
}

long do_dind_block(struct defrag_ctx *c, struct tmp_extent *first_extent,
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
			*nblocks -= do_block(first_extent, last_extent,
			                     mempool, ind[i],
			                     logical_block);
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
		add_sparse(*last_extent, logical_block, numblocks, mempool);
		return numblocks;
	}
	return logical_block - old_logical_block;
}

long do_tind_block(struct defrag_ctx *c, struct tmp_extent *first_extent,
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
			*nblocks -= do_block(first_extent, last_extent,
			                     mempool, ind[i],
			                     logical_block);
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
		add_sparse(*last_extent, logical_block, numblocks, mempool);
		return numblocks;
	}
	return logical_block - old_logical_block;
}

static struct sparse_extent *get_sparse_array(struct tmp_extent *extent,
                                              blk64_t next_logical)
{
	e2_blkcnt_t nr_sparse = 0, i;
	struct tmp_sparse *current = extent->s;
	struct sparse_extent *ret;

	while (current && current->next) {
		nr_sparse++;
		current = current->next;
	}
	if (current) {
		if (current->s.start + current->s.num_blocks != next_logical)
			nr_sparse++;
	}
	if (nr_sparse == 0)
		return NULL;
	ret = malloc(sizeof(struct sparse_extent) * (nr_sparse + 1));
	if (!ret)
		return NULL;
	current = extent->s;
	for (i = 0; i < nr_sparse; i++, current = current->next)
		ret[i] = current->s;
	ret[i].start = 0;
	ret[i].num_blocks = 0;
	return ret;
}

static struct inode *make_inode_extents(struct defrag_ctx *c,
                                        struct tmp_extent *extent,
					ext2_ino_t inode_nr,
                                        blk64_t last_logical_block)
{
	struct tmp_extent *tmp = extent;
	struct inode *ret;
	e2_blkcnt_t i = 0;

	while (tmp != NULL) {
		i += 1;
		tmp = tmp->next;
	}
	ret = malloc(sizeof(struct inode) + sizeof(struct data_extent) * i);
	if (!ret)
		return NULL;
	ret->block_count = 0;
	ret->extent_count = i;
	for (i = 0, tmp = extent; tmp != NULL; i++, tmp = tmp->next) {
		blk64_t next_logical;
		ret->extents[i].start_block = tmp->e.start_block;
		ret->extents[i].end_block = tmp->e.end_block;
		ret->extents[i].inode_nr = inode_nr;
		ret->block_count += tmp->e.end_block - tmp->e.start_block + 1;
		ret->extents[i].start_logical = tmp->e.start_logical;
		if (tmp == NULL || tmp->next == NULL)
			next_logical = last_logical_block;
		else
			next_logical = tmp->next->e.start_logical;
		ret->extents[i].sparse = get_sparse_array(tmp, next_logical);
		insert_data_extent(c, ret->extents + i);
	}
	return ret;
}

struct inode *read_inode_blocks(struct defrag_ctx *c, ext2_ino_t inode_nr,
                                struct ext2_inode *inode)
{
	struct tmp_extent first_extent = {
		.s = NULL,
		.last_sparse = NULL,
		.next = NULL
	};
	struct tmp_extent *last_extent = NULL;
	struct inode *ret;
	struct obstack mempool;
	__u32 logical_block = 0;
	__u32 nblocks = inode->i_blocks;
	__u32 *blocks = inode->i_block;
	int i;

	if (nblocks == 0) {
		ret = malloc(sizeof(struct inode));
		if (ret != NULL) {
			ret->block_count = nblocks;
			ret->extent_count = 0;
			ret->on_disk = inode;
		}
		return ret;
	}
	obstack_init(&mempool);

	for (i = 0; i <= EXT2_NDIR_BLOCKS && nblocks; i++, logical_block++) {
		nblocks -= do_block(&first_extent, &last_extent, &mempool,
		                    blocks[i], logical_block);
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
		long tmp;
		tmp = do_tind_block(c, &first_extent, &last_extent,
		                    &mempool, blocks[EXT2_TIND_BLOCK],
		                    logical_block, &nblocks);
		if (tmp >= 0)
			logical_block += tmp;
		else
			return NULL;
	}
	ret = make_inode_extents(c, &first_extent, inode_nr, logical_block);
	if (ret)
		ret->on_disk = inode;
	obstack_free(&mempool, NULL);
	return ret;
}

long parse_inode(struct defrag_ctx *c, ext2_ino_t inode_nr,
                 struct ext2_inode *inode)
{
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
		printf("Inode %u uses extents. I don't know how to handle "
		       "those, so I'm ignoring it.\n", inode_nr);
		c->inodes[inode_nr] = NULL;
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

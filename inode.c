#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include "e2defrag.h"
#include "extree.h"

#define EXT2_SECTORS_PER_BLOCK(sb) (EXT2_BLOCK_SIZE(sb) / 512)

static void inode_remove_from_trees(struct defrag_ctx *c, struct inode *inode)
{
	int i;
	for (i = 0; i < inode->extent_count; i++)
		rb_remove_data_extent(c, &inode->extents[i]);
}


static struct sparse_extent *merge_sparse(struct sparse_extent *s1,
                                          struct sparse_extent *s2,
                                          blk64_t gap_start,
                                          e2_blkcnt_t gap_length,
                                          e2_blkcnt_t e1_blocks)
{
	struct sparse_extent *tmp = s1;
	int cnt = 0;

	errno = 0;

	if (gap_length == 0 && s2 == NULL) {
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

blk64_t get_logical_block(struct inode *inode, blk64_t phys_block)
{
	struct data_extent *d = inode->extents;
	blk64_t ret;
	while ((d - inode->extents) < inode->extent_count
	       && (d->end_block < phys_block
	           || d->start_block > phys_block)) {
		d++;
	}
	ret = d->start_logical + phys_block - d->start_block;
	if (d->sparse) {
		struct sparse_extent *s = d->sparse;
		while (s->num_blocks && s->start < ret) {
			ret += s->num_blocks;
			s++;
		}
	}
	return ret;
}

/* Note that this function does not free the old sparse list */
int split_sparse(struct data_extent *extent1,
                 struct data_extent *extent2,
                 struct sparse_extent *old_sparse)
{
	if (old_sparse == NULL) {
		extent1->sparse = NULL;
		extent2->sparse = NULL;
	} else {
		struct sparse_extent *s = old_sparse;
		int cnt1 = 0, cnt2 = 0;
		char inbetween = 0;
		while (s->num_blocks) {
			if (s->start < extent2->start_logical)
				cnt1++;
			else if (s->start > extent2->start_logical)
				cnt2++;
			else
				inbetween = 1;
			s++;
		}
		if (!cnt1 && !inbetween) {
			extent2->sparse = old_sparse;
			extent1->sparse = NULL;
		} else if (!cnt2) {
			extent2->sparse = NULL;
			if (inbetween)
				(s-1)->num_blocks = 0;
			extent1->sparse = old_sparse;
		} else {
			struct sparse_extent *s1, *s2;
			s1 = malloc((cnt1 + 1) * sizeof(*s1));
			if (!s1)
				return -1;
			s2 = malloc((cnt2 + 1) * sizeof(*s2));
			if (!s1) {
				s1 = NULL;
				return -1;
			}
			s = old_sparse;
			extent1->sparse = s1;
			extent2->sparse = s2;
			while (s->start < extent2->start_logical) {
				*s1 = *s;
				s++;
				s1++;
			}
			*s1 = (struct sparse_extent) {0, 0};
			if (s->start == extent2->start_logical) {
				extent2->start_logical += s->num_blocks;
				s++;
			}
			while (s->num_blocks) {
				*s2 = *s;
				s++;
				s2++;
			}
			*s2 = (struct sparse_extent) {0, 0};
		}
	}
	return 0;
}

/* Note that this function may realloc inode (and therefore extent) */
int split_extent(struct defrag_ctx *c,
                 struct inode *inode,
                 struct data_extent *extent,
                 blk64_t new_end_block)
{
	struct sparse_extent *sparse;
	blk64_t e2_logical_start;
	size_t nbytes;
	ext2_ino_t inode_nr = extent->inode_nr;
	int extent_nr, ret;
	e2_logical_start = get_logical_block(inode, new_end_block + 1);
	inode_remove_from_trees(c, inode);
	extent_nr = extent - inode->extents;
	inode->extent_count += 1;
	nbytes = sizeof(struct inode);
	nbytes += inode->extent_count * sizeof(struct data_extent);
	inode = realloc(inode, nbytes);
	if (!inode)
		return -1;
	c->inodes[inode_nr] = inode;
	memmove(&inode->extents[extent_nr + 1], &inode->extents[extent_nr],
	        (inode->extent_count - extent_nr - 1) * sizeof(struct data_extent));
	inode->extents[extent_nr + 1].start_block = new_end_block + 1;
	inode->extents[extent_nr + 1].start_logical = e2_logical_start;
	inode->extents[extent_nr].end_block = new_end_block;
	sparse = inode->extents[extent_nr].sparse;
	ret = split_sparse(inode->extents + extent_nr,
	                   inode->extents + extent_nr + 1,
	                   sparse);
	if (ret < 0) {
		inode->extents[extent_nr].end_block
		                      = inode->extents[extent_nr + 1].end_block;
		inode->extent_count--;
		memmove(&inode->extents[extent_nr + 1],
		        &inode->extents[extent_nr + 2],
		        inode->extent_count - extent_nr - 1);
		inode = realloc(inode, nbytes - sizeof(struct data_extent));
		if (inode)
			c->inodes[inode_nr] = inode;
	} else {
		ret = 0;
		free(sparse);
	}
	for (extent_nr = 0; extent_nr < inode->extent_count; extent_nr++) {
		insert_data_extent(c, &inode->extents[extent_nr]);
	}
	return ret;
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

int try_extent_merge(struct defrag_ctx *c,
                     struct inode *inode,
                     struct data_extent *extent)
{
	int oldcount = -1, count = 0, downcount = 0, ret;
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
			downcount++;
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
	return downcount;
}

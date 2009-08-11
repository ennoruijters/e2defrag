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
	for (i = 0; i < inode->data->extent_count; i++)
		rb_remove_data_extent(c, &inode->data->extents[i]);
}

int is_metadata(struct defrag_ctx *c, struct data_extent *extent)
{
	struct inode *inode;
	int i;
	inode = c->inodes[extent->inode_nr];

	if (inode->metadata == NULL)
		return 0;
	/* Yes, I know this for loop is inefficient and could be replaced
	   by simple pointer comparison, but that would cause undefined
	   behaviour according to ISO C89 and C99, and most inodes
	   don't have enough metadata for this to be a problem.
	   */
	for (i = 0; i < inode->metadata->extent_count; i++) {
		if (extent == &inode->metadata->extents[i])
			return 1;
	}
	return 0;
}

blk64_t get_logical_block(struct inode *inode, blk64_t phys_block)
{
	struct data_extent *d = inode->data->extents;
	blk64_t ret;
	int sparse_nr = 0;
	while ((d - inode->data->extents) < inode->data->extent_count
	       && (d->end_block < phys_block
	           || d->start_block > phys_block))
	{
		d++;
	}
	ret = d->start_logical + phys_block - d->start_block;
	while (sparse_nr < inode->num_sparse) {
		struct sparse_extent *s = &inode->sparse[sparse_nr];
		if (s->start >= d->start_logical)
			break;
		sparse_nr++;
	}
	while (sparse_nr < inode->num_sparse) {
		struct sparse_extent *s = &inode->sparse[sparse_nr];
		if (s->start > ret)
			break;
		ret += s->num_blocks;
		sparse_nr++;
	}
	return ret;
}

/* Returns the physical block corresponding to the requested logical block
 * of the provided inode. If extent_nr is not NULL, the extent number
 * containing that blocks is returned in it. If the block is sparse,
 * 0 is returned. On error, (blk64_t)-1 is returned and errno is set
 * appropriatly.
 */
blk64_t get_physical_block(struct inode *inode, blk64_t logical_block,
                           int *extent_nr)
{
	struct data_extent *extent;
	int current = 0, sparse_nr = 0;
	blk64_t ret;
	while (current < inode->data->extent_count - 1) {
		extent = &inode->data->extents[current + 1];
		if (extent->start_logical <= logical_block)
			current++;
		else
			break;
	}
	extent = &inode->data->extents[current];
	ret = extent->start_block + logical_block - extent->start_logical;
	if (extent_nr)
		*extent_nr = current;
	while (sparse_nr < inode->num_sparse) {
		struct sparse_extent *sparse = &inode->sparse[sparse_nr];
		if (sparse->start >= extent->start_logical)
			break;
		sparse_nr++;
	}
	while (sparse_nr < inode->num_sparse) {
		struct sparse_extent *sparse = &inode->sparse[sparse_nr];
		if (sparse->start > logical_block)
			break;
		if (sparse->start == logical_block)
			return 0;
		ret -= sparse->num_blocks;
		sparse_nr++;
	}
	return ret;
}

/* Note that this function may realloc inode (and therefore extent) */
int split_extent(struct defrag_ctx *c,
                 struct inode *inode,
                 struct data_extent *extent,
                 blk64_t new_end_block)
{
	blk64_t e2_logical_start;
	size_t nbytes;
	ext2_ino_t inode_nr = extent->inode_nr;
	int extent_nr, ret;
	e2_logical_start = get_logical_block(inode, new_end_block + 1);
	inode_remove_from_trees(c, inode);
	extent_nr = extent - inode->data->extents;
	inode->data->extent_count += 1;
	nbytes = sizeof(struct inode);
	nbytes += inode->data->extent_count * sizeof(struct data_extent);
	inode = realloc(inode, nbytes);
	if (!inode)
		return -1;
	c->inodes[inode_nr] = inode;
	memmove(&inode->data->extents[extent_nr + 1], &inode->data->extents[extent_nr],
	        (inode->data->extent_count - extent_nr - 1) * sizeof(struct data_extent));
	inode->data->extents[extent_nr + 1].start_block = new_end_block + 1;
	inode->data->extents[extent_nr + 1].start_logical = e2_logical_start;
	inode->data->extents[extent_nr].end_block = new_end_block;
	for (extent_nr = 0; extent_nr < inode->data->extent_count; extent_nr++) {
		insert_data_extent(c, &inode->data->extents[extent_nr]);
	}
	return ret;
}

static int merge_extents(struct inode *inode,
                         struct data_extent *extent1,
                         struct data_extent *extent2)
{
	int pos, num;
	e2_blkcnt_t logical_gap;
	blk64_t e1_logical_blocks = extent1->end_block - extent1->start_block+1;
	blk64_t e1_logical_end;

	e1_logical_end = extent1->start_logical + e1_logical_blocks - 1;
	logical_gap = (extent2->start_logical - 1) - e1_logical_end;

	extent1->end_block = extent2->end_block;

	pos = extent2 - inode->data->extents;
	num = inode->data->extent_count - pos - 1;
	memmove(extent2, extent2 + 1, num * sizeof(struct data_extent));
	inode->data->extent_count--;
	return 0;
}

int try_extent_merge(struct defrag_ctx *c,
                     struct inode *inode,
                     struct data_extent *extent)
{
	int oldcount = -1, count = 0, downcount = 0, ret;
	char removed_from_tree = 0;

	while (oldcount != count) {
		int pos = extent - inode->data->extents;
		struct data_extent *prev = inode->data->extents + pos - 1;
		struct data_extent *next = inode->data->extents + pos + 1;
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

		if ((pos + 1) < inode->data->extent_count
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
		num_bytes += inode->data->extent_count * sizeof(*inode->data->extents);

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
		for (i = 0; i < inode->data->extent_count; i++)
			insert_data_extent(c, &inode->data->extents[i]);
	}
	return downcount;
}

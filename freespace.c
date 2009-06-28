#include <malloc.h>
#include <errno.h>
#include <obstack.h>
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include "e2defrag.h"
#include "extree.h"

int allocate_space(struct defrag_ctx *c,
                   blk64_t start, e2_blkcnt_t numblocks)
{
	struct free_extent *extent;
	extent = containing_free_extent(c, start);
	if (start == extent->start_block) {
		rb_remove_free_extent(c, extent);
		extent->start_block += numblocks;
		if (extent->end_block < extent->start_block)
			free(extent);
		else
			insert_free_extent(c, extent);
	} else if (extent->end_block == start + numblocks - 1) {
		rb_remove_free_extent(c, extent);
		extent->end_block = start - 1;
		insert_free_extent(c, extent);
	} else {
		struct free_extent *new_extent;
		new_extent = malloc(sizeof *new_extent);
		if (new_extent == NULL)
			return -1;
		new_extent->start_block = extent->start_block;
		new_extent->end_block = start - 1;
		rb_remove_free_extent(c, extent);
		extent->start_block = start + numblocks;
		/* end_block unchanged */
		insert_free_extent(c, extent);
		insert_free_extent(c, new_extent);
	}
	mark_blocks_used(c, start, numblocks);
	return 0;
}

/* extent should not be in the free extent list.
 * extent may be freed if merged.
 * The returned structure will not be in the free extent list.
 */
static struct free_extent *try_merge(struct defrag_ctx *c,
                                     struct free_extent *extent)
{
	struct free_extent *other_extent;

	other_extent = containing_free_extent(c, extent->start_block - 1);
	if (other_extent) {
		rb_remove_free_extent(c, other_extent);
		other_extent->end_block = extent->end_block;
		free(extent);
		extent = other_extent;
	}
	other_extent = containing_free_extent(c, extent->end_block + 1);
	if (other_extent) {
		rb_remove_free_extent(c, other_extent);
		other_extent->start_block = extent->start_block;
		free(extent);
		extent = other_extent;
	}
	return extent;
}

int deallocate_space(struct defrag_ctx *c, blk64_t start, e2_blkcnt_t numblocks)
{
	struct free_extent *extent;

	if ((extent = containing_free_extent(c, start - 1)) != NULL) {
		rb_remove_free_extent(c, extent);
		extent->end_block = start + numblocks - 1;
		extent = try_merge(c, extent);
		if (extent == NULL)
			return -1;
	} else if ((extent = containing_free_extent(c, start + numblocks))) {
		rb_remove_free_extent(c, extent);
		extent->start_block = start;
		extent = try_merge(c, extent);
		if (extent == NULL)
			return -1;
	} else {
		extent = malloc(sizeof *extent);
		if (extent == NULL)
			return -1;
		extent->start_block = start;
		extent->end_block = start + numblocks - 1;
	}
	insert_free_extent(c, extent);
	mark_blocks_unused(c, start, numblocks);
	return 0;
}

int deallocate_blocks(struct defrag_ctx *c, struct allocation *space)
{
	int i, ret = 0;
	for (i = 0; i < space->extent_count; i++) {
		e2_blkcnt_t num_blocks = 1 + space->extents[i].end_block
		                         - space->extents[i].start_block;
		if (ret >= 0)
			ret = deallocate_space(c, space->extents[i].start_block,
		                               num_blocks);
		else
			deallocate_space(c, space->extents[i].start_block,
		                         num_blocks);
	}
	return ret < 0 ? ret : 0;
}

/* TODO: some less naive allocation strategy would be very useful */
struct allocation *allocate_blocks(struct defrag_ctx *c, e2_blkcnt_t num_blocks,
                                   ext2_ino_t inode_nr, blk64_t first_logical)
{
	struct rb_node *n = rb_last(&c->free_tree_by_size);
	if (!n) {
		errno = ENOSPC;
		return NULL;
	}
	struct free_extent *biggest = rb_entry(n, struct free_extent, size_rb);
	struct allocation *ret, *tmp_alloc;
	struct obstack stack;
	size_t size;
	int tmp, i, num_extents = 0;

	obstack_init(&stack);
	obstack_blank(&stack, sizeof(struct allocation));
	while (num_blocks) {
		struct data_extent new_extent;
		e2_blkcnt_t biggest_num_blocks;
		if (!biggest) {
			errno = ENOSPC;
			obstack_free(&stack, NULL);
			return NULL;
		}
		new_extent.start_block = biggest->start_block;
		new_extent.start_logical = first_logical;
		new_extent.end_block = biggest->start_block - 1;
		biggest_num_blocks = biggest->end_block - biggest->start_block;
		biggest_num_blocks += 1;
		if (biggest_num_blocks > num_blocks) {
			new_extent.end_block += num_blocks;
			num_blocks = 0;
		} else {
			new_extent.end_block += biggest_num_blocks;
			num_blocks -= biggest_num_blocks;
			first_logical += biggest_num_blocks;
			n = rb_prev(n);
			if (n == NULL) {
				biggest = NULL;
			} else {
				biggest = rb_entry(n, struct free_extent,
				                   size_rb);
			}
		}
		new_extent.sparse = NULL;
		new_extent.inode_nr = inode_nr;
		obstack_grow(&stack, &new_extent, sizeof(new_extent));
		num_extents++;
	}
	size = obstack_object_size(&stack);
	ret = malloc(size);
	if (!ret)
		return NULL;
	tmp_alloc = obstack_finish(&stack);
	memcpy(ret, tmp_alloc, size);
	obstack_free(&stack, NULL);
	ret->block_count = 0;

	for (i = 0; i < num_extents; i++) {
		num_blocks = ret->extents[i].end_block + 1;
		num_blocks -= ret->extents[i].start_block;
		ret->block_count += num_blocks;
		tmp=allocate_space(c, ret->extents[i].start_block, num_blocks);
		if (tmp < 0)
			goto error_dealloc;
	}
	ret->extent_count = num_extents;
	return ret;

error_dealloc:
	; /* null statement because labels cannot precede declarations */
	int j;
	for (j = 0; j < i; j++) {
		num_blocks = ret->extents[j].end_block + 1;
		num_blocks -= ret->extents[j].start_block;
		deallocate_space(c, ret->extents[j].start_block, num_blocks);
	}
	free(ret);
	return NULL;
}

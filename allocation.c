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
#include <malloc.h>
#include <string.h>
#include <obstack.h>
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include "e2defrag.h"

struct allocation *copy_allocation(struct allocation *old)
{
	struct allocation *ret;
	size_t size;
	size = sizeof(struct allocation);
	size += old->extent_count * sizeof(struct data_extent);
	ret = malloc(size);
	if (!ret)
		return NULL;
	memcpy(ret, old, size);
	return ret;
}

/* Will never free or realloc the old allocation. Never call this with an
 * allocation already on the trees, as we will overwrite the old extent
 * without updating the trees if we can merge.
 */
void alloc_move_extent(struct allocation *alloc, struct data_extent *extent,
                       blk64_t new_start)
{
	e2_blkcnt_t nblocks;
	nblocks = extent->end_block - extent->start_block + 1;
	extent->start_block = new_start;
	extent->end_block = new_start + nblocks - 1;
	if (extent != alloc->extents) {
		struct data_extent *prev_extent = extent - 1;
		if (new_start == prev_extent->end_block + 1
		    && extent->uninit == prev_extent->uninit)
		{
			size_t count, nbytes;
			prev_extent->end_block = extent->end_block;
			/* count = nr. of following extents in the array */
			count = alloc->extent_count - (extent - alloc->extents);
			count -= 1;
			nbytes = count * sizeof(struct data_extent);
			memmove(extent, extent + 1, nbytes);
			extent = extent - 1;
			alloc->extent_count--;
		}
	}
	if (extent - alloc->extents != alloc->extent_count - 1) {
		struct data_extent *next_extent = extent + 1;
		if (extent->end_block + 1 == next_extent->start_block
		    && extent->uninit == next_extent->uninit)
		{
			size_t count, nbytes;
			extent->end_block = next_extent->end_block;
			/* count = nr. of following extents in the array */
			count = alloc->extent_count - (extent - alloc->extents);
			nbytes = count * sizeof(struct data_extent);
			memmove(next_extent, next_extent + 1, nbytes);
			alloc->extent_count--;
		}
	}
}

int used_in_alloc(struct allocation *alloc, blk64_t range_start,
                  e2_blkcnt_t range_size)
{
	int i;
	blk64_t range_end = range_start + range_size - 1;
	for (i = 0; i < alloc->extent_count; i++) {
		if (range_end <= alloc->extents[i].end_block
		    && range_start >= alloc->extents[i].start_block)
		{
			return 1;
		}
	}
	return 0;
}

static void alloc_subtract_range(struct data_extent *range,
                                 struct allocation *alloc,
                                 struct obstack *mempool)
{
	int i;
	if (range->end_block < range->start_block)
		return;
	for (i = 0; i < alloc->extent_count; i++) {
		if (range->start_block <= alloc->extents[i].end_block
		    && (range->end_block >= alloc->extents[i].start_block))
		{
			/* Preceding range */
			if (range->start_block < alloc->extents[i].start_block)
			{
				struct data_extent new;
				new = *range;
				new.end_block = alloc->extents[i].start_block;
				new.end_block -= 1;
				alloc_subtract_range(&new, alloc, mempool);
			}
			/* Following range */
			if (range->end_block > alloc->extents[i].end_block) {
				struct data_extent new;
				new = *range;
				new.start_block = alloc->extents[i].end_block;
				new.start_block += 1;
				alloc_subtract_range(&new, alloc, mempool);
			}
			return;
		}
	}
	obstack_grow(mempool, range, sizeof(*range));
}

struct allocation *alloc_subtract(struct allocation *from,
                                  struct allocation *data)
{
	struct obstack mempool;
	struct allocation *ret;
	long size, i;
	obstack_init(&mempool);
	obstack_blank(&mempool, sizeof(struct allocation));
	for (i = 0; i < from->extent_count; i++) {
		struct data_extent *extent;
		extent = &from->extents[i];
		alloc_subtract_range(extent, data, &mempool);
	}
	size = obstack_object_size(&mempool);
	ret = malloc(size);
	if (!ret) {
		obstack_free(&mempool, NULL);
		return NULL;
	}
	memcpy(ret, obstack_finish(&mempool), size);
	obstack_free(&mempool, NULL);
	size -= sizeof(struct allocation);
	size /= sizeof(struct data_extent);
	ret->extent_count = size;
	ret->block_count = 0;
	for (i = 0; i < size; i++) {
		struct data_extent *extent = &ret->extents[i];
		ret->block_count += extent->end_block - extent->start_block + 1;
	}
	return ret;
}

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

/* File for performing block moves */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include "e2defrag.h"
#include "extree.h"

static const size_t copy_buffer_size = 65536;

#ifndef NOSPLICE
static int __move_block_range_nosplice(struct defrag_ctx *c, blk64_t from,
                                       blk64_t to, size_t nr_blocks)
#else
static int __move_block_range(struct defrag_ctx *c, blk64_t from, blk64_t to,
                              size_t nr_blocks)
#endif
{
	static unsigned char *copy_buffer;
	int ret;
	off_t from_offset, to_offset, tmp;
	ssize_t size;

	if (global_settings.simulate || global_settings.no_data_move)
		return 0;
	if (!copy_buffer) {
		copy_buffer = malloc(copy_buffer_size);
		if (!copy_buffer)
			return -1;
	}
	from_offset = from * EXT2_BLOCK_SIZE(&c->sb);
	to_offset = to * EXT2_BLOCK_SIZE(&c->sb);
	size = EXT2_BLOCK_SIZE(&c->sb) * nr_blocks;
	while (size > 0) {
		ssize_t to_write;
		tmp = lseek(c->fd, from_offset, SEEK_SET);
		if (tmp == (off_t) -1)
			return -1;
		to_write = size > copy_buffer_size ? copy_buffer_size : size;
		ret = read(c->fd, copy_buffer, to_write);
		if (ret <= 0)
			return ret;

		size -= ret;
		to_write = ret;
		from_offset += ret;
		while (to_write > 0) {
			tmp = lseek(c->fd, to_offset, SEEK_SET);
			ret = write(c->fd, copy_buffer, to_write);
			if (ret <= 0)
				return ret;
			to_write -= ret;
			to_offset += ret;
		}
	}
	return 0;
}

#ifndef NOSPLICE
static int __move_block_range(struct defrag_ctx *c, blk64_t from, blk64_t to,
                              size_t nr_blocks)
{
	static int transfer_pipe[2] = {-1, -1};
	static char has_splice = 1; /* For older kernels */
	int ret, size;
	loff_t from_offset, to_offset;

	if (global_settings.simulate || global_settings.no_data_move)
		return 0;
	if (!has_splice)
		return __move_block_range_nosplice(c, from, to, nr_blocks);

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
		if (ret < 0) {
			if (errno == ENOSYS) {
				has_splice = 0;
				close(transfer_pipe[0]);
				close(transfer_pipe[1]);
				return __move_block_range_nosplice(c, from, to,
				                                   nr_blocks);
			}
			return ret;
		} else {
			size -= ret;
		}
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
#endif /* NOSPLICE */

/* This function splits and extent in two. The first extent will contain the
 * blocks from the start of the given extent up to new_end_block. The second
 * extent will contain the blocks from new_end_block+1 up to the end of the
 * given extent. The relative locations of the new extents in the allocation
 * will be that of the old extent.
 * It is important that none of the extents in the allocation are on any
 * extent trees (as the allocation will be freed).
 */
static struct allocation *split_extent(struct defrag_ctx *c,
                                       struct allocation *alloc,
                                       struct data_extent *extent,
                                       blk64_t new_end_block,
                                       blk64_t new_start_logical)
{
	size_t nbytes;
	int extent_nr;

	for (extent_nr = 0; extent_nr < alloc->extent_count; extent_nr++)
		rb_remove_data_extent(c, &alloc->extents[extent_nr]);

	extent_nr = extent - alloc->extents;
	alloc->extent_count += 1;
	nbytes = sizeof(struct allocation);
	nbytes += alloc->extent_count * sizeof(struct data_extent);
	alloc = realloc(alloc, nbytes);
	if (!alloc)
		return NULL;
	memmove(&alloc->extents[extent_nr + 1], &alloc->extents[extent_nr],
	                               (alloc->extent_count - extent_nr - 1)
	                               * sizeof(struct data_extent));
	alloc->extents[extent_nr + 1].start_block = new_end_block + 1;
	alloc->extents[extent_nr + 1].start_logical = new_start_logical;
	alloc->extents[extent_nr].end_block = new_end_block;
	for (extent_nr = 0; extent_nr < alloc->extent_count; extent_nr++)
		insert_data_extent(c, &alloc->extents[extent_nr]);
	return alloc;
}

/* Target must have exactly one extent (for now) and exactly as many blocks
   as the source extent. Target is no longer valid afterwards and must be
   cleaned up by the caller. */
int move_data_extent(struct defrag_ctx *c, struct data_extent *extent_to_copy,
                     struct allocation *target)
{
	struct inode *i = c->inodes[extent_to_copy->inode_nr];
	blk64_t old_start;
	e2_blkcnt_t blk_cnt;
	int ret;

	if (target->extent_count > 1) {
		errno = ENOSYS;
		return -1;
	}
	blk_cnt = extent_to_copy->end_block - extent_to_copy->start_block + 1;
	if (blk_cnt != target->extents[0].end_block -
	               target->extents[0].start_block + 1)
	{
		errno = EINVAL;
		return -1;
	}
	if (!extent_to_copy->uninit) {
		ret = __move_block_range(c, extent_to_copy->start_block,
	                                 target->extents[0].start_block,
		                         blk_cnt);
	} else {
		ret = 0;
	}
	if (!ret)
		ret = fdatasync(c->fd);
	if (ret)
		return ret;
	rb_remove_data_extent(c, &target->extents[0]);
	old_start = extent_to_copy->start_block;
	rb_remove_data_extent(c, extent_to_copy);
	*extent_to_copy = target->extents[0];
	insert_data_extent(c, extent_to_copy);
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
	return ret;
}

int copy_data(struct defrag_ctx *c, struct allocation *from,
              struct allocation *target)
{
	struct data_extent *from_extent, *to_extent;
	blk64_t cur_dest, cur_from;
	e2_blkcnt_t blocks_copied = 0;

	if (from->block_count != target->block_count) {
		errno = EINVAL;
		return 0;
	}

	to_extent = target->extents;
	from_extent = from->extents;
	to_extent->uninit = from_extent->uninit;
	cur_dest = to_extent->start_block;
	cur_from = from_extent->start_block;

	while (blocks_copied < from->block_count) {
		e2_blkcnt_t num_blocks;
		int ret;
		if (cur_from > from_extent->end_block) {
			from_extent++;
			cur_from = from_extent->start_block;
		}
		if (cur_dest > to_extent->end_block) {
			to_extent++;
			to_extent->uninit = from_extent->uninit;
			cur_dest = to_extent->start_block;
		}
		if (from_extent->uninit != to_extent->uninit) {
			struct allocation *new_target;
			struct inode *inode = c->inodes[to_extent->inode_nr];
			blk64_t new_start_logical;
			int extent_nr = to_extent - target->extents;
			new_start_logical = get_logical_block(inode, cur_from);
			new_target = split_extent(c, target, to_extent,
			                          cur_dest - 1,
						  new_start_logical);
			if (!new_target)
				return -1;
			target = new_target;
			to_extent = &target->extents[extent_nr + 1];
			to_extent->uninit = from_extent->uninit;
			assert(cur_dest == to_extent->start_block);
		}
		num_blocks = from_extent->end_block - cur_from + 1;
		if (to_extent->end_block - cur_dest + 1 < num_blocks)
			num_blocks = to_extent->end_block - cur_dest + 1;

		if (!to_extent->uninit) {
			ret = __move_block_range(c, cur_from, cur_dest,
			                                            num_blocks);
			if (ret)
				return ret;
		}

		blocks_copied += num_blocks;
		cur_dest += num_blocks;
		cur_from += num_blocks;
	}
	return 0;
}

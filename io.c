#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <sys/mman.h>
#include "e2defrag.h"
#include "extree.h"

int read_block(struct defrag_ctx *c, void *buf, blk64_t block)
{
	long long ret;
	ret = lseek64(c->fd, block * EXT2_BLOCK_SIZE(&c->sb), SEEK_SET);
	if (ret < 0) {
		printf("%lld\n", ret);
		printf("Cannot seek to block %llu (block_size %d)\n", block,
		       EXT2_BLOCK_SIZE(&c->sb));
		return -1;
	}
	ret = read(c->fd, buf, EXT2_BLOCK_SIZE(&c->sb));
	if (ret < EXT2_BLOCK_SIZE(&c->sb))
		return -1;
	return 0;
}

int write_block(struct defrag_ctx *c, void *buf, blk64_t block)
{
	long long ret;
	ret = lseek64(c->fd, block * EXT2_BLOCK_SIZE(&c->sb), SEEK_SET);
	if (ret < 0) {
		printf("Cannot seek to block %llu (block_size %d)\n", block,
		       EXT2_BLOCK_SIZE(&c->sb));
		return -1;
	}
	ret = write(c->fd, buf, EXT2_BLOCK_SIZE(&c->sb));
	if (ret < EXT2_BLOCK_SIZE(&c->sb))
		return -1;
	return 0;
}

struct defrag_ctx *open_drive(char *filename, char read_only)
{
	struct defrag_ctx *ret;
	struct ext2_super_block sb;
	int tmp, fd;
	int nr_block_groups;

	fd = open(filename, read_only ? O_RDONLY : O_RDWR);
	if (fd < 0)
		goto error_out;

	tmp = lseek(fd, SUPERBLOCK_OFFSET, SEEK_SET);
	if (tmp < 0)
		goto error_open;
	tmp = read(fd, &sb, SUPERBLOCK_SIZE);
	if (tmp < SUPERBLOCK_SIZE)
		goto error_open;

	ret = calloc(sizeof(struct defrag_ctx)
	             + sizeof(struct inode *) * sb.s_inodes_count, 1);
	if (!ret)
		goto error_open;
	nr_block_groups = sb.s_blocks_count / sb.s_blocks_per_group;
	if (sb.s_blocks_count % sb.s_blocks_per_group)
		nr_block_groups++;
	ret->bg_maps = malloc(nr_block_groups * sizeof(*ret->bg_maps));
	if (!ret->bg_maps)
		goto error_alloc;
	ret->fd = fd;
	ret->sb = sb;
	ret->read_only = read_only;
	ret->extents_by_block = RB_ROOT;
	ret->extents_by_size = RB_ROOT;
	ret->free_tree = RB_ROOT;
	return ret;

error_alloc:
	free(ret);
error_open:
	close(fd);
error_out:
	return NULL;
}

long parse_inode_table(struct defrag_ctx *c, blk64_t bitmap_block,
                       blk64_t table_start, int group_nr)
{
	unsigned char *bitmap;
	off_t bitmap_start_offset, bitmap_delta_offset;
	size_t bitmap_length;
	unsigned char *inode_table;
	off_t table_start_offset, table_delta_offset;
	size_t table_length;
	long count = 0;
	const ext2_ino_t first_inode = group_nr * c->sb.s_inodes_per_group;
	int i, ret;

	bitmap_start_offset = bitmap_block * EXT2_BLOCK_SIZE(&c->sb);
	bitmap_delta_offset = bitmap_start_offset % getpagesize();
	bitmap_length = (c->sb.s_inodes_per_group + CHAR_BIT - 1) / CHAR_BIT;
	if (bitmap_delta_offset) {
		bitmap_start_offset -= bitmap_delta_offset;
		bitmap_length += bitmap_delta_offset;
	}
	if (bitmap_length % getpagesize()) {
		bitmap_length += getpagesize();
		bitmap_length -= (bitmap_length % getpagesize());
	}

	bitmap = mmap(NULL, bitmap_length, PROT_READ | PROT_WRITE, MAP_PRIVATE,
	              c->fd, bitmap_start_offset);
	if (bitmap == MAP_FAILED)
		return -1;
	bitmap = bitmap + bitmap_delta_offset;

	table_start_offset = table_start * EXT2_BLOCK_SIZE(&c->sb);
	table_delta_offset = table_start_offset % getpagesize();
	table_length = c->sb.s_inodes_per_group * EXT2_INODE_SIZE(&c->sb);
	if (table_delta_offset) {
		table_start_offset -= table_delta_offset;
		table_length += table_delta_offset;
	}
	if (table_length % getpagesize())
		table_length += getpagesize() - (table_length % getpagesize());
	i = c->read_only ? (PROT_READ) : (PROT_READ | PROT_WRITE);
	inode_table = mmap(NULL, table_length, i, MAP_SHARED, c->fd,
	                   table_start_offset);
	if (inode_table == MAP_FAILED) {
		munmap(bitmap - bitmap_delta_offset, bitmap_length);
		return -1;
	}
	c->bg_maps[group_nr].map_start = (void *)inode_table;
	c->bg_maps[group_nr].inode_map_length = table_length;
	inode_table += table_delta_offset;

	for (i = 0; i < c->sb.s_inodes_per_group; i++) {
		if (bitmap[i / CHAR_BIT] == 0) {
			i += CHAR_BIT - (i % CHAR_BIT);
			continue;
		}
		if (bitmap[i / CHAR_BIT] & 1) {
			struct ext2_inode *inode;
			inode = (struct ext2_inode *)
			            (inode_table + i * EXT2_INODE_SIZE(&c->sb));
			ext2_ino_t inode_nr = first_inode + i + 1;
			/* +1 because we start counting inodes at 1
			 * for convention.
			 */
			printf("At inode %u of %u\r", inode_nr,
			                                  c->sb.s_inodes_count);
			ret = parse_inode(c, inode_nr, inode);
			if (ret < 0)
				return -1;
			count++;
		}
		bitmap[i / CHAR_BIT] >>= 1;
	}
	munmap(bitmap - bitmap_delta_offset, bitmap_length);
	return count;
}

void close_drive(struct defrag_ctx *c)
{
	int i;
	for (i = 0; i < c->nr_inode_maps; i++) {
		munmap(c->bg_maps[i].map_start,
		       c->bg_maps[i].inode_map_length);
		munmap(c->bg_maps[i].bitmap - c->bg_maps[i].bitmap_offset,
		       c->bg_maps[i].bitmap_map_length);
	}
	free(c->bg_maps);
	for (i = 0; i < c->sb.s_inodes_count; i++) {
		int j;
		if (!c->inodes[i])
			continue;
		for (j = 0; j < c->inodes[i]->extent_count; j++) {
			struct data_extent *e = &c->inodes[i]->extents[j];
			struct sparse_extent *s = e->sparse;
			free (s);
		}
		free(c->inodes[i]);
	}

	while (c->free_tree.rb_node) {
		struct free_extent *f;
		f = rb_entry(c->free_tree.rb_node, struct free_extent, node);
		rb_erase(c->free_tree.rb_node, &c->free_tree);
		free(f);
	}
	close(c->fd);
	free(c);
}

long parse_free_bitmap(struct defrag_ctx *c, blk64_t bitmap_block,
                       int group_nr)
{
	unsigned char *bitmap;
	off_t start_offset, delta_offset;
	size_t map_length;
	const blk64_t first_block = group_nr * c->sb.s_blocks_per_group;
	struct free_extent *free_extent = NULL;
	struct data_extent *file_extent = NULL;
	long count = 0;
	int i;

	start_offset = bitmap_block * EXT2_BLOCK_SIZE(&c->sb);
	map_length = c->sb.s_blocks_per_group / CHAR_BIT;
	delta_offset = start_offset % getpagesize();
	start_offset -= delta_offset;
	map_length += delta_offset;
	if (map_length % getpagesize())
		map_length += getpagesize() - (map_length % getpagesize());

	i = c->read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
	bitmap = mmap(NULL, map_length, i, MAP_SHARED, c->fd, start_offset);
	if (bitmap == MAP_FAILED)
		return -1;
	bitmap += delta_offset;
	c->bg_maps[group_nr].bitmap = bitmap;
	c->bg_maps[group_nr].bitmap_map_length = map_length;
	c->bg_maps[group_nr].bitmap_offset = delta_offset;

	for (i = 0; i < c->sb.s_blocks_per_group; i += CHAR_BIT) {
		int j;
		unsigned char mask = 1;
		if (file_extent && file_extent->end_block > i + CHAR_BIT - 1)
			continue;
		if (bitmap[i / CHAR_BIT] == 0) {
			if (!free_extent) {
				free_extent=malloc(sizeof(struct free_extent));
				if (!free_extent)
					return -1;
				free_extent->start_block = first_block + i;
			}
			free_extent->end_block = first_block + i + CHAR_BIT - 1;
			continue;
		}
		for (j = 0; j < CHAR_BIT; j++) {
			blk64_t block = first_block + i + j;
			if (file_extent && file_extent->end_block > block) {
				mask <<= 1;
				continue;
			}
			if (block >= c->sb.s_blocks_count)
				break;
			if (bitmap[i / CHAR_BIT] & mask) {
				if (free_extent) {
					insert_free_extent(c, free_extent);
					free_extent = NULL;
				}
				file_extent = containing_data_extent(c, block);
				if (file_extent) {
					mask <<= 1;
					continue;
				}
				count++;
			} else {
				if (!free_extent) {
					free_extent = malloc(
						    sizeof(struct free_extent));
					if (!free_extent)
						return -1;
					free_extent->start_block = block;
				}
				free_extent->end_block = block;
			}
			mask <<= 1;
		}
	}
	if (free_extent)
		insert_free_extent(c, free_extent);
	return count;
}

int set_e2_filesystem_data(struct defrag_ctx *c)
{
	int num_block_groups = (c->sb.s_blocks_count
	                        + c->sb.s_blocks_per_group - 1)
	                       / c->sb.s_blocks_per_group;
	struct ext2_group_desc gds[num_block_groups];
	int ret, i;

	if (lseek(c->fd, EXT2_BLOCK_SIZE(&c->sb), SEEK_SET) == (off_t) -1)
		return -1;
	ret = read(c->fd, gds, sizeof(*gds) * num_block_groups);
	if (ret < sizeof(*gds) * num_block_groups)
		return -1;

	for (i = 0; i < num_block_groups; i++) {
		long ret = 0;
		if ((gds[i].bg_flags & (EXT2_BG_INODE_UNINIT
		                        | EXT2_BG_INODE_ZEROED)))
			continue;
		ret = parse_inode_table(c, gds[i].bg_inode_bitmap,
		                        gds[i].bg_inode_table, i);
		if (ret < 0)
			return -1;
	}

	for (i = 0; i < num_block_groups; i++) {
		long ret = 0;
		if (!(gds[i].bg_flags & EXT2_BG_BLOCK_UNINIT))
			ret = parse_free_bitmap(c, gds[i].bg_block_bitmap, i);
		if (ret < 0)
			return -1;
	}

#if 1 /* EXTENT DISPLAY */
	struct rb_node *n = rb_first(&c->free_tree);
	while (n) {
		struct free_extent *f = rb_entry(n, struct free_extent, node);
		printf("F: %llu-%llu\n", f->start_block, f->end_block);
		n = rb_next(n);
	}
	n = rb_first(&c->extents_by_block);
	while (n) {
		struct data_extent *f = rb_entry(n,struct data_extent,block_rb);
		printf("U: %llu-%llu(%llu) of %u\n", f->start_block,
		       f->end_block, f->start_logical, f->inode_nr);
		n = rb_next(n);
	}
#endif
	return 0;
}

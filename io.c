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
#include "crc16.h"

int read_block(struct defrag_ctx *c, void *buf, blk64_t block)
{
	long long ret;
	ret = lseek64(c->fd, block * EXT2_BLOCK_SIZE(&c->sb), SEEK_SET);
	if (ret < 0) {
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
	if (global_settings.simulate)
		return 0;
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

static int map_gds(struct defrag_ctx *c)
{
	int num_block_groups = c->sb.s_blocks_count / c->sb.s_blocks_per_group;
	if (c->sb.s_blocks_count % c->sb.s_blocks_per_group)
		num_block_groups++;
	size_t map_length = num_block_groups * EXT2_DESC_SIZE(&c->sb);
	off_t map_offset = 0;
	off_t gd_offset = EXT2_BLOCK_SIZE(&c->sb);
	int flags, i;
	if (gd_offset < SUPERBLOCK_OFFSET + SUPERBLOCK_SIZE)
		gd_offset = SUPERBLOCK_OFFSET + SUPERBLOCK_SIZE;
	if (gd_offset % getpagesize()) {
		map_offset = gd_offset % getpagesize();
		map_length += map_offset;
		gd_offset -= map_offset;
	}
	if (map_length % getpagesize())
		map_length += getpagesize() - map_length % getpagesize();
	flags = global_settings.simulate ? MAP_PRIVATE : MAP_SHARED;
	c->gd_map = mmap(NULL, map_length, PROT_READ | PROT_WRITE, flags,
	                 c->fd, gd_offset);
	if (c->gd_map == MAP_FAILED)
		return -1;
	c->gd_map = ((char *)(c->gd_map)) + map_offset;
	c->map_length = map_length;

	for (i = 0; i < num_block_groups; i++) {
		c->bg_maps[i].gd = (void *)
		                   (c->gd_map + i * EXT2_DESC_SIZE(&c->sb));
	}
	return 0;
}

struct defrag_ctx *open_drive(char *filename)
{
	struct defrag_ctx *ret;
	struct ext2_super_block sb;
	int tmp, fd;
	int nr_block_groups;

	fd = open(filename, global_settings.simulate ? O_RDONLY : O_RDWR);
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
	ret->extents_by_block = RB_ROOT;
	ret->extents_by_size = RB_ROOT;
	ret->free_tree_by_size = RB_ROOT;
	ret->free_tree_by_block = RB_ROOT;
	tmp = map_gds(ret);
	if (tmp)
		goto error_alloc_maps;
	return ret;

error_alloc_maps:
	free(ret->bg_maps);
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
	i = global_settings.simulate ? MAP_PRIVATE : MAP_SHARED;
	inode_table = mmap(NULL, table_length, PROT_READ | PROT_WRITE, i, c->fd,
	                   table_start_offset);
	if (inode_table == MAP_FAILED) {
		munmap(bitmap - bitmap_delta_offset, bitmap_length);
		return -1;
	}
	c->bg_maps[group_nr].map_start = inode_table;
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
	printf("\n");
	munmap(bitmap - bitmap_delta_offset, bitmap_length);
	return count;
}

void close_drive(struct defrag_ctx *c)
{
	int i;
	for (i = 0; i < c->nr_inode_maps; i++) {
		int ret;
		ret = munmap(c->bg_maps[i].map_start,
		             c->bg_maps[i].inode_map_length);
		if (ret) {
			printf("Could not unmap inode map %d\n", i);
			printf("Params: %p %ld\n",
			       (c->bg_maps[i].map_start),
			       c->bg_maps[i].inode_map_length);
		}
		ret = munmap(c->bg_maps[i].bitmap - c->bg_maps[i].bitmap_offset,
		             c->bg_maps[i].bitmap_map_length);
		if (ret) {
			printf("Could not unmap bitmap %d\n", i);
			printf("Params: %p %ld\n", c->bg_maps[i].bitmap
			       - c->bg_maps[i].bitmap_offset,
			       c->bg_maps[i].bitmap_map_length);
		}
	}
	free(c->bg_maps);
	for (i = 0; i < c->sb.s_inodes_count; i++) {
		int j;
		if (!c->inodes[i])
			continue;
		for (j = 0; j < c->inodes[i]->data->extent_count; j++) {
			struct data_extent *e = &c->inodes[i]->data->extents[j];
			struct sparse_extent *s = e->sparse;
			free (s);
		}
		if (c->inodes[i]->metadata)
			free(c->inodes[i]->metadata);
		free(c->inodes[i]);
	}
	c->gd_map = PAGE_START(c->gd_map);
	i = munmap(c->gd_map, c->map_length);
	if (i < 0) {
		printf("Could not unmap group descriptors: %s\n",
		       strerror(errno));
		printf("Params: %p %ld\n", c->gd_map, c->map_length);
	}

	while (c->free_tree_by_size.rb_node) {
		struct free_extent *f;
		f = rb_entry(c->free_tree_by_size.rb_node,
		             struct free_extent, size_rb);
		rb_erase(c->free_tree_by_size.rb_node, &c->free_tree_by_size);
		free(f);
	}
	close(c->fd);
	free(c);
}

long add_uninit_bg(struct defrag_ctx *c, struct ext2_group_desc *gd,
                   uint32_t group_nr)
{
	unsigned char bitmap[EXT2_BLOCK_SIZE(&c->sb)];
	blk64_t first_block = group_nr * c->sb.s_blocks_per_group;
	first_block += c->sb.s_first_data_block;
	blk64_t last_block = first_block + c->sb.s_blocks_per_group - 1;
	blk64_t block;
	int byte, bit, num_inode_blocks, ret;
	memset(bitmap, 0, EXT2_BLOCK_SIZE(&c->sb));
	block = gd->bg_inode_bitmap;
	if (block >= first_block && block <= last_block) {
		block -= first_block;
		byte = block / 8;
		bit = block % 8;
		bitmap[byte] |= 1 << bit;
	}
	block = gd->bg_inode_table;
	if (block >= first_block && block <= last_block) {
		block -= first_block;
		byte = block / 8;
		bit = block % 8;
		num_inode_blocks = EXT2_INODES_PER_GROUP(&c->sb) /
	                                          EXT2_INODES_PER_BLOCK(&c->sb);
		while (num_inode_blocks) {
			bitmap[byte] |= 1 << bit;
			bit++;
			if (bit == 8) {
				bit = 0;
				byte++;
			}
			num_inode_blocks--;
		}
	}
	ret = write_block(c, bitmap, gd->bg_block_bitmap);
	if (ret)
		return ret;
	ret = fsync(c->fd);
	gd->bg_flags &= ~EXT2_BG_BLOCK_UNINIT;
	if (c->sb.s_rev_level != EXT2_GOOD_OLD_REV) {
		char *offset;
		int size;
		uint16_t crc;
		crc = crc16(~0, c->sb.s_uuid, sizeof(c->sb.s_uuid));
		crc = crc16(crc, (void *)&group_nr, sizeof(group_nr));
		crc = crc16(crc, (void *)gd,
		            offsetof(struct ext4_group_desc, bg_checksum));
		offset = (char *)&gd->bg_checksum;
		offset += sizeof(crc);
		size = EXT2_DESC_SIZE(&c->sb);
		size -= offsetof(struct ext4_group_desc, bg_checksum);
		size -= sizeof(crc);
		crc = crc16(crc, (void *)offset, size);
		gd->bg_checksum = crc;
	}
	return 0;
}

long parse_free_bitmap(struct defrag_ctx *c, blk64_t bitmap_block,
                       int group_nr)
{
	unsigned char *bitmap;
	off_t start_offset, delta_offset;
	size_t map_length;
	blk64_t first_block = group_nr * c->sb.s_blocks_per_group;
	first_block += c->sb.s_first_data_block;
	struct free_extent *free_extent;
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

	i = global_settings.simulate ? MAP_PRIVATE : MAP_SHARED;
	bitmap = mmap(NULL, map_length, PROT_READ | PROT_WRITE, i,
	              c->fd, start_offset);
	if (bitmap == MAP_FAILED) {
		fprintf(stderr, "Could not map bitmap");
		return -1;
	}
	bitmap += delta_offset;
	c->bg_maps[group_nr].bitmap = bitmap;
	c->bg_maps[group_nr].bitmap_map_length = map_length;
	c->bg_maps[group_nr].bitmap_offset = delta_offset;

	free_extent = containing_free_extent(c, first_block - 1);
	if (free_extent)
		rb_remove_free_extent(c, free_extent);
	for (i = 0; i < c->sb.s_blocks_per_group; i += CHAR_BIT) {
		int j;
		unsigned char mask = 1;
		if (file_extent
		    && file_extent->end_block > i + first_block + CHAR_BIT - 1)
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
	struct ext2_group_desc *gds;
	int i;
	gds = (struct ext2_group_desc *)c->gd_map;

	for (i = 0; i < num_block_groups; i++) {
		long ret = 0;
		if ((gds[i].bg_flags & (EXT2_BG_INODE_UNINIT))
		    || gds[i].bg_free_inodes_count == c->sb.s_inodes_per_group){
			continue;
		}
		ret = parse_inode_table(c, gds[i].bg_inode_bitmap,
		                        gds[i].bg_inode_table, i);
		if (ret < 0)
			return -1;
	}

	for (i = 0; i < num_block_groups; i++) {
		long ret = 0;
		if (!(gds[i].bg_flags & EXT2_BG_BLOCK_UNINIT)) {
			ret = parse_free_bitmap(c, gds[i].bg_block_bitmap, i);
		} else {
			ret = add_uninit_bg(c, &gds[i], i);
			if (!ret) {
				ret = parse_free_bitmap(c,
				                        gds[i].bg_block_bitmap,
				                        i);
			}
		}
		if (ret < 0)
			return -1;
	}

	return 0;
}

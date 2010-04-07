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
#define _XOPEN_SOURCE 600
#define _BSD_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <sys/mman.h>
#include "e2defrag.h"
#include "extree.h"
#include "crc16.h"
#include "jbd2.h"

int read_block(struct defrag_ctx *c, void *buf, blk64_t block)
{
	ssize_t ret;
	off_t offset;
	offset = block * EXT2_BLOCK_SIZE(&c->sb);
	ret = pread(c->fd, buf, EXT2_BLOCK_SIZE(&c->sb), offset);
	if (ret < EXT2_BLOCK_SIZE(&c->sb))
		return -1;
	return 0;
}

int write_block(struct defrag_ctx *c, void *buf, blk64_t block)
{
	ssize_t ret;
	off_t offset;
	if (global_settings.simulate)
		return 0;
	offset = block * EXT2_BLOCK_SIZE(&c->sb);
	ret = pwrite(c->fd, buf, EXT2_BLOCK_SIZE(&c->sb), offset);
	if (ret < EXT2_BLOCK_SIZE(&c->sb))
		return -1;
	return 0;
}

int write_inode(struct defrag_ctx *c, ext2_ino_t inode_nr)
{
	struct ext2_group_desc *gd;
	char *block_start;
	off_t block;
	int group_nr;
	block_start = c->inode_map_start;
	block = (inode_nr - 1) * EXT2_INODE_SIZE(&c->sb);
	block -= block % EXT2_BLOCK_SIZE(&c->sb);
	block_start += block;
	group_nr = inode_nr / EXT2_INODES_PER_GROUP(&c->sb);
	gd = (void *)((char *)(c->gd_map) + group_nr * EXT2_DESC_SIZE(&c->sb));
	block = gd->bg_inode_table;
	inode_nr = inode_nr % EXT2_INODES_PER_GROUP(&c->sb);
	block += inode_nr / EXT2_INODES_PER_BLOCK(&c->sb);
	return write_block(c, block_start, block);
}

int write_bitmap_block(struct defrag_ctx *c, __u32 group_nr)
{
	struct ext2_group_desc *gd;
	unsigned char *block_start;
	off_t block;
	block_start = c->bitmap + group_nr * EXT2_BLOCK_SIZE(&c->sb);
	gd = (void *)((char *)(c->gd_map) + group_nr * EXT2_DESC_SIZE(&c->sb));
	block = gd->bg_block_bitmap;
	return write_block(c, block_start, block);
}

int write_gd(struct defrag_ctx *c, __u32 group_nr)
{
	char *gd;
	blk64_t block_nr;
	off_t disk_offset;
	off_t offset;
	disk_offset = EXT2_BLOCK_SIZE(&c->sb);
	if (disk_offset < SUPERBLOCK_OFFSET + SUPERBLOCK_SIZE)
		disk_offset = SUPERBLOCK_OFFSET + SUPERBLOCK_SIZE;
	offset = group_nr * EXT2_DESC_SIZE(&c->sb);
	block_nr = (disk_offset + offset) / EXT2_BLOCK_SIZE(&c->sb);
	gd = c->gd_map + (offset - (offset % EXT2_BLOCK_SIZE(&c->sb)));
	return write_block(c, gd, block_nr);
}

/* Sync all flushed transactions to the journal */
int sync_journal(struct defrag_ctx *disk)
{
	char *base = disk->journal->map;
	struct journal_superblock_s *sb;
	struct journal_trans *trans;
	int tmp;
	base -= disk->journal->map_offset;
	sb = (void *)disk->journal->map;
	if (disk->journal->transactions
	    && disk->journal->transactions->transaction_state >= TRANS_FLUSHED)
	{
		struct journal_trans *trans;
		struct journal_header_s *hdr;
		trans = disk->journal->transactions;
		sb->s_start = htobe32(trans->start_block);
		hdr = (void *)((char *)disk->journal->map
		              + disk->journal->blocksize * trans->start_block);
		sb->s_sequence = hdr->h_sequence;
	} else {
		sb->s_start = 0;
	}
	if (!disk->journal->journal_alloc) {
		tmp = msync(base,
		            disk->journal->size + disk->journal->map_offset,
		            MS_SYNC);
		if (tmp < 0) {
			tmp = errno;
			fprintf(stderr, "Error sync'ing journal: %s\n",
			        strerror(tmp));
			errno = tmp;
			return -1;
		}
	} else {
		struct allocation *alloc = disk->journal->journal_alloc;
		int i;
		for (i = 0; i < alloc->extent_count; i++) {
			size_t size;
			off_t offset;
			size = alloc->extents[i].end_block
			       - alloc->extents[i].start_block + 1;
			size *= EXT2_BLOCK_SIZE(&disk->sb);
			offset = alloc->extents[i].start_block;
			offset *= EXT2_BLOCK_SIZE(&disk->sb);
			tmp = pwrite(disk->fd, base, size, offset);
			if (tmp < 0) {
				tmp = errno;
				fprintf(stderr, "Error writing journal: %s\n",
				        strerror(tmp));
				errno = tmp;
				return -1;
			}
			base += size;
		}
	}
	tmp = fsync(disk->fd); /* fdatasync is probably better here */
	if (tmp < 0) {
		tmp = errno;
		fprintf(stderr, "Error sync'ing journal: %s\n",
		        strerror(tmp));
		errno = tmp;
		return -1;
	}
	trans = disk->journal->transactions;
	return 0;
}

int sync_disk(struct defrag_ctx *c)
{
	int ret;
	struct journal_trans *trans;
	if (c->journal) {
		trans = c->journal->transactions;
		while (trans && trans->transaction_state == TRANS_DONE)
		{
			ptrdiff_t offset;
			c->journal->transactions = trans->next;
			free_transaction(trans);
			trans = c->journal->transactions;
			if (trans && trans->start_block) {
				offset = trans->start_block;
				offset *= c->journal->blocksize;
			} else {
				offset = c->journal->blocksize;
				c->journal->tail = c->journal->map;
				c->journal->tail += offset;
			}
			c->journal->head = (char *)c->journal->map + offset;
		}
		while (trans) {
			if (trans->transaction_state == TRANS_CLOSED)
				trans->transaction_state = TRANS_DSYNC;
			trans = trans->next;
		}
	}
	ret = fsync(c->fd);
	return ret;
}

static int read_gds(struct defrag_ctx *c)
{
	int num_block_groups = c->sb.s_blocks_count / c->sb.s_blocks_per_group;
	if (c->sb.s_blocks_count % c->sb.s_blocks_per_group)
		num_block_groups++;
	size_t map_length = num_block_groups * EXT2_DESC_SIZE(&c->sb);
	off_t gd_offset = EXT2_BLOCK_SIZE(&c->sb);
	int ret;
	/* Map length must be a multiple of the block size, as we always
	 * write whole blocks
	 */
	map_length += EXT2_BLOCK_SIZE(&c->sb) - 1;
	map_length -= map_length % EXT2_BLOCK_SIZE(&c->sb);
	c->gd_map = malloc(map_length);
	if (c->gd_map == NULL)
		return -1;
	if (gd_offset < SUPERBLOCK_OFFSET + SUPERBLOCK_SIZE)
		gd_offset = SUPERBLOCK_OFFSET + SUPERBLOCK_SIZE;
	while (map_length) {
		ret = pread(c->fd, c->gd_map, map_length, gd_offset);
		if (ret <= 0) {
			if (ret == 0)
				errno = ESPIPE;
			ret = errno;
			fprintf(stderr, "Error reading group descriptors: %s\n",
			        strerror(ret));
			free(c->gd_map);
			errno = ret;
			return -1;
		}
		map_length -= ret;
		gd_offset += ret;
	}

	return 0;
}

struct defrag_ctx *open_drive(char *filename)
{
	struct defrag_ctx *ret;
	struct ext2_super_block sb;
	int tmp, fd;

	fd = open(filename, global_settings.simulate ? O_RDONLY : O_RDWR);
	if (fd < 0)
		goto error_out;

	tmp = lseek(fd, SUPERBLOCK_OFFSET, SEEK_SET);
	if (tmp < 0)
		goto error_open;
	tmp = read(fd, &sb, SUPERBLOCK_SIZE);
	if (tmp < SUPERBLOCK_SIZE)
		goto error_open;
	if (sb.s_state != EXT2_VALID_FS) {
		fprintf(stderr, "Filesystem not cleanly umounted,");
		if (global_settings.simulate) {
			fprintf(stderr, " continuing anyway\n");
		} else {
			fprintf(stderr, " aborting\n");
			errno = EIO;
			close(fd);
			return NULL;
		}
	}
	sb.s_state &= ~EXT2_VALID_FS;
	tmp = lseek(fd, SUPERBLOCK_OFFSET, SEEK_SET);
	if (tmp < 0)
		goto error_open;
	tmp = write(fd, &sb, SUPERBLOCK_SIZE);
	if (tmp < SUPERBLOCK_SIZE)
		goto error_open;

	ret = calloc(sizeof(struct defrag_ctx)
	             + sizeof(struct inode *) * sb.s_inodes_count, 1);
	if (!ret)
		goto error_open;
	ret->fd = fd;
	ret->sb = sb;
	ret->extents_by_block = RB_ROOT;
	ret->extents_by_size = RB_ROOT;
	ret->free_tree_by_size = RB_ROOT;
	ret->free_tree_by_block = RB_ROOT;
	tmp = read_gds(ret);
	if (tmp)
		goto error_alloc;
	return ret;

error_alloc:
	free(ret);
error_open:
	close(fd);
error_out:
	return NULL;
}

long parse_inode_table(struct defrag_ctx *c, blk64_t bitmap_block,
                       int group_nr)
{
	unsigned char *bitmap;
	off_t bitmap_start_offset, bitmap_delta_offset;
	size_t bitmap_length;
	unsigned char *inode_table;
	long count = 0;
	const ext2_ino_t first_inode = group_nr * c->sb.s_inodes_per_group;
	ext2_ino_t i;
	int ret;

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

	inode_table = c->inode_map_start;
	inode_table += group_nr * EXT2_INODE_SIZE(&c->sb)
	                        * EXT2_INODES_PER_GROUP(&c->sb);

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
	sync_disk(c);
	c->sb.s_state |= EXT2_VALID_FS;
	if (lseek(c->fd, SUPERBLOCK_OFFSET, SEEK_SET) >= 0) {
		write(c->fd, &c->sb, SUPERBLOCK_SIZE);
		fsync(c->fd);
	}
	free(c->inode_map_start);
	free(c->bitmap);
	free(c->gd_map);
	for (i = 0; i < c->sb.s_inodes_count; i++) {
		if (!c->inodes[i])
			continue;
		if (c->inodes[i]->metadata)
			free(c->inodes[i]->metadata);
		if (c->inodes[i]->data)
			free(c->inodes[i]->data);
		if (c->inodes[i]->num_sparse)
			free(c->inodes[i]->sparse);
		free(c->inodes[i]);
	}

	while (c->free_tree_by_size.rb_node) {
		struct free_extent *f;
		f = rb_entry(c->free_tree_by_size.rb_node,
		             struct free_extent, size_rb);
		rb_erase(c->free_tree_by_size.rb_node, &c->free_tree_by_size);
		free(f);
	}
	close_journal(c);
	unmap_journal(c);
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

long parse_free_bitmap(struct defrag_ctx *c)
{
	struct free_extent *free_extent = NULL;
	struct data_extent *file_extent = NULL;
	blk64_t first_block;
	blk64_t i;
	long count = 0;

	first_block = c->sb.s_first_data_block;
	for (i = 0; i < c->sb.s_blocks_count; i += CHAR_BIT) {
		ptrdiff_t offset;
		int j;
		unsigned char mask = 1;
		offset = i / EXT2_BLOCKS_PER_GROUP(&c->sb);
		offset *= EXT2_BLOCK_SIZE(&c->sb) * CHAR_BIT;
		offset += i % EXT2_BLOCKS_PER_GROUP(&c->sb);
		/* The j is used because we might be at the end of the block
		 * group, so the last few bits might not be valid.
		 */
		j = i % EXT2_BLOCKS_PER_GROUP(&c->sb);
		if (c->bitmap[offset / CHAR_BIT] == 0
		    && j + CHAR_BIT < EXT2_BLOCKS_PER_GROUP(&c->sb))
		{
			if (!free_extent) {
				free_extent=malloc(sizeof(struct free_extent));
				if (!free_extent) {
					fputs("Out of memory allocating data "
					      "on free space", stderr);
					return -1;
				}
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
			if ((first_block + i) % EXT2_BLOCKS_PER_GROUP(&c->sb)
			    + j >= EXT2_BLOCKS_PER_GROUP(&c->sb))
			{
				/* End of block group */
				break;
			}
			if (block >= c->sb.s_blocks_count)
				break;
			if (c->bitmap[offset / CHAR_BIT] & mask) {
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
					if (!free_extent) {
						fputs("Out of memory "
						      "allocating data on free "
						      "space", stderr);
						return -1;
					}
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
	struct ext2_group_desc *gd;
	off_t disk_offset;
	int num_block_groups = (c->sb.s_blocks_count
	                        + c->sb.s_blocks_per_group - 1)
	                       / c->sb.s_blocks_per_group;
	int i_blocks_per_group, i_bytes_per_group;
	int b_blocks_per_group, b_bytes_per_group;
	int i;
	gd = (void *)(c->gd_map);

	i_blocks_per_group = EXT2_INODES_PER_GROUP(&c->sb);
	i_blocks_per_group /= EXT2_INODES_PER_BLOCK(&c->sb);
	i_bytes_per_group = i_blocks_per_group * EXT2_BLOCK_SIZE(&c->sb);

	c->inode_map_start = malloc(i_bytes_per_group * num_block_groups);
	if (!c->inode_map_start)
		return -1;

	b_blocks_per_group = EXT2_BLOCKS_PER_GROUP(&c->sb);
	b_bytes_per_group = b_blocks_per_group / CHAR_BIT;

	c->bitmap = malloc(EXT2_BLOCK_SIZE(&c->sb) * num_block_groups);
	if (!c->bitmap) {
		free(c->inode_map_start);
		return -1;
	}
	for (i = 0; i < num_block_groups; i++) {
		long ret = 0;
		size_t offset, bytes_to_read;
		offset = i_bytes_per_group * i;
		if (gd->bg_flags & EXT2_BG_BLOCK_UNINIT) {
			ret = add_uninit_bg(c, gd, i);
			if (ret)
				return ret;
		}
		if ((gd->bg_flags & (EXT2_BG_INODE_UNINIT))
		    || gd->bg_free_inodes_count == c->sb.s_inodes_per_group){
			memset((char *)(c->inode_map_start) + offset, 0,
			       i_bytes_per_group);
		} else {
			disk_offset = gd->bg_inode_table;
			disk_offset *= EXT2_BLOCK_SIZE(&c->sb);
			bytes_to_read = i_bytes_per_group;
			while (bytes_to_read) {
				ret = pread(c->fd,
				          (char *)(c->inode_map_start) + offset,
				          bytes_to_read, disk_offset);
				if (ret <= 0) {
					ret = errno;
					if (ret == 0)
						ret = ESPIPE;
					fprintf(stderr, "Error reading inode "
					        "table %d: %s\n", i,
					        strerror(ret));
					errno = ret;
					free(c->inode_map_start);
					return -1;
				}
				bytes_to_read -= ret;
				disk_offset += ret;
				offset += ret;
			}
			ret = parse_inode_table(c, gd->bg_inode_bitmap, i);
			if (ret < 0)
				return -1;
		}

		offset = EXT2_BLOCK_SIZE(&c->sb) * (size_t)i;
		disk_offset = gd->bg_block_bitmap;
		disk_offset *= EXT2_BLOCK_SIZE(&c->sb);
		bytes_to_read = EXT2_BLOCK_SIZE(&c->sb);
		while (bytes_to_read) {
			ret = pread(c->fd, c->bitmap + offset,
				    bytes_to_read, disk_offset);
			if (ret <= 0) {
				ret = errno;
				if (ret == 0)
					ret = ESPIPE;
				fprintf(stderr, "Error reading block "
					        "bitmap %d: %s\n", i,
					        strerror(ret));
				errno = ret;
				free(c->inode_map_start);
				free(c->bitmap);
				return -1;
			}
			bytes_to_read -= ret;
			disk_offset += ret;
			offset += ret;
		}
		gd = (void *)((char *)gd + EXT2_DESC_SIZE(&c->sb));
	}
	return parse_free_bitmap(c);
}

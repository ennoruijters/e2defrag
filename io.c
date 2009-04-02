#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include "e2defrag.h"

int read_block(struct defrag_ctx *c, void *buf, blk64_t block)
{
	long long ret;
	printf("Reading block %llu\n", block);
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

struct defrag_ctx *open_drive(char *filename, char read_only)
{
	struct defrag_ctx *ret;
	int tmp;

	ret = malloc(sizeof(struct defrag_ctx));
	if (!ret)
		goto error_out;

	ret->fd = open(filename, read_only ? O_RDONLY : O_RDWR);
	if (ret->fd < 0)
		goto error_alloc;

	tmp = lseek(ret->fd, SUPERBLOCK_OFFSET, SEEK_SET);
	if (tmp < 0)
		goto error_open;
	tmp = read(ret->fd, &ret->sb, SUPERBLOCK_SIZE);
	if (tmp < SUPERBLOCK_SIZE)
		goto error_open;

	ret->owners = calloc(sizeof(*ret->owners), ret->sb.s_blocks_count);
	if (!ret->owners)
		goto error_open;
	return ret;

error_open:
	close(ret->fd);
error_alloc:
	free(ret);
error_out:
	return NULL;
}

long parse_inode_table(struct defrag_ctx *c, blk64_t bitmap_block,
                       blk64_t table_start, int group_nr)
{
	unsigned char bitmap[EXT2_BLOCK_SIZE(&c->sb)];
	unsigned char _inode_table[EXT2_BLOCK_SIZE(&c->sb)];
	void * const inode_table = _inode_table;
	long count = 0;
	const ext2_ino_t first_inode = group_nr * c->sb.s_inodes_per_group;
	int i, ret;
	const char inodes_per_block = EXT2_BLOCK_SIZE(&c->sb)
	                                     / EXT2_INODE_SIZE(&c->sb);

	if (c->sb.s_inodes_per_group % CHAR_BIT) {
		printf("Invalid nr. of inodes per group\n");
		errno = EINVAL;
		return -1;
	}
	if (read_block(c, bitmap, bitmap_block))
		return -1;

	for (i = 0; i < c->sb.s_inodes_per_group; i += CHAR_BIT) {
		int k = i % inodes_per_block;
		if (k == 0) {
			blk64_t table_block;
			table_block = table_start + i / inodes_per_block;
			if (read_block(c, inode_table, table_block))
				return -1;
		}
		int j;
		if (bitmap[i / CHAR_BIT] == 0)
			continue;
		for (j = 0; j < CHAR_BIT; j++) {
			if (bitmap[i / CHAR_BIT] & 1) {
				struct ext2_inode *inode;
				inode = inode_table
					+ (k + j) * EXT2_INODE_SIZE(&c->sb);
				ext2_ino_t inode_nr = first_inode + i + j + 1;
				/* +1 because we start counting inodes at 1
				 * (both for convention and because 0 is the
				 * owner of free space.
				 */
				ret = parse_inode(c, inode_nr, inode);
				if (ret < 0)
					return -1;
				count++;
			}
			bitmap[i / CHAR_BIT] >>= 1;
		}
	}
	return count;
}

long parse_free_bitmap(struct defrag_ctx *c, blk64_t bitmap_block,
                       int group_nr)
{
	unsigned char bitmap[EXT2_BLOCK_SIZE(&c->sb)];
	const blk64_t first_block = group_nr * c->sb.s_blocks_per_group;
	long count = 0;
	int i;

	if (c->sb.s_blocks_per_group % CHAR_BIT) {
		printf("Invalid nr. of blocks per group\n");
		errno = EINVAL;
		return -1;
	}
	if (read_block(c, bitmap, bitmap_block))
		return -1;

	for (i = 0; i < c->sb.s_blocks_per_group; i += CHAR_BIT) {
		int j;
		if (bitmap[i / CHAR_BIT] == 0)
			continue;
		for (j = 0; j < CHAR_BIT; j++) {
			if (first_block + i + j >= c->sb.s_blocks_count)
				break;
			if (bitmap[i / CHAR_BIT] & 1) {
				c->owners[first_block + i + j] = METADATA_INODE;
				count++;
			}
			bitmap[i / CHAR_BIT] >>= 1;
		}
	}
	return count;
}

int set_e2_filesystem_data(struct defrag_ctx *c)
{
	int num_block_groups = (c->sb.s_blocks_count
	                        + c->sb.s_blocks_per_group - 1)
	                       / c->sb.s_blocks_per_group;
	struct ext2_group_desc gds[sizeof(struct ext2_group_desc)
	                           * num_block_groups];
	int ret, i;

	if (lseek(c->fd, EXT2_BLOCK_SIZE(&c->sb), SEEK_SET) == (off_t) -1)
		return -1;
	ret = read(c->fd, gds, sizeof(*gds) * num_block_groups);
	if (ret < sizeof(*gds) * num_block_groups)
		return -1;

	for (i = 0; i < num_block_groups; i++) {
		long ret = 0;
		if (!(gds[i].bg_flags & EXT2_BG_BLOCK_UNINIT))
			ret = parse_free_bitmap(c, gds[i].bg_block_bitmap, i);
		if (ret < 0)
			return -1;
	}

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

#if 0 /* FREE BLOCK DISPLAY */
	int start = 0;
	for (i = 0; i < c->sb.s_blocks_count; i++) {
		if (c->owners[i]) {
			if (start != i)
				printf("%d-%d\n",start,i - 1);
			start = i + 1;
		}
	}
#endif
	return 0;
}

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

/* Block bitmap management functions */

#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include "e2defrag.h"
#include "crc16.h"

/* Returns the block group that has been updated, so that the caller can
 * sync it to disk.
 */
static __u32 __mark_single_block(struct defrag_ctx *c, blk64_t block, char mark)
{
	struct ext2_group_desc *gd;
	off_t offset;
	uint32_t block_group;
	uint16_t crc;
	unsigned char offset_in_byte;
	block_group = block / c->sb.s_blocks_per_group;
	gd = (void *)(c->gd_map + block_group * EXT2_DESC_SIZE(&c->sb));
	offset = (block % c->sb.s_blocks_per_group) / CHAR_BIT;
	offset_in_byte = (block % c->sb.s_blocks_per_group) % CHAR_BIT;
	offset += block_group * EXT2_BLOCK_SIZE(&c->sb);

	if (mark) {
		c->bitmap[offset] |= 1 << offset_in_byte;
		gd->bg_free_blocks_count--;
	} else {
		c->bitmap[offset] &= ~(1 << offset_in_byte);
		gd->bg_free_blocks_count++;
	}
	if (c->sb.s_rev_level != EXT2_GOOD_OLD_REV) {
		char *offset;
		int size;
		crc = crc16(~0, c->sb.s_uuid, sizeof(c->sb.s_uuid));
		crc = crc16(crc, (void *)&block_group, sizeof(block_group));
		crc = crc16(crc, (unsigned char *)gd,
		            offsetof(struct ext4_group_desc, bg_checksum));
		offset = (char *)&(gd->bg_checksum);
		offset += sizeof(crc);
		size = EXT2_DESC_SIZE(&c->sb);
		size -= offsetof(struct ext4_group_desc, bg_checksum);
		size -= sizeof(crc);
		crc = crc16(crc, (void *)offset, size);
		gd->bg_checksum = crc;
	}

	return block_group;
}

static int mark_blocks(struct defrag_ctx *c, blk64_t first_block,
                       e2_blkcnt_t count, char mark)
{
	__u32 to_sync = 0;
	int ret, byte_mask = mark ? UCHAR_MAX : 0;
	char need_sync = 0;
	first_block -= c->sb.s_first_data_block;
	while ((first_block % c->sb.s_blocks_per_group) % CHAR_BIT && count) {
		__u32 tmp;
		tmp = __mark_single_block(c, first_block, mark);
		if (tmp != to_sync && need_sync) {
			ret = write_bitmap_block(c, to_sync);
			if (!ret)
				ret = write_gd(c, to_sync);
			if (ret)
				return ret;
		}
		to_sync = tmp;
		need_sync = 1;
		first_block++;
		count--;
	}
	while (count / CHAR_BIT) {
		struct ext2_group_desc *gd;
		uint32_t block_group = first_block / c->sb.s_blocks_per_group;
		int offset = first_block % c->sb.s_blocks_per_group;
		gd = (void *)(c->gd_map + block_group * EXT2_DESC_SIZE(&c->sb));
		e2_blkcnt_t n = count;
		if (n + offset >= c->sb.s_blocks_per_group)
			n = c->sb.s_blocks_per_group - offset;
		if (n > count)
			n = count;
		n -= n % CHAR_BIT;
		offset /= CHAR_BIT;
		offset += block_group * EXT2_BLOCK_SIZE(&c->sb);
		if (mark)
			gd->bg_free_blocks_count -= n;
		else
			gd->bg_free_blocks_count += n;
		if (c->sb.s_rev_level != EXT2_GOOD_OLD_REV) {
			uint16_t crc;
			char *offset;
			int size;
			crc = crc16(~0, c->sb.s_uuid, sizeof(c->sb.s_uuid));
			crc = crc16(crc, (void *)&block_group,
			            sizeof(block_group));
			crc = crc16(crc, (void *)gd,
			            offsetof(struct ext4_group_desc,
			                     bg_checksum));
			offset = (char *)&(gd->bg_checksum);
			offset += sizeof(crc);
			size = EXT2_DESC_SIZE(&c->sb);
			size -= offsetof(struct ext4_group_desc, bg_checksum);
			size -= sizeof(crc);
			crc = crc16(crc, (void *)offset, size);
			gd->bg_checksum = crc;
		}
		n /= CHAR_BIT;
		memset(c->bitmap + offset, byte_mask, n);
		if (block_group != to_sync && need_sync) {
			ret = write_bitmap_block(c, to_sync);
			if (!ret)
				ret = write_gd(c, to_sync);
			if (ret)
				return ret;
		}
		to_sync = block_group;
		need_sync = 1;
		first_block += n * CHAR_BIT;
		count -= n * CHAR_BIT;
	}
	while (count) {
		__u32 tmp;
		tmp = __mark_single_block(c, first_block, mark);
		if (tmp != to_sync && need_sync) {
			ret = write_bitmap_block(c, to_sync);
			if (!ret)
				ret = write_gd(c, to_sync);
			if (ret)
				return ret;
		}
		to_sync = tmp;
		need_sync = 1;
		first_block++;
		count--;
	}
	if (need_sync) {
		ret = write_bitmap_block(c, to_sync);
		if (!ret)
			ret = write_gd(c, to_sync);
		if (ret)
			return ret;
	}
	return 0;
}

void mark_blocks_used(struct defrag_ctx *c, blk64_t first_block,
                      e2_blkcnt_t count)
{
	mark_blocks(c, first_block, count, 1);
}

void mark_blocks_unused(struct defrag_ctx *c, blk64_t first_block,
                        e2_blkcnt_t count)
{
	mark_blocks(c, first_block, count, 0);
}

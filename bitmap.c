/* Block bitmap management functions */

#include <limits.h>
#include <string.h>
#include "e2defrag.h"

static void __mark_single_block(struct defrag_ctx *c, blk64_t block, char mark)
{
	int block_group = block / c->sb.s_blocks_per_group;
	int offset = block % c->sb.s_blocks_per_group;
	char offset_in_byte = offset % CHAR_BIT;
	offset /= CHAR_BIT;

	if (mark)
		c->bg_maps[block_group].bitmap[offset] |= 1 << offset_in_byte;
	else
		c->bg_maps[block_group].bitmap[offset] &= ~(1 <<offset_in_byte);}

void mark_blocks_used(struct defrag_ctx *c, blk64_t first_block,
                      e2_blkcnt_t count)
{
	first_block -= c->sb.s_first_data_block;
	while ((first_block % c->sb.s_blocks_per_group) % CHAR_BIT && count) {
		__mark_single_block(c, first_block, 1);
		first_block++;
		count--;
	}
	while (count / CHAR_BIT) {
		int block_group = first_block / c->sb.s_blocks_per_group;
		int offset = first_block % c->sb.s_blocks_per_group;
		e2_blkcnt_t n = count;
		if (n + offset >= c->sb.s_blocks_per_group)
			n = c->sb.s_blocks_per_group - offset;
		if (n > count)
		n = count;
		offset /= CHAR_BIT;
		n /= CHAR_BIT;
		memset(c->bg_maps[block_group].bitmap + offset, UCHAR_MAX, n);
		first_block += n * CHAR_BIT;
		count -= n * CHAR_BIT;
	}
	while (count) {
		__mark_single_block(c, first_block, 1);
		first_block++;
		count--;
	}
}

void mark_blocks_unused(struct defrag_ctx *c, blk64_t first_block,
                        e2_blkcnt_t count)
{
	while ((first_block % c->sb.s_blocks_per_group) % CHAR_BIT) {
		__mark_single_block(c, first_block, 0);
		first_block++;
		count--;
	}
	while (count / CHAR_BIT) {
		int block_group = first_block / c->sb.s_blocks_per_group;
		int offset = first_block % c->sb.s_blocks_per_group;
		e2_blkcnt_t n = count;
		if (n + offset >= c->sb.s_blocks_per_group)
			n = c->sb.s_blocks_per_group - offset;
		if (n > count)
		n = count;
		offset /= CHAR_BIT;
		n /= CHAR_BIT;
		memset(c->bg_maps[block_group].bitmap + offset, 0, n);
		first_block += n * CHAR_BIT;
		count -= n * CHAR_BIT;
	}
	while (count) {
		__mark_single_block(c, first_block, 0);
		first_block++;
		count--;
	}
}

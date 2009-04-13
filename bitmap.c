/* Block bitmap management functions */

#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "e2defrag.h"

/* Returns the address of the page which was modified */
static void *__mark_single_block(struct defrag_ctx *c, blk64_t block, char mark)
{
	int block_group = block / c->sb.s_blocks_per_group;
	int offset = block % c->sb.s_blocks_per_group;
	char offset_in_byte = offset % CHAR_BIT;
	offset /= CHAR_BIT;

	if (mark)
		c->bg_maps[block_group].bitmap[offset] |= 1 << offset_in_byte;
	else
		c->bg_maps[block_group].bitmap[offset] &= ~(1 <<offset_in_byte);
	return PAGE_START(&c->bg_maps[block_group].bitmap[offset]);
}

static void mark_blocks(struct defrag_ctx *c, blk64_t first_block,
                        e2_blkcnt_t count, char mark)
{
	void *to_sync = NULL;
	int byte_mask = mark ? UCHAR_MAX : 0;
	first_block -= c->sb.s_first_data_block;
	while ((first_block % c->sb.s_blocks_per_group) % CHAR_BIT && count) {
		void *tmp;
		tmp = __mark_single_block(c, first_block, mark);
		if (tmp != to_sync && to_sync)
			msync(to_sync, getpagesize(), MS_SYNC);
		to_sync = tmp;
		first_block++;
		count--;
	}
	while (count / CHAR_BIT) {
		void *tmp;
		int block_group = first_block / c->sb.s_blocks_per_group;
		int offset = first_block % c->sb.s_blocks_per_group;
		e2_blkcnt_t n = count;
		if (n + offset >= c->sb.s_blocks_per_group)
			n = c->sb.s_blocks_per_group - offset;
		if (n > count)
		n = count;
		offset /= CHAR_BIT;
		n /= CHAR_BIT;
		memset(c->bg_maps[block_group].bitmap + offset, byte_mask, n);
		tmp = PAGE_START(c->bg_maps[block_group].bitmap + offset);
		if (tmp != to_sync && to_sync)
			msync(to_sync, getpagesize(), MS_SYNC);
		to_sync = tmp;
		tmp = PAGE_START(c->bg_maps[block_group].bitmap + offset + n);
		if (tmp != to_sync) {
			int bytes_to_sync = (char *)tmp - (char *)to_sync;
			bytes_to_sync += getpagesize();
			msync(to_sync, bytes_to_sync, MS_SYNC);
			to_sync = NULL;
		}
		first_block += n * CHAR_BIT;
		count -= n * CHAR_BIT;
	}
	while (count) {
		void *tmp;
		tmp = __mark_single_block(c, first_block, mark);
		first_block++;
		count--;
		if (tmp != to_sync && to_sync)
			msync(to_sync, getpagesize(), MS_SYNC);
		to_sync = tmp;
	}
	if (to_sync)
		msync(to_sync, getpagesize(), MS_SYNC);
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
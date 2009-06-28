#include <obstack.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include "e2defrag.h"
#include "extree.h"

/* Very naive algorithm for now: Just try to find a combination of free
 * extent big enough to fit the whole file, but consisting of fewer extents
 * than the current one.
 * Returns 0 for success, 1 for no opportunity, -1 for error.
 */
int do_one_inode(struct defrag_ctx *c, ext2_ino_t inode_nr, int silent)
{
	struct inode *inode;
	struct allocation *target;
	int ret;

	inode = c->inodes[inode_nr];
	errno = 0;
	target = allocate_blocks(c, inode->block_count, inode_nr, 0);
	if (!target) {
		if (errno)
			return -1;
		else
			return 1;
	}
	if (target->extent_count >= inode->extent_count) {
		if (!silent)
			printf("No better placement possible: best new placement has %llu fragments\n", target->extent_count);
		ret = deallocate_blocks(c, target);
		free(target);
		if (ret < 0)
			return ret;
		else
			return 1;
	}
	ret = move_inode_data(c, inode, target);
	free(target);
	return ret;
}

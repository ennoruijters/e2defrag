#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include "e2defrag.h"

const __u32 KNOWN_INODE_FLAGS_MASK = 0x000FFFFFU;

long parse_inode(struct defrag_ctx *c, ext2_ino_t inode_nr,
                 struct ext2_inode *inode)
{
	if (inode_nr < EXT2_FIRST_INO(&c->sb)) {
		if (inode_nr != EXT2_ROOT_INO && inode_nr != EXT2_JOURNAL_INO)
			return 0;
	}
	if (inode->i_flags & EXT4_EXTENTS_FL) {
		printf("Inode %u uses extents. I don't know how to handle "
		       "those, so I'm ignoring it.\n", inode_nr);
		return 0;
	}
	if (inode->i_flags - (inode->i_flags & KNOWN_INODE_FLAGS_MASK)) {
		printf("Inode %u has unknown flags %x. Ignoring the inode\n",
		       inode_nr,
		       inode->i_flags & ~KNOWN_INODE_FLAGS_MASK);
		return 0;
	}
	printf("Inode %u: %u blocks\n", inode_nr, inode->i_blocks);
	return 0;
}

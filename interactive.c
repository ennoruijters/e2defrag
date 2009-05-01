#include <stdio.h>
#include "e2defrag.h"
#include "extree.h"

int move_extent_interactive(struct defrag_ctx *c)
{
	struct inode *inode;
	blk64_t old_start, new_block;
	ext2_ino_t inode_nr;
	int i;
	do {
		printf("Specify inode number (or -1 for a dump, or 0 to exit): ");
		scanf("%u", &inode_nr);
		if (inode_nr == 0)
			return 0;
		if (inode_nr == -1)
			dump_trees(c, 2);
	} while (inode_nr < 11 || inode_nr > c->sb.s_inodes_count);
	inode = c->inodes[inode_nr];
	if (inode == NULL || inode->extent_count == 0) {
		printf("Inode has no data associated\n");
		return 0;
	}
	printf("Data extents:\n");
	for (i = 0; i < inode->extent_count; i++) {
		printf("%d: %llu-%llu (%llu)\n", i + 1,
		       inode->extents[i].start_block,
		       inode->extents[i].end_block,
		       inode->extents[i].start_logical);
	}
	do {
		printf("Specify extent number (or 0 to exit): ");
		scanf("%d", &i);
		if (i == 0)
			return 0;
		i--;
	} while (i < 0 || i >= inode->extent_count);
	old_start = inode->extents[i].start_logical;

	do {
		printf("Enter new starting block (or 0 for dump, 1 to exit): ");
		scanf("%llu", &new_block);
		if (new_block == 0)
			dump_trees(c, 1);
		if (new_block == 1)
			return 0;
	} while (new_block <= 0 || new_block > c->sb.s_blocks_count);

	i = move_file_extent(c, inode, old_start, new_block);
	if (i)
		return i;
#ifndef NDEBUG
	dump_trees(c, 3);
#endif
	return 0;
}

int move_file_interactive(struct defrag_ctx *c)
{
	struct inode *inode;
	blk64_t destination;
	ext2_ino_t inode_nr;
	int i;
	do {
		printf("Specify inode number (or -1 for a dump, or 0 to exit): ");
		scanf("%u", &inode_nr);
		if (inode_nr == 0)
			return 0;
		if (inode_nr == -1)
			dump_trees(c, 2);
	} while (inode_nr < 11 || inode_nr > c->sb.s_inodes_count);
	inode = c->inodes[inode_nr];
	if (inode == NULL || inode->extent_count == 0) {
		printf("Inode has no data associated\n");
		return 0;
	}
	printf("Old data extents:\n");
	for (i = 0; i < inode->extent_count; i++) {
		printf("%d: %llu-%llu (%llu)\n", i + 1,
		       inode->extents[i].start_block,
		       inode->extents[i].end_block,
		       inode->extents[i].start_logical);
	}

	do {
		printf("Enter new starting block (or 0 for dump, 1 to exit): ");
		scanf("%llu", &destination);
		if (destination == 0)
			dump_trees(c, 1);
		if (destination == 1)
			return 0;
	} while (destination <= 0 || destination > c->sb.s_blocks_count);

	i = move_file_data(c, inode_nr, destination);
	if (i)
		return i;
#ifndef NDEBUG
	dump_trees(c, 3);
#endif
	return 0;
}

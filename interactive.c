#include <stdio.h>
#include "e2defrag.h"
#include "extree.h"

extern int do_one_inode(struct defrag_ctx *c, ext2_ino_t);

static void print_fragged_inodes(const struct defrag_ctx *c)
{
	ext2_ino_t i;
	ext2_ino_t nr_inodes = ext2_inodes_on_disk(&c->sb);
	for (i = 0; i < nr_inodes; i++) {
		const struct inode *inode = c->inodes[i];
		if (!inode)
			continue;
		if (inode->extent_count > 1)
			printf("Inode %u: %llu fragments (%llu blocks)\n",
			       i, inode->extent_count, inode->block_count);
		if (inode->metadata && inode->metadata->extent_count > 1)
			printf("Metadata for inode %u: %llu fragments "
			       "(%llu blocks)\n",
			       i, inode->metadata->extent_count,
			       inode->metadata->block_count);
	}
}

int defrag_file_interactive(struct defrag_ctx *c)
{
	struct inode *inode;
	ext2_ino_t inode_nr;

	print_fragged_inodes(c);
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
	return do_one_inode(c, inode_nr);
}

int move_file_interactive(struct defrag_ctx *c)
{
	struct inode *inode;
	blk64_t destination;
	ext2_ino_t inode_nr;
	int i;

	print_fragged_inodes(c);
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

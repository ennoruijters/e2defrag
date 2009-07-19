#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "e2defrag.h"
#include "extree.h"

extern int do_one_inode(struct defrag_ctx *c, ext2_ino_t, int);

static void print_fragged_inodes(const struct defrag_ctx *c)
{
	ext2_ino_t i;
	ext2_ino_t nr_inodes = ext2_inodes_on_disk(&c->sb);
	for (i = 0; i < nr_inodes; i++) {
		const struct inode *inode = c->inodes[i];
		if (!inode)
			continue;
		if (inode->extent_count > 1)
			printf("Inode %u%s: %llu fragments (%llu blocks)\n",
			       i, inode->metadata ? "" : "*",
			       inode->extent_count, inode->block_count);
		if (inode->metadata && inode->metadata->extent_count > 1)
			printf("Metadata for inode %u: %llu fragments "
			       "(%llu blocks)\n",
			       i, inode->metadata->extent_count,
			       inode->metadata->block_count);
	}
}

int defrag_file_interactive(struct defrag_ctx *c)
{
	struct rb_node *n = rb_last(&c->free_tree_by_size);
	struct free_extent *f = rb_entry(n, struct free_extent, size_rb);
	struct inode *inode;
	e2_blkcnt_t biggest_size = 0;
	int ret, num_biggest = 0;
	ext2_ino_t inode_nr;

	print_fragged_inodes(c);
	if (n)
		biggest_size = f->end_block - f->start_block + 1;
	while (n && biggest_size == f->end_block - f->start_block + 1) {
		num_biggest++;
		n = rb_prev(n);
		f = rb_entry(n, struct free_extent, size_rb);
	}
	printf("Biggest free space: %llu blocks (%d)\n", biggest_size,
	       num_biggest);
	do {
		printf("Specify inode number (or -1 for free space consolidation, or 0 to exit): ");
		scanf("%u", &inode_nr);
		if (inode_nr == 0)
			return 1;
		if (inode_nr == -1) {
			ret = consolidate_free_space(c, 0);
			if (ret)
				printf("Error: %s\n", strerror(errno));
			if (ret && errno != ENOSPC)
				return ret;
			else
				return 0;
		}
		if (inode_nr == -2) {
			ret = 0;
			while (!ret)
				ret = consolidate_free_space(c, 0);
			if (errno == ENOSPC)
				return 0;
			return ret;
		}
	} while (inode_nr < 11 || inode_nr > c->sb.s_inodes_count);
	inode = c->inodes[inode_nr];
	if (inode == NULL || inode->extent_count == 0) {
		printf("Inode has no data associated\n");
		return 0;
	}
	ret = do_one_inode(c, inode_nr, 0);
	if (ret < 0)
		printf("Error: %s\n", strerror(errno));
	printf("Inode now has %llu fragments\n",
	       c->inodes[inode_nr]->extent_count);
	if (ret && errno != ENOSPC)
		return ret;
	else
		return 0;
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

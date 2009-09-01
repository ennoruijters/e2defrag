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

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include "e2defrag.h"
#include "extree.h"

static int getnumber(long *ret, long min, long max)
{
	char *endptr;
	long tmp;
	char input[(sizeof(long) * CHAR_BIT) / 3 + 3];
	/* Definitely large enough to hold LONG_MAX and LONG_MIN */

	if (fgets(input, sizeof(input), stdin) == NULL)
		return -1;
	if (input[strlen(input) - 1] != '\n') {
		do {
			tmp = getchar();
		} while (tmp != '\n' && tmp != EOF);
	} else {
		input[strlen(input) - 1] = '\0';
	}
	*ret = strtol(input, &endptr, 10);
	if ((*ret == LONG_MAX || *ret == LONG_MIN) && errno == ERANGE)
		return -1;
	if (*ret > max || *ret < min) {
		errno = ERANGE;
		return -1;
	}
	if (*endptr != '\0') {
		errno = EDOM;
		return -1;
	}
	return 0;
}

static void print_fragged_inodes(const struct defrag_ctx *c)
{
	ext2_ino_t i;
	ext2_ino_t nr_inodes = ext2_inodes_on_disk(&c->sb);
	e2_blkcnt_t free_blocks = 0;
	long free_extents = 0;
	struct rb_node *n;

	n = rb_first(&c->free_tree_by_block);
	while (n) {
		struct free_extent *extent;
		extent = rb_entry(n, struct free_extent, block_rb);
		free_extents++;
		free_blocks += extent->end_block - extent->start_block + 1;
		n = rb_next(n);
	}
	printf("Free space: %ld fragments (%llu blocks)\n",
	       free_extents, free_blocks);

	for (i = 0; i < nr_inodes; i++) {
		const struct inode *inode = c->inodes[i];
		if (!inode)
			continue;
		if (inode->data->extent_count > 1) {
			printf("Inode %u%s: %llu fragments (%llu blocks)\n",
			       i, inode->metadata ? "" : "*",
			       inode->data->extent_count,
			       inode->data->block_count);
		}
		if (inode->metadata && inode->metadata->extent_count > 1) {
			printf("Metadata for inode %u: %llu fragments "
			       "(%llu blocks)\n",
			       i, inode->metadata->extent_count,
			       inode->metadata->block_count);
		}
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
	char improve;

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
		long number;
		printf("WARNING: UNTESTED!\n");
		printf("Specify inode number.\n");
		printf("You can enter -1 for free space consolidation, or 0 to exit.\n");
		printf("You can also precede the inode number by an 'i' to improve rather than defragment the file.\n");
		printf("Inode number: ");
		fflush(stdout);
		number = getchar();
		if (number == 'i' || number == 'I') {
			improve = 1;
		} else {
			improve = 0;
			ungetc(number, stdin);
		}
		if (getnumber(&number, -2, c->sb.s_inodes_count) < 0) {
			switch (errno) {
			case EDOM:
				printf("Not a valid number\n");
				break;
			case ERANGE:
				printf("Inode number out of range\n");
				break;
			default:
				printf("Error: %s\n", strerror(errno));
				return -1;
			}
			number = LONG_MIN;
		}
		switch (number) {
		case 0:
			return 1;
		case -1:
			ret = consolidate_free_space(c);
			if (ret)
				printf("Error: %s\n", strerror(errno));
			if (ret && errno != ENOSPC)
				return ret;
			return 0;
		case -2:
			ret = 0;
			while (!ret)
				ret = consolidate_free_space(c);
			if (errno == ENOSPC)
				return 0;
			return ret;
		case LONG_MIN:
			inode_nr = 0;
			break;
		default:
			inode_nr = number;
		}
	} while (inode_nr < EXT2_FIRST_INO(&c->sb));
	inode = c->inodes[inode_nr];
	if (inode == NULL || inode->data->extent_count == 0) {
		printf("Inode has no data associated\n");
		return 0;
	}
	if (!improve)
		ret = do_one_inode(c, inode_nr);
	else
		ret = try_improve_inode(c, inode_nr);
	if (ret < 0)
		printf("Error: %s\n", strerror(errno));
	printf("Inode now has %llu fragments\n",
	       c->inodes[inode_nr]->data->extent_count);
	if (ret && errno != ENOSPC)
		return ret;
	else
		return 0;
}

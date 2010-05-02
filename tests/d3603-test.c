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

#include <e2defrag.h>
#include <jbd2.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

struct settings global_settings = {
	.simulate = 0,
	.interactive = 0,
};

int main(int argc, char *argv[])
{
	blk64_t block_nr;
	struct defrag_ctx *disk;
	struct journal_superblock_s *sb;
	journal_trans_t *transaction;
	struct data_extent *cur_extent;
	char *filename;
	char *data;
	int ret, block_count = 0, blocks_to_write;

	if (argc != 2) {
		fprintf(stderr, "Usage: test <filename>\n");
		return EXIT_FAILURE;
	}
	filename = argv[1];
	disk = open_drive(filename);
	if (!disk) {
		fprintf(stderr, "Error opening drive: %s\n", strerror(errno));
		return errno;
	}
	ret = set_e2_filesystem_data(disk);
	if (ret < 0) {
		fprintf(stderr, "Error reading filesystem: %s\n",
		        strerror(errno));
		return errno;
	}
	ret = journal_init(disk);
	if (ret < 0)
		return EXIT_FAILURE;
	sb = disk->journal->map;
	fprintf(stderr, "Max. transaction size: %d\n", disk->journal->max_trans_blocks);
	transaction = start_transaction(disk);
	data = malloc(EXT2_BLOCK_SIZE(&disk->sb));
	if (!data) {
		fprintf(stderr, "Out of memory\n");
		return ENOMEM;
	}
	memset(data, 0xff, EXT2_BLOCK_SIZE(&disk->sb));
	blocks_to_write = (disk->journal->size - 1) / disk->journal->blocksize;
	/* Create a transaction 3/4 journal size */
	blocks_to_write = (3 * blocks_to_write) / 4;
	cur_extent = disk->inodes[12]->data->extents;
	block_nr = cur_extent->start_block;
	while (block_count < blocks_to_write) {
		ret = journal_write_block(transaction, block_nr, data);
		if (ret < 0)
			return errno;
		block_count++;
		if (block_nr == cur_extent->end_block) {
			if (block_count < blocks_to_write) {
				cur_extent++;
				block_nr = cur_extent->start_block;
			}
		} else {
			block_nr++;
		}
	}
	finish_transaction(transaction);
	ret = sync_disk(disk);
	if (ret < 0)
		return errno;
	ret = flush_journal(disk);
	if (ret < 0)
		return errno;
	return 0;
}

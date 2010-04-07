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
	struct defrag_ctx *disk;
	struct journal_superblock_s *sb;
	journal_trans_t *transaction;
	char *filename;
	FILE *tmp;
	char *old_data, *data;
	int ret;

	if (argc != 3) {
		fprintf(stderr, "Usage: test <filename> <data output>\n");
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
	sb->s_feature_compat |= htobe32(JBD2_FEATURE_COMPAT_CHECKSUM);
	disk->journal->flags |= FLAG_JOURNAL_CHECKSUM;
	transaction = start_transaction(disk);
	data = malloc(EXT2_BLOCK_SIZE(&disk->sb));
	if (!data) {
		fprintf(stderr, "Out of memory\n");
		return ENOMEM;
	}
	old_data = malloc(EXT2_BLOCK_SIZE(&disk->sb));
	if (!old_data) {
		fprintf(stderr, "Out of memory\n");
		return ENOMEM;
	}
	ret = read_block(disk, old_data,
	                 disk->inodes[12]->data->extents[0].start_block);
	if (ret < 0)
		return errno;
	memset(data, 0xff, EXT2_BLOCK_SIZE(&disk->sb));
	ret = journal_write_block(transaction,
	                          disk->inodes[12]->data->extents[0].start_block,
	                          data);
	if (ret < 0)
		return errno;
	finish_transaction(transaction);
	ret = sync_disk(disk);
	if (ret < 0)
		return errno;
	ret = flush_journal(disk);
	if (ret < 0)
		return errno;
	ret = write_block(disk, old_data,
	                 disk->inodes[12]->data->extents[0].start_block);
	if (ret < 0)
		return errno;
	tmp = fopen(argv[2], "wb");
	fwrite(data, EXT2_BLOCK_SIZE(&disk->sb), 1, tmp);
	fclose(tmp);
	return 0;
}

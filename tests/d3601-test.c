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
	journal_trans_t *transaction;
	char *filename;
	FILE *tmp;
	uint32_t *data;
	blk64_t block;
	int ret, count, total_transactions;

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
	data = malloc(EXT2_BLOCK_SIZE(&disk->sb));
	if (!data) {
		fprintf(stderr, "Out of memory\n");
		return ENOMEM;
	}
	memset(data, 0x0, EXT2_BLOCK_SIZE(&disk->sb));
	total_transactions = disk->journal->size / disk->journal->blocksize - 1;
	total_transactions = total_transactions / 3 + 2;
	block = disk->inodes[12]->data->extents[0].start_block;
	for (count = 0; count < total_transactions; count++) {
		transaction = start_transaction(disk);
		data[0]++;
		ret = journal_write_block(transaction, block, data);
		if (ret < 0)
			return errno;
		finish_transaction(transaction);
	}
	tmp = fopen(argv[2], "wb");
	fwrite(data, EXT2_BLOCK_SIZE(&disk->sb), 1, tmp);
	fclose(tmp);
	ret = read_block(disk, data, block);
	if (ret < 0)
		return errno;
	ret = sync_disk(disk);
	if (ret < 0)
		return errno;
	ret = flush_journal(disk);
	if (ret < 0)
		return errno;
	ret = write_block(disk, data, block);
	if (ret < 0)
		return errno;
	return 0;
}

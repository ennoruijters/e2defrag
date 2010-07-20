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

/* Journal initialization functions */

#define _XOPEN_SOURCE 600
#define _BSD_SOURCE
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <endian.h>
#include "e2defrag.h"
#include "jbd2.h"

int unmap_journal(struct defrag_ctx *disk)
{
	char *base;
	size_t length;
	int ret_errno = 0, tmp;
	if (!disk->journal || !disk->journal->map)
		return 0;
	base = disk->journal->map;
	base -= disk->journal->map_offset;
	length = disk->journal->size + disk->journal->map_offset;
	tmp = munmap(base, length);
	if (tmp) {
		ret_errno = errno;
		fprintf(stderr, "Error unmapping journal: %s\n",
		        strerror(ret_errno));
	}
	free(disk->journal);
	disk->journal = NULL;
	errno = ret_errno;
	if (errno)
		return -1;
	if (global_settings.simulate)
		return 0;
	/* The following should really go in some kind of close_journal
	 * functions
	 */
	disk->sb.s_feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
	tmp = pwrite(disk->fd, &disk->sb, SUPERBLOCK_SIZE, SUPERBLOCK_OFFSET);
	if (tmp < SUPERBLOCK_SIZE)
		return -1;
	return 0;
}

/* Returns 0 for success, positive (errno) for failure */
static int read_journal(struct defrag_ctx *disk)
{
	struct allocation *alloc = disk->journal->journal_alloc;
	char *base = disk->journal->map;
	int i, tmp;
	base -= disk->journal->map_offset;
	for (i = 0; i < alloc->extent_count; i++) {
		size_t size;
		off_t offset;
		size = alloc->extents[i].end_block
		       - alloc->extents[i].start_block;
		size *= EXT2_BLOCK_SIZE(&disk->sb);
		offset = alloc->extents[i].start_block;
		offset *= EXT2_BLOCK_SIZE(&disk->sb);
		tmp = pread(disk->fd, base, size, offset);
		if (tmp < 0) {
			tmp = errno;
			fprintf(stderr, "Error writing journal: %s\n",
				strerror(tmp));
			errno = tmp;
			return errno;
		}
		base += size;
	}
	return 0;
}

static int map_journal(struct defrag_ctx *disk)
{
	struct inode *ino;
	char *map;
	size_t size;
	int offset_diff, ret_errno;
	disk->journal->journal_alloc = NULL;
       	ino = read_inode(disk, EXT2_JOURNAL_INO);
	if (ino == NULL) {
		fprintf(stderr, "Journal inode corrupt\n");
		errno = EINVAL;
		return -1;
	}
	if (ino->data->extent_count == 1) {
		off_t offset;
		offset = ino->data->extents[0].start_block;
		printf("Mmapping whole range from %lu ", offset);
		offset *= EXT2_BLOCK_SIZE(&disk->sb);
		offset_diff = offset % sysconf(_SC_PAGE_SIZE);
		size = ino->data->block_count * EXT2_BLOCK_SIZE(&disk->sb);
		printf("(%lu blocks)\n", size / EXT2_BLOCK_SIZE(&disk->sb));
		map = mmap(NULL,
		           size + offset_diff,
		           PROT_READ | PROT_WRITE,
		           global_settings.simulate ? MAP_PRIVATE : MAP_SHARED,
			   disk->fd,
			   offset - offset_diff);
		free(ino->data);
		if (map == MAP_FAILED) {
			ret_errno = errno;
			fprintf(stderr, "mmapping journal failed: %s\n",
			        strerror(ret_errno));
			goto out_free_inode;
		}
		map += offset_diff;
	} else {
		struct allocation *data = ino->data;
		char *next_addr, *tmp;
		off_t offset;
		e2_blkcnt_t num_blocks = 0;
		int blocks_per_page = sysconf(_SC_PAGE_SIZE)
		                      / EXT2_BLOCK_SIZE(&disk->sb);
		int i, map_flags;
		num_blocks = data->block_count;
		/* First map an anonymous mapping big enough for the whole
		 * journal so we get an appropriate address, and a fall-back
		 * mapping in case we can't map the journal proper.
		 */
		size = num_blocks * EXT2_BLOCK_SIZE(&disk->sb);
		offset_diff = 0;
		map = mmap(NULL, size, PROT_READ | PROT_WRITE,
		           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (map == MAP_FAILED) {
			ret_errno = errno;
			fprintf(stderr, "Error allocating memory for journal: "
			        "%s\n", strerror(ret_errno));
			free(ino->data);
			goto out_free_inode;
		}

		/* Check that all extents of the journal are page-aligned */
		for (i = 0; i < data->extent_count - 1; i++) {
			if (data->extents[i].start_block % blocks_per_page
			    || data->extents[i].end_block % blocks_per_page)
			{
				disk->journal->journal_alloc = data;
				disk->journal->map = map;
				disk->journal->size = size;
				disk->journal->map_offset = offset_diff;
				ret_errno = read_journal(disk);
				if (ret_errno) {
					free(ino->data);
					munmap(map,
				               num_blocks *
					            EXT2_BLOCK_SIZE(&disk->sb));
				}
				goto out_free_inode;
			}
		}
		/* Last extent end need not be aligned, as we the mmap
		 * length doesn't have to be aligned.
		 */
		if (data->extents[i].start_block % blocks_per_page) {
			disk->journal->journal_alloc = data;
			disk->journal->map = map;
			disk->journal->size = size;
			disk->journal->map_offset = offset_diff;
			ret_errno = read_journal(disk);
			if (ret_errno) {
				free(ino->data);
				munmap(map,
				       num_blocks * EXT2_BLOCK_SIZE(&disk->sb));
			}
			goto out_free_inode;
		}

		/* And we map the actual journal extents over the anonymous
		 * mapping using MAP_FIXED so we can pretent everything
		 * is contiguous
		 */
		offset = data->extents[0].start_block;
		offset *= EXT2_BLOCK_SIZE(&disk->sb);
		offset_diff = offset % sysconf(_SC_PAGE_SIZE);
		size = data->extents->end_block - data->extents->start_block;
		size *= EXT2_BLOCK_SIZE(&disk->sb);
		map_flags = (global_settings.simulate ? MAP_PRIVATE
		                                      : MAP_SHARED
		            ) && MAP_FIXED;
		tmp = mmap(map, size + offset_diff, PROT_READ | PROT_WRITE,
			   map_flags, disk->fd, offset - offset_diff);
		if (tmp == MAP_FAILED) {
			ret_errno = errno;
			fprintf(stderr, "Error mmapping journal: %s\n",
			        strerror(ret_errno));
			munmap(map, num_blocks * EXT2_BLOCK_SIZE(&disk->sb));
			free(ino->data);
			goto out_free_inode;
		}
		map += offset_diff;
		next_addr = map + size;
		for (i = 1; i < data->extent_count; i++) {
			offset = data->extents[i].start_block;
			offset *= EXT2_BLOCK_SIZE(&disk->sb);
			size = data->extents[i].end_block
			       - data->extents[i].start_block;
			size *= EXT2_BLOCK_SIZE(&disk->sb);
			tmp = mmap(next_addr, size,
			           PROT_READ | PROT_WRITE, map_flags,
				   disk->fd, offset);
			if (tmp == MAP_FAILED) {
				ret_errno = errno;
				fprintf(stderr, "Error mmapping journal: %s\n",
					strerror(ret_errno));
				munmap(map,
				       num_blocks * EXT2_BLOCK_SIZE(&disk->sb));
				free(ino->data);
				goto out_free_inode;
			}
			next_addr += size;
		}
		free(ino->data);
		size = num_blocks * EXT2_BLOCK_SIZE(&disk->sb);
	}
	disk->journal->map = map;
	disk->journal->size = size;
	disk->journal->map_offset = offset_diff;
	ret_errno = 0;
out_free_inode:
	free(ino->metadata);
	free(ino->sparse);
	free(ino);
	if (ret_errno)
		free(disk->journal);
	errno = ret_errno;
	if (ret_errno)
		return -1;
	else
		return 0;
}

static int journal_init_v1(struct defrag_ctx *disk)
{
	struct journal_superblock_s *sb = disk->journal->map;
	disk->journal->tag_size = JBD2_TAG_SIZE32;
	if (be32toh(sb->s_start != 0)) {
		fprintf(stderr, "Journal is not clean, run e2fsck\n");
		errno = EBUSY;
		return -1;
	}
	disk->journal->head = disk->journal->map;
	disk->journal->head += disk->journal->blocksize;
	disk->journal->tail = disk->journal->head;

	disk->journal->max_trans_blocks = be32toh(sb->s_maxlen) / 4;
	disk->journal->next_sequence = be32toh(sb->s_sequence) + 1;
	disk->journal->flags = 0;
	return 0;
}

static int journal_init_v2(struct defrag_ctx *disk)
{
	int tags_per_block, max_trans_data;
	struct journal_superblock_s *sb = disk->journal->map;
	if (sb->s_feature_incompat & htobe32(JBD2_FEATURE_INCOMPAT_64BIT))
		disk->journal->tag_size = JBD2_TAG_SIZE64;
	else
		disk->journal->tag_size = JBD2_TAG_SIZE32;
	if (be32toh(sb->s_start != 0)) {
		fprintf(stderr, "Journal is not clean, run e2fsck\n");
		errno = EBUSY;
		return -1;
	}
	disk->journal->head = disk->journal->map;
	disk->journal->head += disk->journal->blocksize;
	disk->journal->tail = disk->journal->head;
	disk->journal->next_sequence = be32toh(sb->s_sequence) + 1;

	if (sb->s_max_transaction != 0) {
		tags_per_block = disk->journal->blocksize
		                 - sizeof(journal_header_t);
		tags_per_block -= JBD2_UUID_BYTES;
		tags_per_block /= sizeof(journal_block_tag_t);

		max_trans_data = be32toh(sb->s_max_transaction) - 1;
		/* -1 for commit block */
		max_trans_data -= (max_trans_data + tags_per_block - 1)
		                  / tags_per_block;
	} else {
		max_trans_data = be32toh(sb->s_maxlen) / 4;
	}

	disk->journal->flags = 0;
	if (sb->s_feature_incompat & htobe32(JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT))
	{
		disk->journal->flags |= FLAG_ASYNC_COMMIT;
	}
	if (sb->s_feature_compat & htobe32(JBD2_FEATURE_COMPAT_CHECKSUM))
		disk->journal->flags |= FLAG_JOURNAL_CHECKSUM;


	if (sb->s_max_trans_data != 0
	    && max_trans_data > be32toh(sb->s_max_trans_data))
	{
		disk->journal->max_trans_blocks = be32toh(sb->s_max_trans_data);
	} else {
		disk->journal->max_trans_blocks = max_trans_data;
	}

	return 0;
}

int journal_init(struct defrag_ctx *disk)
{
	int ret;
	disk->journal = NULL;
	if (EXT2_HAS_COMPAT_FEATURE(&disk->sb, EXT3_FEATURE_COMPAT_HAS_JOURNAL))
	{
		struct journal_superblock_s *sb;
		disk->journal = malloc(sizeof(*disk->journal));
		if (!disk->journal) {
			fprintf(stderr, "Out of memory reading journal\n");
			errno = ENOMEM;
			return -1;
		}
		ret = map_journal(disk);
		if (ret)
			return ret;
		sb = disk->journal->map;
		if (be32toh(sb->s_header.h_magic) != JBD2_MAGIC_NUMBER) {
			fprintf(stderr,
			        "Journal superblock magic value wrong\n");
			unmap_journal(disk);
			errno = EIO;
			return -1;
		}
		if (be32toh(sb->s_maxlen) * be32toh(sb->s_blocksize)
		    != disk->journal->size)
		{
			fprintf(stderr, "Journal inode and superblock "
			        "disagree about journal size\n");
			fprintf(stderr, "inode: %lu bytes, sb: %u bytes\n",
			        disk->journal->size,
			        be32toh(sb->s_maxlen) * be32toh(sb->s_blocksize));
			unmap_journal(disk);
			errno = EIO;
			return -1;
		}
		disk->journal->blocksize = be32toh(sb->s_blocksize);
		disk->journal->transactions = NULL;
		disk->journal->last_transaction = NULL;
		disk->sb.s_feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
		if (!global_settings.simulate) {
			ret = pwrite(disk->fd, &disk->sb, SUPERBLOCK_SIZE,
			             SUPERBLOCK_OFFSET);
			if (ret < SUPERBLOCK_SIZE)
				return -1;
		}

		switch(be32toh(sb->s_header.h_blocktype)) {
		case JBD2_SUPERBLOCK_V1:
			return journal_init_v1(disk);
		case JBD2_SUPERBLOCK_V2:
			return journal_init_v2(disk);
		default:
			fprintf(stderr,
			        "Journal superblock type not recognized\n");
			unmap_journal(disk);
			return -1;
		}
	}
	return 0;
}

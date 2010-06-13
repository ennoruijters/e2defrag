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

/* Journal management functions */

#define _XOPEN_SOURCE 600
#define _BSD_SOURCE
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <endian.h>
#include <assert.h>
#include <sys/mman.h>
#include "e2defrag.h"
#include "jbd2.h"

struct journal_trans {
	struct journal_trans *next;
	struct defrag_ctx *ctx;
	struct writeout_block {
		struct writeout_block *next;
		blk64_t block_nr;
		unsigned data[];
	} *writeout_blocks;
	/* A protected extent is one which must not be changed before this
	 * transaction has been comitted.
	 */
	struct protected_extent {
		struct protected_extent *next;
		blk64_t start_block;
		blk64_t end_block;
	} *protected_extents;
	
	/* The in-journal block at which the transaction starts */
	__u32 start_block;

	__u32 num_writeout_blocks;
	int transaction_state;
};

struct journal_trans no_journal_trans = {
	.ctx = NULL,
	.transaction_state = TRANS_ACTIVE
};

/* Returns a new, active transaction placed at the end of the superblock's
 * transaction list.
 */
journal_trans_t *start_transaction(struct defrag_ctx *c)
{
	struct journal_trans *ret;
	if (!c->journal) {
		if (no_journal_trans.ctx == NULL)
			no_journal_trans.ctx = c;
		assert(no_journal_trans.ctx == c);
		return &no_journal_trans;
	}
	ret = malloc(sizeof(struct journal_trans));
	if (!ret) {
		fprintf(stderr, "Out of memory allocating transaction\n");
		errno = ENOMEM;
		return NULL;
	}
	ret->writeout_blocks = NULL;
	ret->protected_extents = NULL;
	ret->transaction_state = TRANS_ACTIVE;
	ret->ctx = c;
	ret->start_block = 0;
	ret->num_writeout_blocks = 0;
	ret->next = NULL;
	if (c->journal->last_transaction)
		c->journal->last_transaction->next = ret;
	c->journal->last_transaction = ret;
	if (!c->journal->transactions)
		c->journal->transactions = ret;
	return ret;
}

/* Mark the given transaction as closed (i.e. no new blocks can be added) */
void finish_transaction(journal_trans_t *trans)
{
	if (trans != &no_journal_trans)
		trans->transaction_state = TRANS_CLOSED;
}

/* Free the finished transaction */
void free_transaction(journal_trans_t *trans)
{
	while (trans->writeout_blocks) {
		void *tmp = trans->writeout_blocks;
		trans->writeout_blocks = trans->writeout_blocks->next;
		free(tmp);
	}
	assert(!trans->protected_extents);
	free(trans);
}

/* Add to the transaction, a write command for the indicated block with the
 * givan data. The data is copied into the transaction (so can safely be
 * mutated by the caller afterwards). The transaction must be ACTIVE.
 */
int journal_write_block(journal_trans_t *trans, blk64_t block_nr, void *buffer)
{
	struct writeout_block *wo_block;
	size_t block_size;
	assert(trans->transaction_state == TRANS_ACTIVE);
	if (trans == &no_journal_trans)
		return write_block(trans->ctx, buffer, block_nr);
	block_size = EXT2_BLOCK_SIZE(&trans->ctx->sb);
	wo_block = trans->writeout_blocks;

	while (wo_block) {
		if (wo_block->block_nr == block_nr) {
			memcpy(wo_block->data, buffer, block_size);
			return 0;
		}
		wo_block = wo_block->next;
	}
	if (trans->num_writeout_blocks >= trans->ctx->journal->max_trans_blocks)
	{
		errno = ENOSPC;
		return -1;
	}
	wo_block = malloc(sizeof(*wo_block) + block_size);
	if (!wo_block) {
		fprintf(stderr, "Out of memory allocating journal block\n");
		errno = ENOMEM;
		return -1;
	}
	memcpy(wo_block->data, buffer, block_size);
	wo_block->block_nr = block_nr;
	wo_block->next = trans->writeout_blocks;
	trans->writeout_blocks = wo_block;
	trans->num_writeout_blocks++;
	return 0;
}

/* Mark the indicated blocks as protected (i.e. make sure no other write
 * modifies them before the transaction is flushed to the journal.
 */
int journal_protect_blocks(journal_trans_t *trans, blk64_t start_block,
                           blk64_t end_block)
{
	struct protected_extent *new_extent;
	assert(trans->transaction_state == TRANS_ACTIVE);

	/* If there is no journal, nothing is protected (as there is no such
	 * state as 'in journal but not yet flushed to disk'
	 */
	if (trans == &no_journal_trans)
		return 0;
	/* We might be more efficient and check if we can merge this range
	 * with another one in the list, but it probably doesn't really
	 * matter enough to bother.
	 */
	new_extent = malloc(sizeof(*new_extent));
	if (!new_extent) {
		fprintf(stderr, "Out of memory allocating journal extent\n");
		errno = ENOMEM;
		return -1;
	}
	new_extent->start_block = start_block;
	new_extent->end_block = end_block;
	new_extent->next = trans->protected_extents;
	trans->protected_extents = new_extent;
	return 0;
}

/* Mark all blocks in the allocation  as protected (i.e. make sure no other
 * write modifies them before the transaction is flushed to the journal.
 */
int journal_protect_alloc(journal_trans_t *t, struct allocation *alloc)
{
	struct data_extent *extent;
	int i, ret = 0;
	for (i = 0, extent = alloc->extents;
	     i < alloc->extent_count && !ret;
	     extent = &alloc->extents[++i])
	{
		ret = journal_protect_blocks(t, extent->start_block,
		                             extent->end_block);
	}
	return ret;
}

/* Increment the tail of the journal by one block, properly wrapping around
 * when the end of the journal is reached. If the journal is full, all data for
 * transactions already flushed to the journal is written out. If the journal
 * is still full, all closed transactions are flushed to the disk and the
 * associated data is written out. If the journal is then still full, an error
 * (ENOSPC) is generated.
 */
static int incr_journal_tail(struct defrag_ctx *c)
{
	char *new_tail;
	int ret;
	new_tail = c->journal->tail + c->journal->blocksize;
	if (new_tail >= (char *)(c->journal->map) + c->journal->size)
		new_tail = (char *)(c->journal->map) + c->journal->blocksize;
	if (new_tail != c->journal->head) {
		c->journal->tail = new_tail;
		return 0;
	}
	assert(c->journal->transactions);
	if (c->journal->transactions->transaction_state == TRANS_ACTIVE) {
		errno = ENOSPC;
		return -1;
	}
	ret = sync_disk(c); /* Clear any DONE transactions */
	if (ret)
		return ret;
	if (new_tail != c->journal->head) {
		c->journal->tail = new_tail;
		return 0;
	}

	ret = flush_journal(c);
	if (ret)
		return ret;
	ret = sync_disk(c); /* Clear any nonactive transactions */
	if (ret)
		return ret;
	if (new_tail != c->journal->head) {
		c->journal->tail = new_tail;
		return 0;
	}
	errno = ENOSPC;
	return -1;
}

/* Write the data associated with the given transaction out to the disk, and
 * remove the transaction from the journal. The transaction must be FLUSHED. All
 * preceding transactions are also written out. The transaction will be DONE
 * afterwards.
 */
int writeout_trans_data(struct defrag_ctx *c, struct journal_trans *trans)
{
	int ret;
	struct writeout_block *block;
	assert(trans->transaction_state == TRANS_FLUSHED);
	block = trans->writeout_blocks;
	while (block) {
		ret = write_block(c, block->data, block->block_nr);
		if (ret < 0)
			return ret;
		block = block->next;
	}
	trans->transaction_state = TRANS_DONE;
	return 0;
}

/* Flushes all the writeout blocks in the transaction to the journal, as
 * well as the appropriate descriptor blocks.
 * Returns the number of blocks written, or negative for error.
 */
static int flush_trans_blocks(struct defrag_ctx *c, struct journal_trans *trans,
                              __u32 sequence)
{
	char *descr, *descr_block_end;
	struct writeout_block *b_descr;
	int ret, count = 0;
	char first_in_descr = 1;

	descr_block_end = (char *)c->journal->tail + c->journal->blocksize;
	descr = descr_block_end; /* Force new descriptor at start */
	b_descr = trans->writeout_blocks;

	while (b_descr) {
		struct journal_block_tag_s *tag;
		if (descr >= descr_block_end - c->journal->tag_size) {
			struct journal_header_s *hdr;
			descr = c->journal->tail;
			descr_block_end = descr + c->journal->blocksize;
			ret = incr_journal_tail(c);
			if (ret < 0)
				return ret;
			hdr = (struct journal_header_s *)descr;
			hdr->h_magic = htobe32(JBD2_MAGIC_NUMBER);
			hdr->h_blocktype = htobe32(JBD2_DESCRIPTOR_BLOCK);
			hdr->h_sequence = htobe32(sequence);
			descr = descr + sizeof(struct journal_header_s);
			first_in_descr = 1;
		}
		tag = (struct journal_block_tag_s *)descr;
		tag->t_blocknr = htobe32(b_descr->block_nr & 0xFFFFFFFF);
		tag->t_flags = !first_in_descr ? htobe32(JBD2_FLAG_SAME_UUID)
		                               : 0;
		if (!b_descr->next)
			tag->t_flags |= htobe32(JBD2_FLAG_LAST_TAG);
		if (c->journal->tag_size >= JBD2_TAG_SIZE64)
			tag->t_blocknr_high = htobe32(b_descr->block_nr >> 32);
		memcpy(c->journal->tail, b_descr->data, c->journal->blocksize);
		ret = incr_journal_tail(c);
		if (ret < 0)
			return ret;
		descr = descr + c->journal->tag_size;
		if (first_in_descr) {
			memset(descr, 0, JBD2_UUID_BYTES);
			first_in_descr = 0;
		}
		if (descr >= descr_block_end - c->journal->tag_size)
			tag->t_flags |= htobe32(JBD2_FLAG_LAST_TAG);
		b_descr = b_descr->next;
		count++;
	}
	return count;
}

/* Flushes the given transaction to disk. and marks it as FLUSHED. The
 * transaction should be DSYNC before calling this. The journal will be
 * synced after the flush, to ensure ordering of the transactions on disk.
 */
static int flush_transaction(struct defrag_ctx *c, struct journal_trans *trans)
{
	void *trans_begin;
	struct commit_header *hdr;
	__u64 tmp;
	__u32 sequence;
	int ret, count;
	assert(trans->transaction_state != TRANS_ACTIVE);
	if (!trans->writeout_blocks) {
		trans->transaction_state = TRANS_FLUSHED;
		return 0;
	}
	assert(trans->transaction_state == TRANS_DSYNC);
	sequence = c->journal->next_sequence++;
	trans_begin = c->journal->tail;
	tmp = (char *)trans_begin - (char *)c->journal->map;
	trans->start_block = tmp / c->journal->blocksize;
	count = flush_trans_blocks(c, trans, sequence);
	if (count < 0)
		return count;
	hdr = (struct commit_header *)c->journal->tail;
	ret = incr_journal_tail(c);
	if (ret < 0)
		return ret;
	if (*(__u32 *)c->journal->tail == htobe32(JBD2_MAGIC_NUMBER)) {
		ret = sync_journal(c);
		if (ret < 0)
			return ret;
		*(__u32 *)c->journal->tail = 0;
	}
	hdr->h_magic = htobe32(JBD2_MAGIC_NUMBER);
	hdr->h_blocktype = htobe32(JBD2_COMMIT_BLOCK);
	hdr->h_sequence = htobe32(sequence);
	if (c->journal->flags & FLAG_JOURNAL_CHECKSUM) {
		hdr->h_chksum_type = JBD2_SHA1_CHKSUM;
		hdr->h_chksum_size = 20;
		memset(hdr->h_chksum, 0, 20);
		/* TODO: Implement actual checksumming */
	}
	hdr->h_commit_sec = 0;
	hdr->h_commit_nsec = 0;
	trans->transaction_state = TRANS_FLUSHED;
	ret = sync_journal(c);
	if (ret < 0)
		return ret;
	while (trans->protected_extents) {
		void *tmp = trans->protected_extents;
		trans->protected_extents = trans->protected_extents->next;
		free(tmp);
	}
	return writeout_trans_data(c, trans);
}

/* Flush all flushable transaction to the journal.
 * All DSYNC transactions will be flushed to the journal and marked FLUSHED.
 */
int flush_journal(struct defrag_ctx *c)
{
	struct journal_trans *trans;
	trans = c->journal->transactions;
	while (trans)
	{
		if (trans->transaction_state == TRANS_DSYNC) {
			int ret;
			ret = flush_transaction(c, trans);
			if (ret < 0)
				return ret;
		}
		if (trans->transaction_state < TRANS_DSYNC) {
			/* Cannot flush later transactions without violating
			 * ordering constraint
			 */
			return 0;
		}
		trans = trans->next;
	}
	return 0;
}

/* Flush all CLOSED transaction upto and including the given transaction out
 * to the journal (and therefore make their protected blocks unprotected).
 * Disk sync may be performed to writeout data for some transactions.
 */
int journal_flush_upto(struct defrag_ctx *c, struct journal_trans *trans)
{
	struct journal_trans *current;
	current = c->journal->transactions;
	while (current != trans && current->transaction_state >= TRANS_FLUSHED)
	{
		current = current->next;
		continue;
	}
	do {
		int ret;
		/* The current transaction cannot be NULL, or the caller could
		 * not have given us a transaction.
		 */
		assert(current->transaction_state != TRANS_ACTIVE);
		if (current->transaction_state == TRANS_CLOSED)
			sync_disk(c);
		assert(current->transaction_state >= TRANS_DSYNC);
		if (current->transaction_state < TRANS_FLUSHED) {
			ret = flush_transaction(c, current);
			if (ret < 0)
				return ret;
		}
	} while (current != trans && (current = current->next));
	return 0;
}

/* Ensure the given block range is not protected by any transactions. This
 * means flushing any older transactions to journal if they do protect the
 * range.
 * Note that a range protected by the active transaction cannot be unprotected.
 */
int journal_ensure_unprotected(struct defrag_ctx *c, blk64_t start_block,
                               blk64_t end_block)
{
	struct journal_trans *trans;
	/* If there is no journal, nothing is protected (as there is no such
	 * state as 'in journal but not yet flushed to disk'
	 */
	if (!c->journal)
		return 0;
	trans = c->journal->transactions;
	while (trans) {
		struct protected_extent *extent;
		struct writeout_block *block;
		extent = trans->protected_extents;
		while (extent) {
			if ((extent->start_block <= start_block
			         && extent->end_block >= start_block)
			    || (extent->start_block <= end_block
			         && extent->end_block >= end_block))
			{
				int ret;
				ret = journal_flush_upto(c, trans);
				if (ret < 0)
					return ret;
			}
			extent = extent->next;
		}
		block = trans->writeout_blocks;
		while (block) {
			if (block->block_nr >= start_block
			    && block->block_nr <= end_block)
			{
				int ret;
				ret = journal_flush_upto(c, trans);
				if (ret < 0)
					return ret;
			}
			block = block->next;
		}
		trans = trans->next;
	}
	return 0;
}

int close_journal(struct defrag_ctx *c)
{
	int ret;
	if (!c->journal)
		return 0;
	ret = sync_disk(c); /* Clear any DONE transactions */
	if (ret)
		return ret;
	ret = flush_journal(c);
	if (ret)
		return ret;
	ret = sync_disk(c); /* Clear any nonactive transactions */
	if (ret)
		return ret;
	ret = sync_journal(c); /* Clear journal's sequence nr */
	if (ret)
		return ret;
	assert(!c->journal->transactions);
	return 0;
}

_Bool journal_try_read_block(struct defrag_ctx *c, void *buf, blk64_t block_nr)
{
	struct journal_trans *trans;
	_Bool ret = 0;
	if (!c->journal)
		return 0;
	trans = c->journal->transactions;
	while (trans) {
		struct writeout_block *block = trans->writeout_blocks;
		while (block) {
			if (block->block_nr == block_nr) {
				size_t blk_size = EXT2_BLOCK_SIZE(&c->sb);
				memcpy(buf, block->data, blk_size);
				ret = 1;
				break;
			}
			block = block->next;
		}
		trans = trans->next;
	}
	return ret;
}

/* Sync all flushed transactions to the journal */
int sync_journal(struct defrag_ctx *disk)
{
	char *base = disk->journal->map;
	struct journal_superblock_s *sb;
	struct journal_trans *trans;
	int tmp;
	base -= disk->journal->map_offset;
	sb = (void *)disk->journal->map;
	if (disk->journal->transactions
	    && disk->journal->transactions->transaction_state >= TRANS_FLUSHED)
	{
		struct journal_trans *trans;
		struct journal_header_s *hdr;
		trans = disk->journal->transactions;
		sb->s_start = htobe32(trans->start_block);
		hdr = (void *)((char *)disk->journal->map
		              + disk->journal->blocksize * trans->start_block);
		sb->s_sequence = hdr->h_sequence;
	} else {
		sb->s_start = 0;
	}
	if (!disk->journal->journal_alloc) {
		tmp = msync(base,
		            disk->journal->size + disk->journal->map_offset,
		            MS_SYNC);
		if (tmp < 0) {
			tmp = errno;
			fprintf(stderr, "Error sync'ing journal: %s\n",
			        strerror(tmp));
			errno = tmp;
			return -1;
		}
	} else {
		struct allocation *alloc = disk->journal->journal_alloc;
		int i;
		for (i = 0; i < alloc->extent_count; i++) {
			size_t size;
			off_t offset;
			size = alloc->extents[i].end_block
			       - alloc->extents[i].start_block + 1;
			size *= EXT2_BLOCK_SIZE(&disk->sb);
			offset = alloc->extents[i].start_block;
			offset *= EXT2_BLOCK_SIZE(&disk->sb);
			tmp = pwrite(disk->fd, base, size, offset);
			if (tmp < 0) {
				tmp = errno;
				fprintf(stderr, "Error writing journal: %s\n",
				        strerror(tmp));
				errno = tmp;
				return -1;
			}
			base += size;
		}
	}
	tmp = fsync(disk->fd); /* fdatasync is probably better here */
	if (tmp < 0) {
		tmp = errno;
		fprintf(stderr, "Error sync'ing journal: %s\n",
		        strerror(tmp));
		errno = tmp;
		return -1;
	}
	trans = disk->journal->transactions;
	return 0;
}

void journal_mark_synced(struct defrag_ctx *c)
{
	struct journal_trans *trans;
	if (c->journal) {
		trans = c->journal->transactions;
		while (trans && trans->transaction_state == TRANS_DONE)
		{
			ptrdiff_t offset;
			c->journal->transactions = trans->next;
			free_transaction(trans);
			trans = c->journal->transactions;
			if (trans && trans->start_block) {
				offset = trans->start_block;
				offset *= c->journal->blocksize;
			} else {
				offset = c->journal->blocksize;
				c->journal->tail = c->journal->map;
				c->journal->tail += offset;
			}
			c->journal->head = (char *)c->journal->map + offset;
		}
		while (trans) {
			if (trans->transaction_state == TRANS_CLOSED)
				trans->transaction_state = TRANS_DSYNC;
			trans = trans->next;
		}
	}
}

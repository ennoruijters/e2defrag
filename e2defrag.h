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

#ifndef E2DEFRAG_H
#define E2DEFRAG_H

#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext3_extents.h>
#include <stdint.h>
#include "rbtree.h"

typedef __u64 blk64_t;
typedef __u64 e2_blkcnt_t;
typedef __u32 ext2_ino_t;

struct settings {
	unsigned int simulate : 1;
	unsigned int interactive : 1;
	unsigned int no_data_move : 1;
};

extern struct settings global_settings;

#define SUPERBLOCK_OFFSET 1024
#define SUPERBLOCK_SIZE 1024

/* Logical block numbers of the first-level indirect blocks */
#define EXT2_IND_LBLOCK(sb)	EXT2_IND_BLOCK
#define EXT2_DIND_LBLOCK(sb)	(EXT2_DIND_BLOCK + EXT2_ADDR_PER_BLOCK(sb))
#define EXT2_TIND_LBLOCK(sb)	(EXT2_DIND_LBLOCK(sb) + 1 \
                                 + EXT2_ADDR_PER_BLOCK(sb) \
                                   * (EXT2_ADDR_PER_BLOCK(sb) + 1))

#define EXT2_SECTORS_PER_BLOCK(sb) (EXT2_BLOCK_SIZE(sb) / 512)

#define PAGE_START(x) \
	((void *)(((uintptr_t)(x)) - (((uintptr_t)(x)) % getpagesize())))

static inline e2_blkcnt_t ext2_groups_on_disk(const struct ext2_super_block *sb)
{
	e2_blkcnt_t ret = sb->s_blocks_count / sb->s_blocks_per_group;
	if (sb->s_blocks_count % sb->s_blocks_per_group)
		ret++;
	return ret;
}

static inline ext2_ino_t ext2_inodes_on_disk(const struct ext2_super_block *sb)
{
	return ext2_groups_on_disk(sb) * EXT2_INODES_PER_GROUP(sb);
}

struct sparse_extent {
	blk64_t start;
	e2_blkcnt_t num_blocks;
};

struct data_extent {
	blk64_t start_block;
	blk64_t end_block;
	blk64_t start_logical;
	ext2_ino_t inode_nr;
	struct rb_node block_rb;
	struct rb_node size_rb;
	unsigned int uninit : 1;
};

struct free_extent {
	blk64_t start_block;
	blk64_t end_block;
	struct rb_node block_rb;
	struct rb_node size_rb;
};

struct allocation {
	e2_blkcnt_t block_count;
	e2_blkcnt_t extent_count;
	struct data_extent extents[];
};

struct inode {
	struct allocation *data;
	struct allocation *metadata; /* If NULL: inode uses direct addressing */
	union on_disk_block {
		__u32 i_block[EXT2_N_BLOCKS];
		struct {
			struct ext3_extent_header hdr;
			union {
				struct ext3_extent leaf;
				struct ext3_extent_idx index;
			} extent[4];
		} extents;
	} *on_disk;
	struct sparse_extent *sparse;
	int num_sparse;
};

struct disk_journal {
	void *map;
	char *head; /* Start of the first unflushed transaction */
	char *tail; /* One past the end of the last (or current) transaction */
	struct allocation *journal_alloc; /* NULL if map is a proper mmap */
	off_t map_offset;
	size_t size;
	int blocksize;
	int max_trans_blocks; /* Maximum number of data blocks per trans. */
	unsigned char tag_size;
};

struct defrag_ctx {
	struct ext2_super_block sb;
	struct rb_root extents_by_block;
	struct rb_root extents_by_size;
	struct rb_root free_tree_by_block;
	struct rb_root free_tree_by_size;
	struct {
		void *map_start;
		unsigned char *bitmap;
		struct ext4_group_desc *gd;
		size_t inode_map_length;
		size_t bitmap_map_length;
		off_t bitmap_offset;
	} *bg_maps;
	char *gd_map;
	struct disk_journal *journal;
	size_t map_length;
	int nr_inode_maps;
	int fd;
	struct inode *inodes[];
};

/* FUNCTION DECLARATIONS */

/* algorithm.c */
int consolidate_free_space(struct defrag_ctx *c);
int try_improve_inode(struct defrag_ctx *c, ext2_ino_t inode_nr);
int do_one_inode(struct defrag_ctx *c, ext2_ino_t inode_nr);
int do_whole_disk(struct defrag_ctx *c);

/* allocation.c */
struct allocation *copy_allocation(struct allocation *old);
void alloc_move_extent(struct allocation *alloc, struct data_extent *extent,
                       blk64_t new_start);
int used_in_alloc(struct allocation *alloc, blk64_t start, e2_blkcnt_t size);
struct allocation *alloc_subtract(struct allocation *from,
                                  struct allocation *data);
struct allocation *split_extent(struct allocation *alloc,
                                struct data_extent *extent, blk64_t new_end,
                                blk64_t new_start_logical);

/* bitmap.c */
void mark_blocks_unused(struct defrag_ctx *c, blk64_t first_block,
                        e2_blkcnt_t count);
void mark_blocks_used(struct defrag_ctx *c, blk64_t first_block,
                      e2_blkcnt_t count);

/* bmove.c */
int move_file_range(struct defrag_ctx *c, ext2_ino_t inode, blk64_t from,
                    e2_blkcnt_t numblocks, blk64_t dest);
int move_data_extent(struct defrag_ctx *c, struct data_extent *extent_to_copy,
                     struct allocation *target);
int copy_data(struct defrag_ctx *c, struct allocation *from,
              struct allocation **target);

/* debug.c */
void dump_trees(struct defrag_ctx *c, int to_dump);

/* freespace.c */
int deallocate_space(struct defrag_ctx *c, blk64_t start, e2_blkcnt_t num);
int deallocate_blocks(struct defrag_ctx *c, struct allocation *space);
int allocate(struct defrag_ctx *c, struct allocation *space);
struct allocation *get_blocks(struct defrag_ctx *c, e2_blkcnt_t num_blocks,
                              ext2_ino_t inode_nr, blk64_t first_logical);
struct allocation *get_range_allocation(blk64_t start_block,
                                        e2_blkcnt_t num_blocks,
                                        blk64_t start_logical);

/* inode.c */
int try_extent_merge(struct defrag_ctx *, struct inode *, struct data_extent *);
blk64_t get_physical_block(struct inode *inode, blk64_t logical_block,
                           int *extent_nr);
blk64_t get_logical_block(struct inode *inode, blk64_t phys_block);
int is_metadata(struct defrag_ctx *c, struct data_extent *extent);

/* interactive.c */
int move_extent_interactive(struct defrag_ctx *c);
int defrag_file_interactive(struct defrag_ctx *c);

/* io.c */
struct defrag_ctx *open_drive(char *filename);
int read_block(struct defrag_ctx *c, void *buf, blk64_t block);
int write_block(struct defrag_ctx *c, void *buf, blk64_t block);
int set_e2_filesystem_data(struct defrag_ctx *c);
void close_drive(struct defrag_ctx *c);

/* journal.c */
int unmap_journal(struct defrag_ctx *disk);
int journal_init(struct defrag_ctx *disk);

/* metadata_write.c */
int write_extent_metadata(struct defrag_ctx *c, struct data_extent *e);
int move_metadata_extent(struct defrag_ctx *c, struct data_extent *extent,
                         struct allocation *target);
int write_inode_metadata(struct defrag_ctx *c, struct inode *inode);

/* metadata_read.c */
long parse_inode(struct defrag_ctx *c, ext2_ino_t inode_nr,
                 struct ext2_inode *inode);
struct inode *read_inode(struct defrag_ctx *c, ext2_ino_t ino);

#endif

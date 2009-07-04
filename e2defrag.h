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
	struct sparse_extent *sparse;
	/* sparse extent list terminated by a sparse extent of 0 blocks */
	ext2_ino_t inode_nr;
	struct rb_node block_rb;
	struct rb_node size_rb;
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
	e2_blkcnt_t block_count; /* Excludes any extent tree blocks */
	e2_blkcnt_t extent_count; /* Excludes any extent tree extents */
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
	struct data_extent extents[];
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
	size_t map_length;
	int nr_inode_maps;
	int fd;
	struct inode *inodes[];
};

/* FUNCTION DECLARATIONS */

/* algorithm.c */
int consolidate_free_space(struct defrag_ctx *c, int silent);

/* bitmap.c */
void mark_blocks_unused(struct defrag_ctx *c, blk64_t first_block,
                        e2_blkcnt_t count);
void mark_blocks_used(struct defrag_ctx *c, blk64_t first_block,
                      e2_blkcnt_t count);

/* bmove.c */
int move_file_range(struct defrag_ctx *c, ext2_ino_t inode, blk64_t from,
                    e2_blkcnt_t numblocks, blk64_t dest);
int move_data_extent(struct defrag_ctx *c, struct data_extent *extent_to_copy,
                     blk64_t new_start);
int move_file_data(struct defrag_ctx *c, ext2_ino_t inode, blk64_t dest);
int move_inode_data(struct defrag_ctx *c, struct inode *inode,
                    struct allocation *target);

/* debug.c */
void dump_trees(struct defrag_ctx *c, int to_dump);

/* freespace.c */
int allocate_space(struct defrag_ctx *c, blk64_t start, e2_blkcnt_t numblocks);
int deallocate_space(struct defrag_ctx *c, blk64_t start, e2_blkcnt_t num);
int deallocate_blocks(struct defrag_ctx *c, struct allocation *space);
struct allocation *allocate_blocks(struct defrag_ctx *c, e2_blkcnt_t num_blocks,
                                   ext2_ino_t inode_nr, blk64_t first_logical);

/* inode.c */
int try_extent_merge(struct defrag_ctx *, struct inode *, struct data_extent *);
int split_extent(struct defrag_ctx *c, struct inode *inode,
                 struct data_extent *extent, blk64_t new_end_block);
blk64_t get_physical_block(struct inode *inode, blk64_t logical_block,
                           int *extent_nr);

/* interactive.c */
int move_extent_interactive(struct defrag_ctx *c);
int move_file_interactive(struct defrag_ctx *c);
int defrag_file_interactive(struct defrag_ctx *c);

/* io.c */
struct defrag_ctx *open_drive(char *filename);
int read_block(struct defrag_ctx *c, void *buf, blk64_t block);
int write_block(struct defrag_ctx *c, void *buf, blk64_t block);
int set_e2_filesystem_data(struct defrag_ctx *c);
void close_drive(struct defrag_ctx *c);

/* metadata_writeback.c */
int write_extent_metadata(struct defrag_ctx *c, struct data_extent *e);

/* metadata_read.c */
long parse_inode(struct defrag_ctx *c, ext2_ino_t inode_nr,
                 struct ext2_inode *inode);

#endif

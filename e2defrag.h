#ifndef E2DEFRAG_H
#define E2DEFRAG_H

#include <ext2fs/ext2_fs.h>
#include <stdint.h>
#include "rbtree.h"

typedef __u64 blk64_t;
typedef __u64 e2_blkcnt_t;
typedef __u32 ext2_ino_t;

#define SUPERBLOCK_OFFSET 1024
#define SUPERBLOCK_SIZE 1024

/* Logical block numbers of the first-level indirect blocks */
#define EXT2_IND_LBLOCK(sb)	EXT2_IND_BLOCK
#define EXT2_DIND_LBLOCK(sb)	(EXT2_DIND_BLOCK + 1 + EXT2_ADDR_PER_BLOCK(sb))
#define EXT2_TIND_LBLOCK(sb)	(EXT2_TIND_BLOCK + 1 \
                                 + EXT2_ADDR_PER_BLOCK(sb) \
                                   * (EXT2_ADDR_PER_BLOCK(sb) + 1))

#define PAGE_START(x) \
	((void *)(((uintptr_t)(x)) - (((uintptr_t)(x)) % getpagesize())))

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
	struct rb_node node;
};

struct inode {
	e2_blkcnt_t block_count;
	e2_blkcnt_t extent_count;
	struct ext2_inode *on_disk; /* We don't care about the extended part */
	struct data_extent extents[];
};

struct defrag_ctx {
	struct ext2_super_block sb;
	struct rb_root extents_by_block;
	struct rb_root extents_by_size;
	struct rb_root free_tree;
	struct {
		struct ext2_inode *map_start;
		unsigned char *bitmap;
		size_t inode_map_length;
		size_t bitmap_map_length;
		off_t bitmap_offset;
	} *bg_maps;
	int nr_inode_maps;
	int fd;
	int read_only;
	struct inode *inodes[];
};

/* FUNCTION DECLARATIONS */

/* bitmap.c */
void mark_blocks_unused(struct defrag_ctx *c, blk64_t first_block,
                        e2_blkcnt_t count);
void mark_blocks_used(struct defrag_ctx *c, blk64_t first_block,
                      e2_blkcnt_t count);

/* debug.c */
void dump_trees(struct defrag_ctx *c);

/* inode.c */
long parse_inode(struct defrag_ctx *, ext2_ino_t inode_nr, struct ext2_inode *);
int try_extent_merge(struct defrag_ctx *, struct inode *, struct data_extent *);

/* io.c */
struct defrag_ctx *open_drive(char *filename, char read_only);
int read_block(struct defrag_ctx *c, void *buf, blk64_t block);
int write_block(struct defrag_ctx *c, void *buf, blk64_t block);
int set_e2_filesystem_data(struct defrag_ctx *c);
void close_drive(struct defrag_ctx *c);

#endif

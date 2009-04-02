#ifndef E2DEFRAG_H
#define E2DEFRAG_H

#include <ext2fs/ext2_fs.h>

typedef __u64 blk64_t;
typedef __u64 e2_blkcnt_t;
typedef __u32 ext2_ino_t;

#define SUPERBLOCK_OFFSET 1024
#define SUPERBLOCK_SIZE 1024

#define METADATA_INODE 1

struct extent {
	blk64_t start_block;
	blk64_t end_block;
	blk64_t start_logical_block;
};

struct inode {
	e2_blkcnt_t block_count;
	e2_blkcnt_t extent_count;
	struct extent *blocks;
};

struct defrag_ctx {
	int fd;
	struct ext2_super_block sb;
	ext2_ino_t *owners;
	struct inode *inodes;
};

/* FUNCTION DECLARATIONS */

/* inode.c */
long parse_inode(struct defrag_ctx *c, ext2_ino_t inode_nr,
                 struct ext2_inode *inode);

/* io.c */
struct defrag_ctx *open_drive(char *filename, char read_only);
int read_block(struct defrag_ctx *c, void *buf, blk64_t block);
int set_e2_filesystem_data(struct defrag_ctx *c);

#endif

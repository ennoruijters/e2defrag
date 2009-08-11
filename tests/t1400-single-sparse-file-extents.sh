#!/bin/sh

# Tests if a single file of two 1024-byte non-adjacent blocks with a
# sparse block in between the data blocks is correctly defragmented 
# on a tiny ext4 filesystem (the file has extents).

. ./test-lib.sh

test_begin "t1400-single-sparse-file-extents"

load_image single-sparse-file-ext4

infra_cmd "mv single-sparse-file-ext4.img disk.img"
infra_cmd "echo \"dump_inode <12> before\nquit\n\" | debugfs disk.img \
           > /dev/null"

test_and_stop_on_error "defragmenting ext4 disk with single file" \
                       "e2defrag disk.img > /dev/null"

test_and_continue "resulting image should not have file system errors" \
                  "e2fsck -f -y disk.img 2>/dev/null > fsckout"

test_and_continue "resulting image should not be fragmented" \
                  "grep \"0.0% non-contiguous\" fsckout > /dev/null"

test_and_continue "file in image should be unchanged" \
                  "echo \"dump_inode <12> after\nquit\n\" \
                   | debugfs disk.img \
                   > /dev/null 2>/dev/null && cmp before after"

test_end

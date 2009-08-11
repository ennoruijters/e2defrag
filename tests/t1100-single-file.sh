#!/bin/sh

# Tests if a single file of two 1024-byte non-adjacent blocks on a tiny
# ext2 filesystem is correctly defragmented

. ./test-lib.sh

test_begin "t1100-single-file"

load_image single-file

infra_cmd "echo \"dump_inode <12> before\nquit\n\" | debugfs single-file.img \
           > /dev/null"

test_and_stop_on_error "defragmenting ext2 disk with single file" \
                       "e2defrag single-file.img > /dev/null"

test_and_continue "resulting image should not have file system errors" \
                  "e2fsck -f -y single-file.img 2>/dev/null > fsckout"

test_and_continue "resulting image should not be fragmented" \
                  "grep \"0.0% non-contiguous\" fsckout > /dev/null"

test_and_continue "file in image should be unchanged" \
                  "echo \"dump_inode <12> after\nquit\n\" \
                   | debugfs single-file.img \
                   > /dev/null 2>/dev/null && cmp before after"

test_end

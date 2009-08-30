#!/bin/sh

# Tests if a single file of one normal 1-block extent and one adjacent 1-block
# uninitialized extent is correctly left unchanged on a tiny ext4 filesystem.

. ./test-lib.sh

test_begin "t1402-prealloc-nofrag"

load_image prealloc-nofrag

infra_cmd "cp prealloc-nofrag.img disk.img"

test_and_stop_on_error "defragmenting unfragmented ext4 disk with uninitialized extent" \
                       "e2defrag disk.img > /dev/null"

test_and_continue "resulting image should be unchanged" \
                  "cmp -s prealloc-nofrag.img disk.img"

test_end

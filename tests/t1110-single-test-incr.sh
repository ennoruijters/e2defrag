# Copyright 2009 Enno Ruijters
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of version 2 of the GNU General
# Public License as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#!/bin/sh

# Tests if a single file of two 1024-byte non-adjacent blocks on a tiny
# ext2 filesystem is correctly defragmented by a single improvement command.

. ./test-lib.sh

test_begin "t1110-single-file-incremental"

load_image single-file

infra_cmd "echo \"dump_inode <12> before\nquit\n\" | debugfs single-file.img \
           > /dev/null"

echo "i12\n0" > tmp
test_and_stop_on_error "defragmenting ext2 disk with single file" \
                       "cat tmp | e2defrag -i single-file.img > /dev/null"

test_and_continue "resulting image should not have file system errors" \
                  "e2fsck -f -y single-file.img 2>/dev/null > fsckout"

test_and_continue "resulting image should not be fragmented" \
                  "grep \"0.0% non-contiguous\" fsckout > /dev/null"

test_and_continue "file in image should be unchanged" \
                  "echo \"dump_inode <12> after\nquit\n\" \
                   | debugfs single-file.img \
                   > /dev/null 2>/dev/null && cmp before after"

test_end
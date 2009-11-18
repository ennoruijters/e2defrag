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

# Tests if a single file of two 1024-byte non-adjacent blocks on a tiny, full
# ext2 filesystem is correctly left unchanged on improvement request.
# This is a regression test for a bug which locked up the program in this case.

. ./test-lib.sh

test_begin "t1111-full"

load_image full

infra_cmd "echo \"dump_inode <12> before\nquit\n\" | debugfs full.img \
           > /dev/null"

test_and_stop_on_error "defragmenting full ext2 disk with fragmented file" \
                       "echo "i12\n0" | e2defrag -i full.img > /dev/null"

test_and_continue "resulting image should not have file system errors" \
                  "e2fsck -f -y full.img 2>/dev/null > fsckout"

test_and_continue "file in image should be unchanged" \
                  "echo \"dump_inode <12> after\nquit\n\" \
                   | debugfs full.img \
                   > /dev/null 2>/dev/null && cmp before after"

test_end

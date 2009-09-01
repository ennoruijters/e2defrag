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

# Tests if a single file of one normal 1-block extent and one 1-block
# uninitialized extent (and an unused block inbetween them) is correctly
# defragmented on a tiny ext4 filesystem using incremental improvement

. ./test-lib.sh

test_begin "t1411-prealloc-incremental"

load_image prealloc

infra_cmd "mv prealloc.img disk.img"
infra_cmd "echo \"dump_inode <12> before\nquit\n\" | debugfs disk.img \
           > /dev/null"

echo "i12\n0" > tmp
test_and_stop_on_error "defragmenting ext4 disk with uninitialized extent" \
                       "cat tmp | e2defrag -i disk.img > /dev/null"

test_and_continue "resulting image should not have file system errors" \
                  "e2fsck -f -y disk.img 2>/dev/null > fsckout"

test_and_continue "resulting image should not be fragmented" \
                  "grep \"0.0% non-contiguous\" fsckout > /dev/null"

infra_cmd "echo \"dump_inode <12> after\nquit\n\" | debugfs disk.img"

# Note: This relies on debugfs dumping even the uninitialized extents. If this
# ever changes we'll have to find some other way of testing this.
test_and_continue "uninitialized blocks should not have been copied" \
                  "! cmp -s -i 1024 before after"

test_and_continue "initialized blocks should be unchanged" \
                  "cmp -n 1024 before after"

test_end

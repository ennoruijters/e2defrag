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

# Tests if a single file of three 1024-byte non-adjacent blocks is correctly
# defragmented on a tiny ext4 filesystem (the file has extents) using
# using incremental improvement

. ./test-lib.sh

if [ "$1" != "do_test" ]; then
	test_begin "t1311-single-3-extent-file-incr" "$0"
	exit $?;
fi

load_image single-3ext-file

infra_cmd "mv single-3ext-file.img disk.img"
infra_cmd "echo \"dump_inode <12> before\nquit\n\" | debugfs disk.img \
           > /dev/null"

echo "i12\n0" > tmp
test_and_stop_on_error "defragmenting ext4 disk with single 3-extent file" \
                       "cat tmp | e2defrag -i disk.img > /dev/null"

test_and_continue "resulting image should not have file system errors" \
                  "e2fsck -f -y disk.img 2>/dev/null > fsckout"

test_and_continue "resulting image should not be fragmented" \
                  "grep \"0.0% non-contiguous\" fsckout > /dev/null"

test_and_continue "file in image should be unchanged" \
                  "echo \"dump_inode <12> after\nquit\n\" \
                   | debugfs disk.img \
                   > /dev/null 2>/dev/null && cmp before after"

test_end

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

# Tests if a single file consisting of two 1024-byte non-adjacent blocks,
# with a single sparse blocks in between them is correctly defragmented using
# incremental improvement.

. ./test-lib.sh

if [ "$1" != "do_test" ]; then
	test_begin t1210-single-sparse-file-incremental "$0"
	exit $?;
fi

load_image single-sparse-file

infra_cmd "echo \"dump_inode <12> before\nquit\n\" \
           | debugfs single-sparse-file.img > /dev/null"

echo "i12\n0" > tmp
test_and_stop_on_error "defragmenting ext2 disk with single sparse file" \
                       "cat tmp | e2defrag -i single-sparse-file.img > /dev/null"

test_and_continue "resulting image should not have file system errors" \
                  "e2fsck -f -y single-sparse-file.img 2>/dev/null > fsckout"

test_and_continue "resulting image should not be fragmented" \
                  "grep \"0.0% non-contiguous\" fsckout > /dev/null"

test_and_continue "file in image should be unchanged" \
                  "echo \"dump_inode <12> after\nquit\n\" \
                   | debugfs single-sparse-file.img > /dev/null 2>/dev/null \
                   && cmp before after"

test_end

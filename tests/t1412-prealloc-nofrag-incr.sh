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

# Tests if a single file of one normal 1-block extent and one adjacent 1-block
# uninitialized extent is correctly left unchanged on a tiny ext4 filesystem
# using incremental improvement.

. ./test-lib.sh

test_begin "t1412-prealloc-nofrag-incremental"

load_image prealloc-nofrag

infra_cmd "cp prealloc-nofrag.img disk.img"

echo "i12\n0" > tmp
test_and_stop_on_error "defragmenting unfragmented ext4 disk with uninitialized extent" \
                       "cat tmp | e2defrag -i disk.img > /dev/null"

test_and_continue "resulting image should be unchanged" \
                  "cmp -s prealloc-nofrag.img disk.img"

test_end

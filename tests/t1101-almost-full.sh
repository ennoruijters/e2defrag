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

# Tests if the program is capable of performing in-place overwrites of files
# using a disk that is full except for one block separating two extents
# of a file.

. ./test-lib.sh

if [ "$1" != "do_test" ]; then
	test_begin "t1101-almost-full" "$0"
	exit $?
else

load_image almost-full

infra_cmd "echo \"dump_inode <12> before1\nquit\n\" | debugfs almost-full.img \
           > /dev/null"
infra_cmd "echo \"dump_inode <13> before2\nquit\n\" | debugfs almost-full.img \
           > /dev/null"

test_and_stop_on_error "defragmenting an almost full ext2 disk" \
                       "e2defrag almost-full.img > /dev/null"

test_and_continue "resulting image should not have file system errors" \
                  "e2fsck -f -y almost-full.img 2>/dev/null > fsckout"

test_and_continue "resulting image should not be fragmented" \
                  "grep \"0.0% non-contiguous\" fsckout > /dev/null"

infra_cmd "echo \"dump_inode <12> after1\nquit\n\" | debugfs almost-full.img \
           > /dev/null"
infra_cmd "echo \"dump_inode <13> after2\nquit\n\" | debugfs almost-full.img \
           > /dev/null"

test_and_continue "files in image should be unchanged" \
                  "cmp before1 after1 && cmp before2 after2"

test_end
fi #do_test

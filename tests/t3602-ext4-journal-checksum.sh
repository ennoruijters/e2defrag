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

# Write the journal on an ext3 disk with a single (file data) block, runs
# e2fsck to play the journal, and checks that the file in fact has the
# data is should.

. ./test-lib.sh

if [ "$1" != "do_test" ]; then
	test_begin "t3602-journal-checksum" "$0"
	exit $?
fi

load_image ext4-journal
load_data d3602-test.c

infra_cmd "cc -ggdb -o test -I ../../ d3602-test.c ../../journal.c ../../journal_init.c ../../io.c ../../crc16.c ../../rbtree.c ../../metadata_read.c"

test_and_stop_on_error "write block to journal" \
                       "./test ext4-journal.img new_data > /dev/null"

infra_cmd "dd if=/dev/zero of=old_data bs=1K count=1"

test_and_continue "file in image should have old data" \
                  "echo \"dump_inode <12> file\nquit\n\" \
                   | debugfs ext4-journal.img \
		   > /dev/null 2>/dev/null && cmp file old_data > /dev/null"

infra_cmd "e2fsck -v -y ext4-journal.img 2>&1 >fsckout || true"

test_and_continue "file in image should have new data" \
                  "echo \"dump_inode <12> file\nquit\n\" \
                   | debugfs ext4-journal.img \
		   > /dev/null 2>/dev/null && cmp file new_data > /dev/null"

test_end

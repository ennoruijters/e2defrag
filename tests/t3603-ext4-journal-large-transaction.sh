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
	test_begin "t3603-journal-large-transaction" "$0"
	exit $?
fi

load_image ext4-journal-large-file
load_data d3603-test.c

infra_cmd "cc -ggdb -o test -I ../../ d3603-test.c ../../journal.c ../../journal_init.c ../../io.c ../../crc16.c ../../rbtree.c ../../metadata_read.c"

test_and_continue "should fail to write transaction to journal" \
                  "! ./test ext4-journal-large-file.img > /dev/null 2>testerr"

infra_cmd "dd if=/dev/zero of=old_data bs=1M count=10"

echo "#!/bin/sh" > fsck.sh
echo "e2fsck -v -p ext4-journal-large-file.img 2>fsckerr >fsckout" >> fsck.sh
echo "RET=\$?" >> fsck.sh
echo "exit \$(( \$RET != 0 && \$RET != 1 ))" >> fsck.sh

test_and_continue "image should not have fs corruption" \
                  "sh fsck.sh"

test_and_continue "file in image should have old data" \
                  "echo \"dump_inode <12> file\nquit\n\" \
                   | debugfs ext4-journal-large-file.img \
		   > /dev/null 2>/dev/null && cmp file old_data > /dev/null"

test_end

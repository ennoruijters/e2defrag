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

usage() {
	echo "Usage: run-all-tests.sh [--valgrind]"
}

if [ -e "test-passes" ]; then
	rm test-passes;
fi

if [ -e "test-failures" ]; then
	rm test-failures;
fi

if [ -e "test-aborts" ]; then
	rm test-aborts;
fi

if [ -d "test_trash" ]; then
	rm -r test_trash
fi

while true; do
	if [ -z "$1" ]; then
		break;
	fi
	case "$1" in
		-h|--help|-\?) usage; exit 0;;
		--valgrind) export VALGRIND=1; shift;;
		*) echo "Unknown parameter $1"; exit 0;;
	esac
done

for i in t[0-9][0-9][0-9][0-9]-*.sh; do
	sh $i;
done

if [ -e "test-passes" ]; then
	num_passed=`wc -l < test-passes`
	echo "passed: $num_passed"
else
	echo "absolutely none of tests passed. Did the program even compile?"
fi

if [ -e "test-failures" ]; then
	num_failed=`wc -l < test-failures`
	echo "FAILED: $num_failed"
else
	echo "failed: 0"
fi

if [ -e "test-aborts" ]; then
	num_aborted=`wc -l < test-aborts`
	echo "ABORTED: $num_aborted"
fi

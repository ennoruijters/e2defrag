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

# Basic infrastructure for tests.

tests_path=
test_count=0

test_failed=""
infra_failed=0

test_begin () {
	if [ "$#" -ne 1 ]; then
		echo "bug in test script: not 1 parameter to test_begin"
		exit 1;
	fi
	test_name="$1"
	echo "$test_name:"
}

load_image () {
	if [ "$#" -ne 1 ]; then
		echo "bug in test script: not 1 parameter to load_image"
		test_end
		exit 1;
	fi
	eval "cp ../images/$1.img.gz" .
	if [ "$?" != 0 ]; then
		echo "test error: could not load image file $1"
		test_end
		exit 1;
	fi
	eval "gunzip $1.img.gz"
	if [ "$?" != 0 ]; then
		echo "test error: could not gunzip image file $1"
		test_end
		exit 1;
	fi
}

test_end () {
	cd "$tests_path"
	rm -r test_trash
	if [ "$infra_failed" != 0 ]; then
		echo "test_name: $test_count" >> test-aborts
		echo "Could not test: $test_name"
		exit 2
	fi
	if [ "$test_failed" != "" ]; then
		echo "$test_name: $test_count" >> test-failures
		echo "FAIL: $test_name\n"
		exit 1
	else
		echo "$test_name" >> test-passes
		echo "pass: $test_name\n"
		exit 0
	fi
}

first_word () {
	FIRST="$1";
}

test_cmd () {
	test_count=$(expr "$test_count" + 1)
	first_word $2;
	if [ ! -z "$VALGRIND" ] && [ "$FIRST" = "e2defrag" ]; then
		eval valgrind -q --error-exitcode=1 "$2"
	else
		eval "$2"
	fi

	if [ "$?" = 0 ]; then
		echo "	pass $test_count: $1"
		true
	else
		echo "	FAIL $test_count: $1"
		test_failed="$test_failed$test_count "
		return 1
	fi
}

test_and_continue () {
	if [ "$#" -ne 2 ]; then
		echo "bug in test script: not 2 parameters to test_and_continue"
		test_end
		exit 1;
	fi
	test_cmd "$1" "$2"
}

test_and_stop_on_error () {
	if [ "$#" -ne 2 ]; then
		echo "bug in test script: not 2 parameters to test_and_continue"
		test_end
		exit 1;
	fi
	test_cmd "$1" "$2"

	if [ "$?" != 0 ]; then
		test_end;
	fi
	true
}

infra_cmd () {
	eval 1>/dev/null 2>/dev/null "$1"
	if [ "$?" -ne "0" ]; then
		echo "error executing command:"
		echo "$1"
		infra_failed=1
		test_end
		exit 1
	fi
}

PATH=$(pwd)/..:$PATH
tests_path=$(pwd)
mkdir test_trash
cd test_trash

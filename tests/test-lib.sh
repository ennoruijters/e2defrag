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

tests_path=""

test_begin () {
	if [ "$#" -ne 2 ]; then
		echo "bug in test script: not 2 parameters to test_begin"
		exit 1;
	fi
	test_name="$1"
	echo "$test_name:"
	tests_path=$(pwd);
	PATH=$(pwd)/..:$PATH

	TRASH_DIR=""
	trap "on_exit" EXIT
	trap "on_exit" HUP
	trap "on_exit" TERM
	trap "on_exit" QUIT
	make_tmpdir;
	cp test-lib.sh "$TRASH_DIR"
	( export test_name && cd "$TRASH_DIR" && sh "../$2" do_test )
	RET=$?;
	case $RET in
		0) echo "$test_name" >> test-passes ;;
		1) echo "$test_name: $test_count" >> test-failures ;;
		2) echo "test_name: $test_count" >> test-aborts ;;
	esac
	exit $?;
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
	if [ "$infra_failed" != 0 ]; then
		echo "Could not test: $test_name"
		exit 2
	fi
	if [ "$test_failed" != "" ]; then
		echo "FAIL: $test_name\n"
		exit 1
	else
		echo "pass: $test_name\n"
		exit 0
	fi
}

first_word () {
	FIRST="$1";
}

test_cmd () {
	LC_ALL=C
	export LC_ALL
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
	LC_ALL=C
	export LC_ALL
	if [ "$#" -ne 2 ]; then
		echo "bug in test script: not 2 parameters to test_and_continue"
		test_end
		exit 1;
	fi
	test_cmd "$1" "$2"
}

test_and_stop_on_error () {
	LC_ALL=C
	export LC_ALL
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
	LC_ALL=C
	export LC_ALL
	eval 1>/dev/null 2>/dev/null "$1"
	if [ "$?" -ne "0" ]; then
		echo "error executing command:"
		echo "$1"
		infra_failed=1
		test_end
		exit 1
	fi
}

make_tmpdir () {
	local TMP_PREFIX="test_trash_"
	local TMP_SUFFIX=0
	local TMP_MAX=10000
	TRASH_DIR="$TMP_PREFIX$TMP_SUFFIX"
	local TMPDIR_GOOD=0

	while [ $TMPDIR_GOOD -eq 0 ] && [ $TMP_SUFFIX -le $TMP_MAX ]; do
		if (umask 077 && mkdir $TRASH_DIR 2> /dev/null); then
			TMPDIR_GOOD=1;
		else
			TMP_SUFFIX=$(($TMP_SUFFIX+1));
			TRASH_DIR="$TMP_PREFIX$TMP_SUFFIX";
		fi;
	done;

	if [ $TMPDIR_GOOD -eq 0 ]; then
		echo "Could not make temporary directory"
		exit 1
	fi;
	if [ ! -d $TRASH_DIR ]; then
		echo "Something has gone very wrong: My temporary directory has disappeared"
		exit 1;
	fi;
	return 0
}

on_exit () {
	if [ $TRASH_DIR != "" ]; then
		rm -rf $TRASH_DIR;
	fi
}

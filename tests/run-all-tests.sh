#!/bin/sh

if [ -e "test-passes" ]; then
	rm test-passes;
fi

if [ -e "test-failures" ]; then
	rm test-failures;
fi

if [ -e "test-aborts" ]; then
	rm test-aborts;
fi

for i in t[0-9][0-9][0-9][0-9]-*.sh; do
	sh $i;
done

if [ -e "test-passes" ]; then
	num_passed=`cat test-passes | wc -l`
	echo "passed: $num_passed"
else
	echo "absolutely none of tests passed. Did the program even compile?"
fi

if [ -e "test-failures" ]; then
	num_failed=`wc -l test-failures`
	echo "FAILED: $num_failed"
else
	echo "failed: 0"
fi

if [ -e "test-aborts" ]; then
	num_aborted=`wc -l test-aborts`
	echo "ABORTED: $num_aborted"
fi

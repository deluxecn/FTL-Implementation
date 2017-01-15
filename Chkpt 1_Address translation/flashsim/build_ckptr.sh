#!/bin/bash

set -e
set -u

ckptr=$1
num_tests=`find tests/checkpoint_${ckptr}/* -maxdepth 0 -type d | wc -l`
for ((i=1; i<=$num_tests; i++))
do
	p=${ckptr}_${i}
	make -C tests/checkpoint_${ckptr} $p
	touch test_$p.log
	./test_$p tests/checkpoint_${ckptr}/test_$p/test_$p.conf test_$p.log
done

echo "Passed all tests"

exit 0

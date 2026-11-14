#!/bin/bash
# Runs named tests of the comdb2 suite several times, one at a time, with
# ordered apply and without, on the same code: for a test that fails with
# ordered apply now and then.
# usage: repeat-tests.sh <run name> <times> <test>...
#
# Run on xeon, detached (nohup).  Setup as suite-with-ordered-apply.sh says:
# the tree built in /tmp/rltest2/src and again in /tmp/rltest3/src, the
# settings in /tmp/rltest-keep/oa.lrl, our pmux on 19500, and an empty
# /tmp/rltest-keep/<run name>/.  The runs with ordered apply go in
# /tmp/rltest2, the runs without in /tmp/rltest3, the two side by side; each
# repetition has a test directory of its own, so nothing is left from the one
# before.  Output: with-<i>.out and without-<i>.out, and a line for each test
# of each repetition in results.txt.
set -u
K=/tmp/rltest-keep; O=$K/$1; N=$2; shift 2
export MAX_TEST_FAILURES=100000 PMUXPORT=19500
res() { grep -E "^![^:]+: (success|failed|timeout)" $1 | sed -E "s/^!([^:]+): ([a-z]+).*/\1 \2/" | sort -u; }
side() { # name, tree, first test id, settings file or nothing
  for i in $(seq 1 $N); do
    ( cd $2/src/tests && TESTID=$(($3 + i)) CUSTOMLRLPATH=$4 make -kj1 "${@:5}" > $O/$1-$i.out 2>&1 )
    res $O/$1-$i.out | sed "s/^/$1 $i /" >> $O/results.txt
  done
}
side with /tmp/rltest2 3100 $K/oa.lrl "$@" &
side without /tmp/rltest3 3200 "" "$@" &
wait
echo ALL-DONE >> $O/status

#!/bin/bash
# The comdb2 test suite on one node with ordered apply on, as runs 13, 15 and
# 16 in BENCHMARK-LOG.md.  usage: suite-with-ordered-apply.sh [run name] [jobs]:
# the run name names the output directory, run15 if not given; jobs is how
# many tests run at once in the first pass, 40 if not given.  Many tests
# depend on timing, and fewer at once gives fewer failures that are only load.  Runs the whole suite with ordered_apply and
# ordered_apply_unit added to every test database, then the tests that failed
# on a build of main, then the ones that pass on main once more with ordered
# apply, then the ordered_apply test with nothing added.
#
# Run on xeon, detached (nohup).  Before starting:
#   /tmp/rltest2/src      the tree to test (tar it over; no build needed)
#   /tmp/rltest-main/src  a tree of main, built with
#                         cmake -G Ninja -S . -B build -DCOMDB2_TEST=1 && ninja -C build && ninja -C build test-tools
#   /tmp/rltest-keep/oa.lrl   two lines: "ordered_apply 1" and "ordered_apply_unit 1"
#   /tmp/rltest-keep/<run name>/   empty directory for the output
#   a pmux of our own: <build>/tools/pmux/pmux -n -p 19500 -b /tmp/pmux.socket.19500
#       (the system pmux on 5105 refuses our databases)
# -DCOMDB2_TEST=1 is needed for the test tools to link.  MAX_TEST_FAILURES is
# raised because the harness otherwise stops after ten failed tests, and about
# 190 fail on a single node whatever the build.  Database directories of
# failed tests are removed as the pass goes, logs kept: /tmp is a 124G RAM
# disk and they would fill it.  A pass takes about an hour at -j40; tests with
# tight time limits (insert_lots, maxtable) fail at that load and pass at -j20.
# Output: /tmp/rltest-keep/<run name>/status, oa.out, main.out, oa-again.out.
set -u
K=/tmp/rltest-keep; O=/tmp/rltest-keep/${1:-run15}; J=${2:-40}
export MAX_TEST_FAILURES=100000 PMUXPORT=19500
cd /tmp/rltest2/src
cmake -G Ninja -S . -B build -DCOMDB2_TEST=1 > $O/cmake.out 2>&1 && ninja -C build > $O/ninja.out 2>&1 && ninja -C build test-tools >> $O/ninja.out 2>&1 || { echo BUILD-FAILED > $O/status; exit 1; }
echo BUILD-OK > $O/status
res() { grep -E "^![^:]+: $2" $1 | sed -E "s/^!([^:]+):.*/\1/" | sort -u; }
# drop the database directories of failed tests as the pass goes; logs stay
cleanoa() {
  while [ ! -f $O/stop-clean ]; do
    res $O/oa.out failed | while read t; do n="$(echo $t | tr -d _)1313"; find /tmp/rltest2/src/tests/test_1313 -mindepth 1 -maxdepth 1 -type d -name "$n" -exec rm -rf {} + 2>/dev/null; done
    sleep 60
  done
}
cleanmain() {
  while [ ! -f $O/stop-clean ]; do
    res $O/main.out failed | while read t; do n="$(echo $t | tr -d _)1414"; find /tmp/rltest-main/src/tests/test_1414 -mindepth 1 -maxdepth 1 -type d -name "$n" -exec rm -rf {} + 2>/dev/null; done
    sleep 60
  done
}
cd /tmp/rltest2/src/tests
cleanoa &
TESTID=1313 CUSTOMLRLPATH=$K/oa.lrl make -kj$J > $O/oa.out 2>&1
echo PASS-DONE >> $O/status
res $O/oa.out failed | grep -v "^ordered_apply$" > $O/oa-failed.txt
cd /tmp/rltest-main/src/tests
cleanmain &
TESTID=1414 make -kj30 $(cat $O/oa-failed.txt | tr "\n" " ") > $O/main.out 2>&1
echo MAIN-DONE >> $O/status
res $O/main.out success > $O/main-pass.txt
cd /tmp/rltest2/src/tests
if [ -s $O/main-pass.txt ]; then
  TESTID=1515 CUSTOMLRLPATH=$K/oa.lrl make -kj20 $(cat $O/main-pass.txt | tr "\n" " ") > $O/oa-again.out 2>&1
fi
TESTID=1616 make -kj2 ordered_apply > $O/oa-own.out 2>&1
touch $O/stop-clean
echo ALL-DONE >> $O/status

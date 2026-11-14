#!/bin/bash
# The tests written for ADR 0003: transactions under contention, each run
# under page locks, commit units, and commit units applied a file per thread
# (tests/oa_*.test, tests/tools/oa_common.sh).  Minutes, where the whole suite
# takes hours; run this while the work is going on and the suite at the end.
# usage: adr-tests.sh <tree> [test...]
# The tree must be built as suite-with-ordered-apply.sh says (-DCOMDB2_TEST=1,
# test-tools), and a pmux must be on PMUXPORT (19500 if not set).  The tests
# that check results run five at a time; oa_throughput then runs alone, so
# that its rates mean something.  Prints each test's result, its OA-RESULT
# lines, and oa_throughput's table.  Exit status 1 if any test failed.
set -u
S=$1; shift
CHECKS="oa_counters oa_bank oa_unique oa_deadlock oa_queue"
TIMED="oa_throughput"
if [ $# -gt 0 ]; then
  CHECKS=""; TIMED=""
  for t in "$@"; do if [ $t = oa_throughput ]; then TIMED=$t; else CHECKS="$CHECKS $t"; fi; done
fi
export MAX_TEST_FAILURES=100000 PMUXPORT=${PMUXPORT:-19500}
ID=${TESTID:-4100}
cd $S/tests || exit 1
rm -rf test_$ID
O=${OUT:-/tmp/adr-tests.$ID.out}; : > $O
t0=$(date +%s)
[ -n "$CHECKS" ] && TESTID=$ID make -kj5 $CHECKS >> $O 2>&1
[ -n "$TIMED" ] && TESTID=$ID make -kj1 $TIMED >> $O 2>&1
echo "== results ($(( $(date +%s) - t0 )) s)"
grep -E "^![^:]+: (success|failed|timeout)" $O | sed -E "s/^!([^:]+): ([a-z]+).*/\1 \2/" | sort -u
echo "== by workload and mode"
cat test_$ID/logs/*.testcase 2>/dev/null | sed -E "s/^[0-9:]+> //" | grep -E "^OA-RESULT" | sed "s/^OA-RESULT //"
echo "== throughput"
cat test_$ID/logs/*.testcase 2>/dev/null | sed -E "s/^[0-9:]+> //" | grep -E "^OA-SUMMARY" | sed "s/^OA-SUMMARY //"
echo "== failures"
cat test_$ID/logs/*.testcase 2>/dev/null | sed -E "s/^[0-9:]+> //" | grep -E "^FAILED|failed with rc" | sort | uniq -c | sort -rn | head -20
! grep -qE "^![^:]+: (failed|timeout)" $O

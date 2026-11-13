#!/bin/bash
# Stage 1.1 on one standalone node: single-row runs with the deadlock detector on and
# off for waits for the apply turn (ordered_apply_turn_detect), serial and early.
cd /tmp/rlbench
D="docker exec rlbench-dev-1"
send() { $D cdb2sql -s testdb --host node1 "exec procedure sys.cmd.send('$1')" >/dev/null; }
snap() { $D cdb2sql -s --tabs testdb --host node1 "select value from comdb2_metrics where name in ('ordered_apply_txns','ordered_apply_waits','ordered_apply_wait_us','ordered_apply_held_us','ordered_apply_parallel','ordered_apply_fallbacks','ordered_apply_apply_us') order by name" | tr '\n' ' '; }
i=0
run() {
  i=$((i+1)); echo "STEP $i/19 $*"
  b=($(snap)); python3 bench_cluster.py "$@"; a=($(snap))
  t=$((a[4]-b[4])); [ $t -gt 0 ] && echo "     plan: txns=$t apply-us/txn=$(( (a[0]-b[0]) / t )) held-us/txn=$(( (a[2]-b[2]) / t )) wait-us/txn=$(( (a[5]-b[5]) / t ))"
}
NODES=1 ./newcluster.sh "" "maxwt 32;cachekb 4194304" 2>&1 | tail -4
send "ordered_apply 1"; send "ordered_apply_threads 3"
echo "## preload"; run rand 16 500 375
for cfg in "1 1" "1 0" "0 1" "0 0"; do
  set -- $cfg
  send "ordered_apply 1"; send "ordered_apply_serial_commit $1"; send "ordered_apply_turn_detect $2"
  echo "## serial_commit=$1 turn_detect=$2"
  run rand 32 1 2000; run rand 64 1 2000; run mono 32 1 2000; run rand 32 100 60
done
send "ordered_apply 0"; echo "## page locks"; run rand 32 1 2000; run rand 64 1 2000
$D cdb2sql -s --tabs testdb --host node1 "exec procedure sys.cmd.verify('t')" | tail -1
echo "ALLDONE"

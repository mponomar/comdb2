#!/bin/bash
# Updates and deletes with ordered apply, against page locks.  async_dist_commit
# on throughout.  NODES=1 in the environment runs it on one standalone node.
#   pg     ordered_apply off (page locks held from each write to commit)
#   unit   ordered_apply on with commit units, updates and deletes planned
#   unit-i unit, but updates and deletes not planned (ordered_apply_update off):
#          they run under page locks next to planned inserts
# VARIANTS in the environment picks which to run, in order.  KEYS is the number
# of rows loaded with a = 1..KEYS for the workloads to pick from.
cd /tmp/rlbench
D="docker exec rlbench-dev-1"
VARIANTS=${VARIANTS:-"pg unit"}
export KEYS=${KEYS:-1000000}
TOTAL=$(( $(echo $VARIANTS | wc -w) * 9 )); i=0
send() { $D cdb2sql -s testdb --host $m "exec procedure sys.cmd.send('$1')" >/dev/null; }
cfg() {
  send "ordered_apply_unit 0"; send "ordered_apply_update 1"
  case $1 in
    pg)     send "ordered_apply 0";;
    unit)   send "ordered_apply 1"; send "ordered_apply_threads 3"; send "ordered_apply_unit 1";;
    unit-i) send "ordered_apply 1"; send "ordered_apply_threads 3"; send "ordered_apply_unit 1"; send "ordered_apply_update 0";;
  esac
  echo "## config=$1"
}
NAMES="'ordered_apply_txns','ordered_apply_units','ordered_apply_parallel','ordered_apply_fallbacks','ordered_apply_apply_us','ordered_apply_stale_retries','ordered_apply_read_locked','ordered_apply_dup_retries','ordered_apply_unit_retries'"
snap() { $D cdb2sql -s --tabs testdb --host $m "select value from comdb2_metrics where name in ($NAMES) order by name" | tr '\n' ' '; }
run() {
  i=$((i+1)); echo "STEP $i/$TOTAL $*"
  b=($(snap)); python3 bench_cluster.py "$@"; a=($(snap))
  [ ${#a[@]} -eq 9 ] || { echo "FAILED: the master is not answering"; exit 1; }
  # order: apply_us dup_retries fallbacks parallel read_locked stale_retries txns unit_retries units
  t=$((a[6]-b[6])); [ $t -gt 0 ] && echo "     plan: txns=$t units=$((a[8]-b[8])) parallel=$((a[3]-b[3])) fallbacks=$((a[2]-b[2])) stale=$((a[5]-b[5])) read-locked=$((a[4]-b[4])) dup=$((a[1]-b[1])) unit-retries=$((a[7]-b[7])) apply-us/txn=$(( (a[0]-b[0]) / t ))"
}
check() {
  sleep 3
  for n in $(docker ps --filter name=rlbench-node --format '{{.Names}}' | cut -d- -f2 | sort); do echo -n "$n: "; $D cdb2sql -s --tabs testdb --host $n "select count(*), sum(a % 1000003), sum(b % 1000003), sum(c % 1000003), sum(length(pad)) from t"; done
  $D cdb2sql -s --tabs testdb --host $m "exec procedure sys.cmd.verify('t')" | tail -1
}
./newcluster.sh "" "maxwt 32;cachekb 4194304" 2>&1 | tail -4
m=$($D cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='Y'"); m=${m:-node1}
rm -f /tmp/rlbench/scripts/del.next
echo "## load $KEYS rows, a = 1..$KEYS"
send "ordered_apply 1"; send "ordered_apply_unit 1"
t0=$(date +%s)
for lo in $(seq 1 20000 $KEYS); do
  echo "insert into t(a,b,c,pad) select value, (value * 7919) % 1000003, (value * 104729) % 1000033, 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' from generate_series($lo, $((lo + 19999)))"
done > /tmp/rlbench/scripts/load.sql
$D cdb2sql -s testdb default -f /scripts/load.sql > /dev/null
echo "loaded in $(( $(date +%s) - t0 ))s: $($D cdb2sql -s --tabs testdb default 'select count(*) from t') rows"
for c in $VARIANTS; do
  cfg $c
  run upd 1 1 2000; run upd 32 1 2000; run updk 32 1 2000; run upd 64 1 2000
  run upd 32 100 60; run updk 32 100 60
  run updhot 32 1 1000
  run del 32 1 2000; run del 32 100 60
done
check
echo "ALLDONE"

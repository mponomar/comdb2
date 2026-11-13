#!/bin/bash
# Stage 1.1 benchmark, all variants.  async_dist_commit on throughout.
# NODES=1 in the environment runs it on one standalone node.
#   pg        ordered_apply off (page locks held from each write to commit)
#   serial    ordered_apply on, one thread applies a plan, next plan waits for commit
#   par       serial + a btree file per thread for plans of 64+ operations
#   par-early par, but the next plan starts once this one is applied
cd /tmp/rlbench
D="docker exec rlbench-dev-1"
TOTAL=${TOTAL:-41}; i=0
send() { $D cdb2sql -s testdb --host $m "exec procedure sys.cmd.send('$1')" >/dev/null; }
cfg() {
  case $1 in
    pg)        send "ordered_apply 0";;
    serial)    send "ordered_apply 1"; send "ordered_apply_threads 0"; send "ordered_apply_serial_commit 1";;
    par)       send "ordered_apply 1"; send "ordered_apply_threads 3"; send "ordered_apply_serial_commit 1";;
    par-early) send "ordered_apply 1"; send "ordered_apply_threads 3"; send "ordered_apply_serial_commit 0";;
  esac
  echo "## config=$1"
}
snap() { $D cdb2sql -s --tabs testdb --host $m "select value from comdb2_metrics where name in ('ordered_apply_txns','ordered_apply_waits','ordered_apply_wait_us','ordered_apply_held_us','ordered_apply_parallel','ordered_apply_fallbacks','ordered_apply_apply_us') order by name" | tr '\n' ' '; }
run() {
  i=$((i+1)); echo "STEP $i/$TOTAL $*"
  b=($(snap)); python3 bench_cluster.py "$@"; a=($(snap))
  # order: apply_us fallbacks held_us parallel txns wait_us waits
  t=$((a[4]-b[4])); [ $t -gt 0 ] && echo "     plan: txns=$t parallel=$((a[3]-b[3])) fallbacks=$((a[1]-b[1])) apply-us/txn=$(( (a[0]-b[0]) / t )) held-us/txn=$(( (a[2]-b[2]) / t )) wait-us/txn=$(( (a[5]-b[5]) / t )) turn-held-secs=$(( (a[2]-b[2]) / 1000000 ))"
}
check() {
  sleep 3
  for n in $(docker ps --filter name=rlbench-node --format '{{.Names}}' | cut -d- -f2 | sort); do echo -n "$n: "; $D cdb2sql -s --tabs testdb --host $n "select count(*), sum(a % 1000003), sum(b % 1000003), sum(c % 1000003) from t"; done
  $D cdb2sql -s --tabs testdb --host $m "exec procedure sys.cmd.verify('t')" | tail -1
}
./newcluster.sh "" "maxwt 32;cachekb 4194304" 2>&1 | tail -4
m=$($D cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='Y'"); m=${m:-node1}
for c in pg serial par par-early; do
  cfg $c
  run mono 1 1 2000; run mono 32 1 2000; run mono 1 100 60; run mono 8 100 60; run mono 32 100 60
done
check
echo "## preload random rows"
cfg par
run rand 16 500 375
for c in pg serial par par-early; do
  cfg $c
  run rand 32 1 2000; run rand 64 1 2000; run rand 1 100 60; run rand 8 100 60; run rand 32 100 60
done
check
echo "ALLDONE"

#!/bin/bash
# One large transaction against many small ones on the same rows: a client
# that updates ROWS rows in one transaction, over and over, and SMALL clients
# that each update one of those rows over and over, for SECS seconds.  Does
# each kind make progress?  async_dist_commit on throughout.  NODES=1 in the
# environment runs it on one standalone node.
#   pg       ordered_apply off
#   unit     ordered_apply with commit units; updates applied on one thread
#   unit-pN  unit, with updates applied a file per thread
#            (ordered_apply_parallel_updates) until a unit has been a deadlock
#            victim N times (ordered_apply_parallel_retries)
# VARIANTS in the environment picks which to run, in order.
cd /tmp/rlbench
D="docker exec rlbench-dev-1"
VARIANTS=${VARIANTS:-"pg unit unit-p1 unit-p2 unit-p4"}
ROWS=${ROWS:-10000}; SMALL=${SMALL:-10}; SECS=${SECS:-60}; WORK=${WORK:-updk}
send() { $D cdb2sql -s testdb --host $m "exec procedure sys.cmd.send('$1')" >/dev/null; }
cfg() {
  send "ordered_apply_unit 0"; send "ordered_apply_update 1"; send "ordered_apply_parallel_updates 0"; send "ordered_apply_parallel_retries 1"
  case $1 in
    pg)      send "ordered_apply 0";;
    unit)    send "ordered_apply 1"; send "ordered_apply_threads 3"; send "ordered_apply_unit 1";;
    unit-p*) send "ordered_apply 1"; send "ordered_apply_threads 3"; send "ordered_apply_unit 1"
             send "ordered_apply_parallel_updates 1"; send "ordered_apply_parallel_retries ${1#unit-p}";;
  esac
  echo "## config=$1"
}
check() {
  sleep 3
  for n in $(docker ps --filter name=rlbench-node --format '{{.Names}}' | cut -d- -f2 | sort); do echo -n "$n: "; $D cdb2sql -s --tabs testdb --host $n "select count(*), sum(a % 1000003), sum(b % 1000003), sum(c % 1000003), sum(length(pad)) from t"; done
  $D cdb2sql -s --tabs testdb --host $m "exec procedure sys.cmd.verify('t')" | tail -1
}
./newcluster.sh "" "maxwt 32;cachekb 4194304" 2>&1 | tail -4
m=$($D cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='Y'"); m=${m:-node1}
echo "## load $ROWS rows, a = 1..$ROWS"
send "ordered_apply 1"; send "ordered_apply_unit 1"
for lo in $(seq 1 20000 $ROWS); do
  hi=$((lo + 19999)); [ $hi -gt $ROWS ] && hi=$ROWS
  echo "insert into t(a,b,c,pad) select value, (value * 7919) % 1000003, (value * 104729) % 1000033, 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' from generate_series($lo, $hi)"
done > /tmp/rlbench/scripts/load.sql
$D cdb2sql -s testdb default -f /scripts/load.sql > /dev/null
echo "loaded: $($D cdb2sql -s --tabs testdb default 'select count(*) from t') rows"
i=0; TOTAL=$(echo $VARIANTS | wc -w)
for c in $VARIANTS; do
  cfg $c
  i=$((i+1)); echo "STEP $i/$TOTAL $c"
  python3 big-small.py $ROWS $SMALL $SECS $WORK
done
check
echo "ALLDONE"

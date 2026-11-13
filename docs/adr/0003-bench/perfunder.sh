#!/bin/bash
# On-CPU profile of the master, with full call graphs, broken down by what runs
# under the named functions (see foldperf.py).
# usage: perfunder.sh <label> <workload> <K> <M> <T> <function> [<function> ...]
set -u
label=$1; wl=$2; K=$3; M=$4; T=$5; shift 5
secs=10; hz=499
out=/tmp/rlbench/prof/$label; mkdir -p $out
cd /tmp/rlbench
DEV=rlbench-dev-1
master=$(docker exec $DEV cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='Y'"); master=${master:-node1}
mc=rlbench-$master-1
python3 bench_cluster.py $wl $K $M $T > $out/bench.txt 2>&1 &
bpid=$!
sleep 4
perf=$(docker exec $mc bash -c 'ls /usr/lib/linux-tools/*/perf 2>/dev/null | head -1')
docker exec $mc mkdir -p /db/prof
docker exec $mc bash -c "$perf record -F $hz --call-graph dwarf,16384 -p 1 -o /db/prof/perfu.data -- sleep $secs > /db/prof/perfu.record.out 2>&1"
wait $bpid
docker exec $mc bash -c "$perf script -i /db/prof/perfu.data -F comm,tid,ip,sym 2>/dev/null" | python3 foldperf.py $secs $hz "$@" > $out/under.txt
docker exec $mc bash -c "rm -f /db/prof/perfu.data"
cat $out/bench.txt; echo "profiled: $mc"; cat $out/under.txt

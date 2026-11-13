#!/bin/bash
# Flat on-CPU profile (by thread name and by symbol) of one node while a workload runs.
# usage: perfflat.sh <label> <workload> <K> <M> <T> [master|replicant]
set -u
label=$1; wl=$2; K=$3; M=$4; T=$5; who=${6:-master}
out=/tmp/rlbench/prof/$label; mkdir -p $out
cd /tmp/rlbench
DEV=rlbench-dev-1
master=$(docker exec $DEV cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='Y'"); master=${master:-node1}
rep=$(docker exec $DEV cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='N' limit 1")
if [ $who = master ]; then mc=rlbench-$master-1; else mc=rlbench-$rep-1; fi
python3 bench_cluster.py $wl $K $M $T > $out/bench.txt 2>&1 &
bpid=$!
sleep 3
perf=$(docker exec $mc bash -c 'ls /usr/lib/linux-tools/*/perf 2>/dev/null | head -1')
docker exec $mc mkdir -p /db/prof
docker exec $mc bash -c "$perf record -F 499 -p 1 -o /db/prof/perff.data -- sleep 8 > /db/prof/perff.record.out 2>&1"
wait $bpid
docker exec $mc bash -c "$perf report -i /db/prof/perff.data --stdio --no-children --sort comm 2>/dev/null | grep -v '^#' | grep -v '^$' | head -25" > $out/perff.comm.txt
docker exec $mc bash -c "$perf report -i /db/prof/perff.data --stdio --no-children --sort comm,symbol --percent-limit 0.6 2>/dev/null | grep -v '^#' | grep -v '^$' | head -70" > $out/perff.sym.txt
docker exec $mc bash -c "rm -f /db/prof/perff.data"
cat $out/bench.txt; echo "profiled: $mc ($who)"; cat $out/perff.comm.txt | head -14; echo "--"; cat $out/perff.sym.txt | head -45

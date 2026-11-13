#!/bin/bash
# On-CPU profile of the master with full (DWARF) call graphs while a workload runs.
# usage: perfdwarf.sh <label> <workload> <K> <M> <T> [node-to-profile: master|replicant]
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
sleep 4
perf=$(docker exec $mc bash -c 'ls /usr/lib/linux-tools/*/perf 2>/dev/null | head -1')
docker exec $mc mkdir -p /db/prof
docker exec $mc bash -c "$perf record -F 99 --call-graph dwarf,16384 -p 1 -o /db/prof/perfd.data -- sleep 12 > /db/prof/perfd.record.out 2>&1"
wait $bpid
docker exec $mc bash -c "$perf report -i /db/prof/perfd.data --stdio --children --sort symbol -g none --percent-limit 0.7 2>/dev/null | grep -v '^#' | grep -v '^$' | sed 's/ *-      -  *//' | head -260" > $out/perfd.children.txt
docker exec $mc bash -c "$perf report -i /db/prof/perfd.data --stdio --no-children --sort comm -g none 2>/dev/null | grep -v '^#' | grep -v '^$' | head -20" > $out/perfd.comm.txt
docker exec $mc bash -c "rm -f /db/prof/perfd.data"
cat $out/bench.txt; echo "profiled: $mc"; cat $out/perfd.comm.txt | head -8

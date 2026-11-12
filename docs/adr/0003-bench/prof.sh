#!/bin/bash
# Profile the rlbench docker cluster while a workload runs.
# usage: prof.sh <label> <workload> <K> <M> <T> [nsamples] [interval]
# On the master: wall-clock stack samples (gsample.py), on-CPU perf profile,
# lock-table and request-queue snapshots, berkdb region stats before/after.
# On one replicant: wall-clock stack samples.
# Output: /tmp/rlbench/prof/<label>/
set -u
label=$1; wl=$2; K=$3; M=$4; T=$5; ns=${6:-40}; iv=${7:-1.0}
out=/tmp/rlbench/prof/$label
rm -rf $out; mkdir -p $out/master $out/rep
cd /tmp/rlbench
DEV=rlbench-dev-1
master=$(docker exec $DEV cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='Y'")
rep=$(docker exec $DEV cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='N' limit 1")
mc=rlbench-$master-1; rc=rlbench-$rep-1
echo "master=$master replicant=$rep" | tee $out/info.txt
msql() { docker exec $DEV cdb2sql -s --tabs testdb --host $master "$1"; }
stats() {
    for c in "bdb lockstat" "bdb logstat" "bdb txnstat" "bdb cachestat"; do
        echo "== $c"; msql "exec procedure sys.cmd.send('$c')"
    done
}
stats > $out/stats.before.txt 2>&1
# attach the samplers first (symbol loading freezes the process once), then start the load
python3 gsample.py $mc $out/master $ns $iv 10 > $out/gsample.master.out 2>&1 &
gm=$!
python3 gsample.py $rc $out/rep $ns $iv 10 > $out/gsample.rep.out 2>&1 &
gr=$!
for i in $(seq 1 60); do grep -q READY $out/gsample.master.out 2>/dev/null && grep -q READY $out/gsample.rep.out 2>/dev/null && break; sleep 0.5; done
python3 bench_cluster.py $wl $K $M $T > $out/bench.txt 2>&1 &
bpid=$!
sleep 8
perf=$(docker exec $mc bash -c 'ls /usr/lib/linux-tools/*/perf 2>/dev/null | head -1')
docker exec $mc mkdir -p /db/prof
docker exec $mc bash -c "$perf record -F 199 -g -p 1 -o /db/prof/perf.data -- sleep 20 > /db/prof/perf.record.out 2>&1" &
pp=$!
i=0
while kill -0 $gm 2>/dev/null; do
    i=$((i+1))
    msql "select mode, status, object, locktype, page, count(*) from comdb2_locks where locktype='PAGE' group by 1,2,3,4,5" > $out/locks.$i.txt 2>&1
    msql "exec procedure sys.cmd.send('stal')" 2>&1 | grep -E "items on queue|wrt busy|maxwthreadpenalty" > $out/queue.$i.txt
    sleep 1.5
done
wait $gm; wait $gr; wait $pp
docker exec $mc bash -c "$perf report -i /db/prof/perf.data --stdio --no-children --sort symbol --percent-limit 0.4 -g none 2>/dev/null | grep -v '^#' | grep -v '^$' | head -120" > $out/perf.sym.txt
docker exec $mc bash -c "$perf report -i /db/prof/perf.data --stdio --no-children --sort comm --percent-limit 0.4 -g none 2>/dev/null | grep -v '^#' | grep -v '^$' | head -30" > $out/perf.comm.txt
docker exec $mc bash -c "$perf report -i /db/prof/perf.data --stdio --children --sort symbol --percent-limit 1.5 -g none 2>/dev/null | grep -v '^#' | grep -v '^$' | head -120" > $out/perf.children.txt
wait $bpid
stats > $out/stats.after.txt 2>&1
cat $out/bench.txt; cat $out/gsample.master.out | tail -2

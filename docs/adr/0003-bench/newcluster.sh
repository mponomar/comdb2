#!/bin/bash
# Recreate the rlbench cluster from scratch.  usage: newcluster.sh "<create-time lrl>" "<runtime lrl lines ;-separated>"
# NODES=1 in the environment creates one standalone node (no replication) in place of the three-node cluster.
# UNIQUE=1 gives the table a unique key.
cd /tmp/rlbench/docker
if [ "${NODES:-3}" = 1 ]; then f=compose.1node.yaml; else f=compose.yaml; fi
docker compose -p rlbench -f $f down -v --remove-orphans > /dev/null 2>&1
docker run --rm --entrypoint bash -v /tmp/rlbench/vol:/vol rlbench-comdb2:latest -c "rm -rf /vol/node1 /vol/node2 /vol/node3; mkdir -p /vol/node1 /vol/node2 /vol/node3"
EXTRA_LRL="$1" EXTRA_RUN_LRL="$2" docker compose -p rlbench -f $f up -d 2>&1 | tail -1
docker compose -p rlbench -f $f logs init 2>&1 | tail -1
D="docker exec rlbench-dev-1 cdb2sql -s testdb default"
$D "create table t(a bigint, b bigint, c bigint, pad varchar(64))"
# UNIQUE=1 makes the index on a unique; the benchmark never repeats a value of a
$D "create ${UNIQUE:+unique }index ta on t(a)"; $D "create index tb on t(b)"; $D "create index tc on t(c)"
m=$(docker exec rlbench-dev-1 cdb2sql -s --tabs testdb default "select host from comdb2_cluster where is_master='Y'")
m=${m:-node1}     # a standalone node has no cluster to report
echo "master=$m"
docker exec rlbench-dev-1 cdb2sql -s testdb --host $m "select name, value from comdb2_tunables where name in ('maxwt','cachekb')"
docker exec rlbench-dev-1 cdb2sql -s testdb --host $m "exec procedure sys.cmd.send('stat')" | grep -i rowlock

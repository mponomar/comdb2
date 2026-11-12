#!/bin/bash
# Recreate the rlbench cluster from scratch.  usage: newcluster.sh "<create-time lrl>" "<runtime lrl lines ;-separated>"
cd /tmp/rlbench/docker
docker compose -p rlbench down -v > /dev/null 2>&1
docker run --rm --entrypoint bash -v /tmp/rlbench/vol:/vol rlbench-comdb2:latest -c "rm -rf /vol/node1 /vol/node2 /vol/node3; mkdir -p /vol/node1 /vol/node2 /vol/node3"
EXTRA_LRL="$1" EXTRA_RUN_LRL="$2" docker compose -p rlbench up -d 2>&1 | tail -1
docker compose -p rlbench logs init 2>&1 | tail -1
D="docker exec rlbench-dev-1 cdb2sql -s testdb default"
$D "create table t(a bigint, b bigint, c bigint, pad varchar(64))"
$D "create index ta on t(a)"; $D "create index tb on t(b)"; $D "create index tc on t(c)"
m=$($D "select host from comdb2_cluster where is_master='Y'" | sed "s/.*='\(node[0-9]\)'.*/\1/")
echo "master=$m"
docker exec rlbench-dev-1 cdb2sql -s testdb --host $m "select name, value from comdb2_tunables where name in ('maxwt','cachekb')"
docker exec rlbench-dev-1 cdb2sql -s testdb --host $m "exec procedure sys.cmd.send('stat')" | grep -i rowlock

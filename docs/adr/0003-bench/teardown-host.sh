#!/bin/bash
# Remove everything setup-host.sh and the benchmarks created.
cd /tmp/rlbench/docker 2>/dev/null && docker compose -p rlbench down -v
docker run --rm --entrypoint bash -v /tmp/rlbench/vol:/vol rlbench-comdb2:latest -c "rm -rf /vol/node1 /vol/node2 /vol/node3"
docker rmi rlbench-comdb2:latest
rm -rf /tmp/rlbench

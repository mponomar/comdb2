#!/bin/bash
# Prepare /tmp/rlbench on this host and build the cluster image from the
# committed tree (git HEAD; uncommitted changes are not included).
# The image is the stock contrib/docker image plus gdb, elfutils and perf.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
W=/tmp/rlbench
mkdir -p $W/src $W/docker $W/scripts $W/prof $W/vol/node1 $W/vol/node2 $W/vol/node3
if git -C "$repo" rev-parse --git-dir > /dev/null 2>&1; then
    git -C "$repo" archive HEAD | tar -x -C $W/src
else
    tar -C "$repo" --exclude=./build -cf - . | tar -x -C $W/src
fi
python3 - <<'PY'
p = "/tmp/rlbench/src/contrib/docker/"
s = open(p + "Dockerfile.dev").read()
marker = "    iputils-ping \\\n"
assert marker in s, "Dockerfile.dev changed; add gdb/elfutils/linux-tools-generic to the runtime stage by hand"
open(p + "Dockerfile.prof", "w").write(s.replace(marker, marker + "    gdb \\\n    elfutils \\\n    linux-tools-generic \\\n", 1))
PY
cp $W/src/contrib/docker/cluster-entrypoint.sh $W/src/contrib/docker/client-entrypoint.sh $W/src/contrib/docker/testdb.cfg $W/docker/
cp "$here"/docker/* $W/docker/
cp "$here"/*.py "$here"/*.sh $W/
chmod +x $W/*.sh
cd $W/docker && docker compose -p rlbench build
echo "ready: cd $W && ./newcluster.sh \"\" \"maxwt 32;cachekb 4194304\""

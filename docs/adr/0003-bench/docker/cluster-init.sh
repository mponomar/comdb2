#!/bin/bash
# One-shot cluster initializer (benchmark copy): creates testdb with optional
# extra create-time lrl lines ($EXTRA_LRL) and seeds every node's data directory.
set -euo pipefail
DB=testdb
NODES="node1 node2 node3"
if [ -f "/nodes/node1/${DB}.lrl" ]; then
    echo "[init] ${DB} already initialized; nothing to do"
    exit 0
fi
mkdir -p /nodes/node1
cat > /tmp/create.lrl <<EOL
name ${DB}
dir /nodes/node1
${EXTRA_LRL:-}
EOL
comdb2 --create --lrl /tmp/create.lrl "$DB"
cat > "/tmp/${DB}.lrl" <<EOL
name    ${DB}
dir     /db
cluster nodes ${NODES}
EOL
# extra runtime lrl lines, ';'-separated (e.g. "maxwt 32;cachekb 4194304")
echo "${EXTRA_RUN_LRL:-}" | tr ';' '\n' >> "/tmp/${DB}.lrl"
for n in ${NODES}; do
    dest="/nodes/$n"
    mkdir -p "$dest"
    [ "$n" = node1 ] || cp -a /nodes/node1/. "$dest/"
    cp "/tmp/${DB}.lrl" "$dest/${DB}.lrl"
done
echo "[init] ${DB} initialized on: ${NODES} (extra: ${EXTRA_LRL:-none})"

#!/usr/bin/env bash
# Shared by the oa_*.test tests: transactions under contention, for ordered
# apply (docs/adr/0003).  Each test runs its workload once for each mode in
# OA_MODES, in one database, and checks an invariant that does not depend on
# the mode:
#   pg       ordered_apply off: page locks held from each write to commit
#   unit     ordered_apply with commit units; plans that update or delete
#            are applied on one thread
#   unitpar  unit, with those plans applied a file per thread, and a unit
#            split by file from OA_PAR_MIN_OPS operations up: 4 if not set,
#            to have small transactions on several threads; the default of
#            ordered_apply_parallel_min_ops is 64
# Every run of clients is watched: if the master commits nothing for
# OA_STALL_SECS while clients are still running, the lock table is saved and
# the test fails.  A line starting OA-RESULT is printed for each workload and
# mode; docs/adr/0003-bench/adr-tests.sh collects them.
#
# The tests need disable_tagged_api_writes in lrl.options: ordered_apply is
# refused without it.

OA_MODES=${OA_MODES:-"pg unit unitpar"}
OA_STALL_SECS=${OA_STALL_SECS:-60}
OA_CDB2SQL=${CDB2SQL_EXE:-cdb2sql}
OA_METRICS="commits deadlocks retries verify_replays ordered_apply_txns ordered_apply_units ordered_apply_unit_retries ordered_apply_unit_serial ordered_apply_parallel ordered_apply_fallbacks ordered_apply_stale_retries"

oa_fail() {
    echo "FAILED: $*" >&2
    echo "Failed $*" > ${DBNAME:-$OA_DB}.failexit # runtestcase looks for this
    exit 1
}

oa_sql() { $OA_CDB2SQL --tabs ${CDB2_OPTIONS} $OA_DB default "$1"; }
oa_master_sql() { $OA_CDB2SQL --tabs ${CDB2_OPTIONS} $OA_DB --host $OA_MASTER "$1"; }
oa_send() { $OA_CDB2SQL ${CDB2_OPTIONS} $OA_DB --host $OA_MASTER "exec procedure sys.cmd.send('$1')" > /dev/null; }

# oa_init <dbname> <test name>
oa_init() {
    OA_DB=$1
    OA_TEST=$2
    [ -z "$OA_DB" ] && { echo "usage: runit <dbname>" >&2; exit 1; }
    OA_MASTER=$(oa_sql "select host from comdb2_cluster where is_master='Y'")
    [ -z "$OA_MASTER" ] && oa_fail "could not find the master"
    OA_DIR=${TMPDIR:-/tmp}/$OA_TEST.$OA_DB.$$
    rm -rf $OA_DIR
    mkdir -p $OA_DIR
}

oa_mode() {
    oa_send "ordered_apply_unit 0"
    oa_send "ordered_apply_parallel_updates 0"
    oa_send "ordered_apply_parallel_min_ops 64"
    case $1 in
    pg) oa_send "ordered_apply 0" ;;
    unit)
        oa_send "ordered_apply 1"
        oa_send "ordered_apply_unit 1"
        ;;
    unitpar)
        oa_send "ordered_apply 1"
        oa_send "ordered_apply_unit 1"
        oa_send "ordered_apply_parallel_updates 1"
        # the transactions here are small: split any unit of a few operations
        oa_send "ordered_apply_parallel_min_ops ${OA_PAR_MIN_OPS:-4}"
        ;;
    *) oa_fail "unknown mode $1" ;;
    esac
    local want=ON
    [ $1 = pg ] && want=OFF
    [ "$(oa_master_sql "select value from comdb2_tunables where name='ordered_apply'")" = $want ] ||
        oa_fail "mode $1: ordered_apply is not $want"
    OA_MODE=$1
}

# the master's counters, a name and a value to a line
oa_snap() {
    local names
    names=$(echo $OA_METRICS | sed "s/ /','/g")
    oa_master_sql "select name, cast(value as int) from comdb2_metrics where name in ('$names')" > $1
}

# oa_delta <before> <after> <name>
oa_delta() {
    awk -v n=$3 'NR == FNR { if ($1 == n) b = $2; next } $1 == n { a = $2 } END { print a - b }' $1 $2
}

oa_now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }

oa_commits() { oa_master_sql "select cast(value as int) from comdb2_metrics where name='commits'"; }

# oa_run_clients <dir>: runs every <dir>/*.sql through cdb2sql at once, the
# output of x.sql in x.out and its errors in x.err, and waits for them.  Sets OA_SECS, and leaves the
# master's counters before and after in <dir>/before and <dir>/after.
oa_run_clients() {
    local dir=$1 pids="" f p alive last now moved looked commits t0 t1
    oa_snap $dir/before
    t0=$(oa_now)
    for f in $dir/*.sql; do
        $OA_CDB2SQL ${CDB2_OPTIONS} $OA_DB default -f $f > ${f%.sql}.out 2> ${f%.sql}.err &
        pids="$pids $!"
    done
    last=$(oa_commits)
    moved=$(date +%s)
    looked=$moved
    while :; do
        alive=0
        for p in $pids; do
            kill -0 $p 2> /dev/null && alive=1
        done
        [ $alive = 0 ] && break
        sleep 0.2
        now=$(date +%s)
        [ $((now - looked)) -lt 5 ] && continue
        looked=$now
        commits=$(oa_commits)
        if [ "$commits" != "$last" ]; then
            last=$commits
            moved=$now
        elif [ $((now - moved)) -ge $OA_STALL_SECS ]; then
            oa_master_sql "exec procedure sys.cmd.send('bdb lockinfo lockers')" > $dir/stall.lockers 2>&1
            cp $dir/stall.lockers ${TESTDIR:-.}/logs/$OA_DB.stall.lockers 2> /dev/null
            for p in $pids; do
                kill $p 2> /dev/null
            done
            oa_fail "$OA_TEST $OA_MODE: no commit in ${OA_STALL_SECS}s with clients running; lock table in $dir/stall.lockers"
        fi
    done
    wait
    t1=$(oa_now)
    OA_SECS=$(echo "$t1 $t0" | awk '{ printf "%.2f", $1 - $2 }')
    oa_snap $dir/after
}

# What cdb2sql was told, from its output for a file of statements.  It prints
# "[statement] rc 0" for each that succeeds and "(rows updated=N)" and the
# like for each write outside a transaction, and on stderr "[statement]
# failed with rc ..." for each that does not.  The two are kept apart because
# they are not written in step.  A transaction whose commit fails (another
# changed a row it read: "uncommittable txn") is not run again by cdb2sql, as
# an application would; it counts as failed, and its statements do not count.
# The functions take the .out files and find the .err files beside them.

# statements that succeeded and whose transaction committed: those between a
# begin and a commit count only if the commit succeeded
oa_acked() {
    awk '
        FNR == 1 { intxn = 0; cur = 0 }
        /^\[begin\] rc 0/ { intxn = 1; cur = 0; next }
        /^\[commit\] rc 0/ { total += cur; intxn = 0; cur = 0; next }
        /^\[(set|select) / { next }
        /\] rc 0$/ { if (intxn) cur++; else total++ }
        END { print total + 0 }' "$@"
}
# transactions that committed: commits, and statements outside a transaction
oa_committed() {
    awk '
        FNR == 1 { intxn = 0 }
        /^\[begin\] rc 0/ { intxn = 1; next }
        /^\[commit\] rc 0/ { total++; intxn = 0; next }
        /^\[(set|select) / { next }
        /\] rc 0$/ { if (!intxn) total++ }
        END { print total + 0 }' "$@"
}
# rows the client was told it changed: oa_rows updated|deleted|inserted <files>
oa_rows() {
    local what=$1
    shift
    cat "$@" | sed -n "s/^(rows $what=\([0-9]*\))$/\1/p" | awk '{ s += $1 } END { print s + 0 }'
}
oa_errfiles() {
    local f
    for f in "$@"; do echo ${f%.out}.err; done
}
oa_failures() { cat $(oa_errfiles "$@") | grep -c "failed with rc"; }
# failures, by message with the ids taken out, most frequent first
oa_failure_kinds() {
    cat $(oa_errfiles "$@") | grep "failed with rc" | sed -e 's/^\[\([a-z]*\).*\] failed/[\1] failed/' -e 's/genid=[0-9a-f]*/genid=X/' -e 's/[0-9][0-9]*/N/g' | sort | uniq -c | sort -rn | head -5
}

# oa_report <workload> <transactions> [name=value ...]: one line for the
# workload just run by oa_run_clients in <dir>, with the master's counters
oa_report() {
    local dir=$1 work=$2 txns=$3
    shift 3
    local rate
    rate=$(echo "$txns $OA_SECS" | awk '{ printf "%.0f", ($2 > 0) ? $1 / $2 : 0 }')
    echo "OA-RESULT test=$OA_TEST work=$work mode=$OA_MODE txns=$txns secs=$OA_SECS txn/s=$rate" \
        "deadlocks=$(oa_delta $dir/before $dir/after deadlocks)" \
        "retries=$(oa_delta $dir/before $dir/after retries)" \
        "verify_replays=$(oa_delta $dir/before $dir/after verify_replays)" \
        "planned=$(oa_delta $dir/before $dir/after ordered_apply_txns)" \
        "units=$(oa_delta $dir/before $dir/after ordered_apply_units)" \
        "unit_retries=$(oa_delta $dir/before $dir/after ordered_apply_unit_retries)" \
        "unit_serial=$(oa_delta $dir/before $dir/after ordered_apply_unit_serial)" \
        "parallel=$(oa_delta $dir/before $dir/after ordered_apply_parallel)" \
        "fallbacks=$(oa_delta $dir/before $dir/after ordered_apply_fallbacks)" "$@"
}

# a mode with ordered apply on must have planned something
oa_expect_planned() {
    local dir=$1
    [ $OA_MODE = pg ] && return 0
    [ "$(oa_delta $dir/before $dir/after ordered_apply_txns)" -gt 0 ] ||
        oa_fail "$OA_TEST $OA_MODE: nothing was planned"
}

# oa_check_table <table> <query>: the table verifies, and the query, which
# should sum up its rows, gives the same answer on every node.  A replicant
# may be behind, so it gets a minute to agree.
oa_check_table() {
    local t=$1 q=$2 n want got i
    oa_master_sql "exec procedure sys.cmd.verify('$t')" > $OA_DIR/verify.$t 2>&1
    grep -q "Verify succeeded" $OA_DIR/verify.$t || {
        tail -5 $OA_DIR/verify.$t >&2
        oa_fail "$OA_TEST: verify of $t failed"
    }
    want=$(oa_master_sql "$q")
    [ -z "$want" ] && oa_fail "$OA_TEST: no answer from the master to: $q"
    for n in $(oa_sql "select host from comdb2_cluster"); do
        [ $n = $OA_MASTER ] && continue
        for i in $(seq 1 60); do
            got=$($OA_CDB2SQL --tabs ${CDB2_OPTIONS} $OA_DB --host $n "$q" 2> /dev/null)
            [ "$got" = "$want" ] && break
            sleep 1
        done
        [ "$got" = "$want" ] || oa_fail "$OA_TEST: table $t on $n is ($got), on the master ($want)"
    done
}

oa_done() {
    oa_mode pg
    rm -rf $OA_DIR
    echo "Success"
}

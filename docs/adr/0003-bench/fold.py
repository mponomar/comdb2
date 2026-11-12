#!/usr/bin/env python3
"""Aggregate gdb all-thread stack samples into a wall-clock profile.

usage: fold.py <dir with stack.N.txt> [topN]

Prints (1) threads per sample in each state, classified by the rules below
(first match wins, checked innermost-frame first where it matters), and
(2) the topN most common raw stack signatures.
"""
import collections, glob, re, sys

FRAME = re.compile(r"^#\d+\s+(?:0x[0-9a-f]+ in )?([^\s(]+)")
WRAPPERS = {"futex_wait", "futex_abstimed_wait", "__GI___futex_abstimed_wait_cancelable64", "__pthread_cond_wait_common",
            "___pthread_cond_wait", "___pthread_cond_timedwait", "__pthread_cond_timedwait", "__GI___poll",
            "__GI___clock_nanosleep", "__GI___nanosleep", "start_thread", "clone3", "__lll_lock_wait",
            "___pthread_mutex_lock", "__GI___lll_lock_wait", "lll_mutex_lock_optimized", "__pthread_mutex_cond_lock",
            "??", "__futex_abstimed_wait_common", "__futex_abstimed_wait_common64", "futex_abstimed_wait_cancelable",
            "__GI___pthread_mutex_lock", "__GI_epoll_wait", "__GI___libc_write", "__GI___writev"}
BLOCKED = ("pthread_cond_wait", "pthread_cond_timedwait", "__lll_lock_wait", "pthread_mutex_lock", "futex_wait",
           "__futex_abstimed_wait_common", "pthread_rwlock", "nanosleep", "usleep", "poll", "epoll_wait")


def has(st, name, depth=None):
    s = st if depth is None else st[:depth]
    return any(name in f for f in s)


def blocked(st):
    return any(any(b in f for b in BLOCKED) for f in st[:5])


def classify(st):
    w = has(st, "thd_req") or has(st, "toblock") or has(st, "handle_op_local")  # block processor (writer) thread
    if has(st, "handle_op_local", 8) and (has(st, "usleep", 5) or has(st, "nanosleep", 5)):
        return "writer: deadlock backoff sleep (handle_op_local)"
    if has(st, "__lock_get"):
        if blocked(st):
            return "writer: blocked on a lock (__lock_get)" if w else "other: blocked on a lock (__lock_get)"
        return "writer: lock manager, running" if w else "other: lock manager, running"
    if has(st, "__lock_detect") or has(st, "__dd_"):
        return "deadlock detector"
    if has(st, "bdb_wait_for_seqnum"):
        return "writer: waiting for replicant acks"
    if has(st, "__log_put") or has(st, "__log_flush") or has(st, "__log_write"):
        if has(st, "berkdb_send_rtn") or has(st, "__rep_send_message"):
            return "log: sending to replicants inside __log_put"
        return "log: __log_put blocked (region mutex)" if blocked(st) else "log: __log_put running"
    if has(st, "bdb_osql_trn_repo_lock"):
        return "commit: repo lock"
    if has(st, "__lock_vec") or has(st, "__lock_put") or has(st, "__lock_freefamilylocker") or has(st, "__lock_id"):
        return "writer: lock manager, running" if not blocked(st) else "writer: lock manager, blocked on its mutex"
    if has(st, "__txn_"):
        return "txn begin/commit/abort"
    if has(st, "__memp_"):
        return "buffer pool (__memp_*)" + (" blocked" if blocked(st) else "")
    if has(st, "__bam_") or has(st, "__db_pitem") or has(st, "__db_ditem") or has(st, "__db_c_") or has(st, "__db_put") or has(st, "__db_del"):
        return "btree code, running" if w else "btree code (non-writer)"
    if has(st, "toblock_main") or has(st, "osql_bplog_commit") or has(st, "apply_changes") or has(st, "add_record") or \
            has(st, "upd_record") or has(st, "del_record") or has(st, "toblock"):
        return "block processor other" + (" (blocked)" if blocked(st) else "")
    if has(st, "osql_bplog_saveop") or has(st, "bdb_temp_table") or has(st, "osql_sess_rcvop") or has(st, "net_osql"):
        return "bplog receive / temp tables"
    if has(st, "osql_chkboard") or has(st, "osql_sock_commit"):
        return "sql thread: waiting for master to commit"
    if has(st, "sqlite3") or has(st, "sqlengine_work") or has(st, "execute_sql_query") or has(st, "handle_sqlite"):
        return "sql engine"
    if has(st, "processor_thd") or has(st, "worker_thd") or has(st, "__rep_process_txn") or has(st, "__rep_apply"):
        return "rep: applying log" + (" (blocked)" if blocked(st) else "")
    if has(st, "__rep_process_message") or has(st, "process_berkdb") or has(st, "berkdb_receive"):
        return "rep: receiving/processing messages" + (" (blocked)" if blocked(st) else "")
    if has(st, "thd_req") and blocked(st):
        return "idle: writer thread waiting for work"
    if has(st, "thdpool_thd") and blocked(st):
        return "idle: pool thread waiting for work"
    if has(st, "net_dispatch") or has(st, "event_base_loop") or has(st, "rd_worker") or has(st, "akq"):
        return "net event loops / readers" + ("" if blocked(st) else " (running)")
    if blocked(st):
        return "idle: housekeeping threads"
    return "other, running"


def parse(fn):
    threads, cur = [], None
    for line in open(fn, errors="replace"):
        if line.startswith("Thread "):
            cur = []
            threads.append(cur)
        elif cur is not None:
            m = FRAME.match(line)
            if m:
                cur.append(m.group(1))
    return [t for t in threads if t]


def main():
    d = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    counts, sigs, n = collections.Counter(), collections.Counter(), 0
    for fn in sorted(glob.glob(d + "/stack.*.txt")):
        ts = parse(fn)
        if not ts:
            continue
        n += 1
        for st in ts:
            counts[classify(st)] += 1
            sigs[" < ".join([f for f in st if f not in WRAPPERS][:9])] += 1
    print("%d samples, %.1f threads/sample" % (n, sum(counts.values()) / max(1, n)))
    for lab, c in counts.most_common():
        print("%7.2f  %s" % (c / max(1, n), lab))
    if top:
        print("-- top %d stack signatures (threads per sample)" % top)
        for s, c in sigs.most_common(top):
            print("%7.2f  %s" % (c / max(1, n), s))


if __name__ == "__main__":
    main()

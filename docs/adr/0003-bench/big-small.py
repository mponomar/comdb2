#!/usr/bin/env python3
"""One large transaction against many small ones on the same rows.

usage: big-small.py <rows in the large transaction> <small clients> <seconds> [upd|updk]

One client updates ROWS rows (a = 1..ROWS) in one statement, which is one
transaction, again and again.  Each of the small clients updates one row
of that set again and again, a statement a transaction; client i has the row
with a = (i + 1) * ROWS / (clients + 1).  All of them run for the given time
and are then stopped.  The question is whether both kinds make progress.

upd changes a column with no index, updk (the default) column b, which has
one.  Needs rows with a = 1..ROWS; ordered-apply-bigsmall.sh loads them.
Clients run in the dev container against the "default" tier, as
bench_cluster.py's do.
"""
import os, re, subprocess, sys, time

DEV = "rlbench-dev-1"
SDIR = "/tmp/rlbench/scripts"
NAMES = ["commits", "deadlocks", "retries", "verify_replays", "ordered_apply_txns", "ordered_apply_units",
         "ordered_apply_unit_retries", "ordered_apply_unit_serial", "ordered_apply_parallel", "ordered_apply_fallbacks",
         "ordered_apply_stale_retries", "ordered_apply_read_locked"]


def sql(q, host=None):
    cmd = ["docker", "exec", DEV, "cdb2sql", "-s", "--tabs", "testdb"] + (["--host", host] if host else ["default"]) + [q]
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def metrics(host):
    q = "select name, value from comdb2_metrics where name in (%s)" % ",".join("'%s'" % m for m in NAMES)
    res = {}
    for line in sql(q, host).splitlines():
        p = line.split("\t")
        if len(p) == 2:
            try:
                res[p[0]] = float(p[1])
            except ValueError:
                pass
    return res


def main():
    rows, K, secs = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    workload = sys.argv[4] if len(sys.argv) > 4 else "updk"
    setcol = (lambda v: "b = %d" % v) if workload == "updk" else (lambda v: "pad = 'p%d'" % v)
    for f in os.listdir(SDIR):
        if f.startswith("bs."):
            os.unlink(os.path.join(SDIR, f))
    # more work than fits in the time; the clients are stopped
    with open(SDIR + "/bs.big.sql", "w") as f:
        for t in range(1, 1 + secs * 20):
            f.write("update t set %s where a <= %d\n" % (setcol(t), rows))
    for w in range(K):
        a = (w + 1) * rows // (K + 1)
        with open(SDIR + "/bs.small%d.sql" % w, "w") as f:
            for t in range(secs * 3000):
                f.write("update t set %s where a = %d\n" % (setcol(-(t * 100 + w) - 1), a))
    m = sql("select host from comdb2_cluster where is_master='Y'").strip() or "node1"
    before = metrics(m)
    t0 = time.time()
    # /scripts is read-only in the container: each client's output comes back over docker exec
    procs = []
    for name in ["bs.big"] + ["bs.small%d" % w for w in range(K)]:
        out = open("%s/%s.out" % (SDIR, name), "w")
        procs.append(subprocess.Popen(["docker", "exec", DEV, "timeout", str(secs), "cdb2sql", "testdb", "default", "-f",
                                       "/scripts/%s.sql" % name], stdout=out, stderr=subprocess.STDOUT))
    for pr in procs:
        pr.wait()
    el = time.time() - t0
    after = metrics(m)

    def count(name):
        # cdb2sql echoes each statement with its rc
        ok = bad = 0
        errs = {}
        try:
            for line in open("%s/%s.out" % (SDIR, name), errors="replace"):
                line = line.strip()
                if line.endswith("] rc 0"):
                    ok += 1
                elif line and "rows updated" not in line:
                    bad += 1
                    k = re.sub(r"[0-9]+", "N", line)[:90]
                    errs[k] = errs.get(k, 0) + 1
        except OSError:
            pass
        return ok, bad, errs

    ok, bad, errs = count("bs.big")
    print("big   rows=%d secs=%.0f committed=%d failed=%d (%.2f s a transaction)" %
          (rows, el, ok, bad, el / ok if ok else float("inf")))
    for k, v in errs.items():
        print("      big error x%d: %s" % (v, k))
    sok = sbad = 0
    least = None
    serrs = {}
    for w in range(K):
        o, b, e = count("bs.small%d" % w)
        sok += o
        sbad += b
        least = o if least is None else min(least, o)
        for k, v in e.items():
            serrs[k] = serrs.get(k, 0) + v
    print("small clients=%d committed=%d (%.0f txn/s, slowest client %d) failed=%d" % (K, sok, sok / el, least or 0, sbad))
    for k, v in serrs.items():
        print("      small error x%d: %s" % (v, k))
    d = {k: int(after.get(k, 0) - before.get(k, 0)) for k in NAMES}
    print("      master: " + " ".join("%s=%d" % (k.replace("ordered_apply_", "oa_"), d[k]) for k in NAMES))
    sys.stdout.flush()


if __name__ == "__main__":
    main()

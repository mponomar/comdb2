#!/usr/bin/env python3
"""Concurrent-writer benchmark against the docker cluster (compose project rlbench), three nodes or one.

usage: bench_cluster.py <workload> <writers K> <rows per txn M> <txns per writer T>

Workloads:
  mono    inserts, ascending keys
  rand    inserts, random keys
  upd     updates of a column with no index, rows picked at random by key a in the
          upper half of 1..KEYS
  updk    updates of column b, which has an index, rows picked the same way
  updhot  as updk, but every client picks from the rows with a = 1..200
  del     deletes by key a, each key once: takes the next keys on each run, from
          1001 up to KEYS/2, so that it never removes a row the others update
The update and delete workloads need rows with a = 1..KEYS (the environment
variable, default 1000000), which ordered-apply-updates.sh loads.  A transaction
of M rows is M statements between begin and commit.

Clients run inside the dev container and connect to the cluster ("default" tier),
so SQL runs on replicants and the bplog is applied by the master.  Lock metrics
are read from the master.  With one standalone node (NODES=1 ./newcluster.sh) SQL
and the bplog both run on that node, and its CPU figure includes the SQL.
"""
import glob, json, os, random, subprocess, sys, time

DEV = "rlbench-dev-1"
METRICS = ["commits", "deadlocks", "retries", "lockrequests", "lockwaits", "lockwait_time", "verify_replays"]


def sql(q, host=None):
    cmd = ["docker", "exec", DEV, "cdb2sql", "-s", "--tabs", "testdb"]
    cmd += ["--host", host] if host else ["default"]
    cmd.append(q)
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def master():
    return sql("select host from comdb2_cluster where is_master='Y'").strip() or "node1"  # standalone: no cluster


def metrics(host):
    q = "select name, value from comdb2_metrics where name in (%s)" % ",".join("'%s'" % m for m in METRICS)
    res = {}
    for line in sql(q, host).splitlines():
        parts = line.split("\t")
        if len(parts) == 2:
            try:
                res[parts[0]] = float(parts[1])
            except ValueError:
                pass
    return res


def cputicks():
    """utime+stime (clock ticks) of comdb2 (pid 1) in each node container."""
    res = {}
    names = subprocess.run(["docker", "ps", "--filter", "name=rlbench-node", "--format", "{{.Names}}"],
                           capture_output=True, text=True).stdout.split()
    for n in sorted(x.split("-")[1] for x in names):
        out = subprocess.run(["docker", "exec", "rlbench-%s-1" % n, "cat", "/proc/1/stat"], capture_output=True, text=True).stdout
        f = out.rsplit(")", 1)[-1].split()
        res[n] = int(f[11]) + int(f[12])
    return res


def master_events(m, t0, t1):
    """Per-transaction timing recorded by the master's event log between t0 and t1 (epoch secs)."""
    sql("exec procedure sys.cmd.send('reql events flush')", m)
    out = subprocess.run(["docker", "exec", "rlbench-%s-1" % m, "bash", "-c",
                          "zcat -f /opt/bb/var/log/cdb2/testdb.events.* 2>/dev/null | grep '\"type\":\"txn\"'"],
                         capture_output=True, text=True, errors="replace").stdout
    n = tot = q = proc = lw = nlw = 0
    for line in out.splitlines():
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if not (t0 * 1e6 <= e.get("time", 0) <= (t1 + 1) * 1e6):
            continue
        p = e.get("perf", {})
        n += 1
        tot += p.get("tottime", 0)
        q += p.get("qtime", 0)
        proc += p.get("processingtime", 0)
        lw += p.get("lockwaittime", 0)
        nlw += p.get("lockwaits", 0)
    if not n:
        return "master-events: none"
    el = (t1 - t0) * 1e6
    return ("master-events n=%d avg-ms total=%.1f queue=%.1f processing=%.1f lockwait=%.1f | avg-concurrent queued=%.1f "
            "processing=%.1f lockwaiting=%.2f" % (n, tot / n / 1e3, q / n / 1e3, proc / n / 1e3, lw / n / 1e3, q / el, proc / el, lw / el))


def main():
    workload, K, M, T = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    run = int(time.time() * 1000) % 100000000
    sdir = "/tmp/rlbench/scripts"
    for old in glob.glob(sdir + "/c.w*.sql"):
        os.unlink(old)
    rnd = random.Random(run)
    keys = int(os.environ.get("KEYS", "1000000"))
    delnext = 1001
    if workload == "del":
        # each key is deleted once: carry on from where the last run stopped
        try:
            delnext = int(open(sdir + "/del.next").read())
        except (OSError, ValueError):
            delnext = 1001
        if delnext + K * T * M - 1 > keys // 2:
            sys.exit("del: not enough keys left (next %d of %d)" % (delnext, keys))
        with open(sdir + "/del.next", "w") as f:
            f.write("%d\n" % (delnext + K * T * M))
    for w in range(K):
        with open("%s/c.w%d.sql" % (sdir, w), "w") as f:
            for t in range(T):
                if workload in ("mono", "rand"):
                    rows = []
                    for r in range(M):
                        seq = (t * M + r) * K + w
                        a = run * 100000000 + seq if workload == "mono" else rnd.getrandbits(62)
                        rows.append("(%d,%d,%d,'%s')" % (a, rnd.getrandbits(62), rnd.getrandbits(62), "x" * 40))
                    f.write("insert into t(a,b,c,pad) values %s\n" % ",".join(rows))
                    continue
                stmts = []
                if workload == "del":
                    for r in range(M):
                        stmts.append("delete from t where a = %d" % (delnext + (t * M + r) * K + w))
                else:
                    picked = rnd.sample(range(1, 201) if workload == "updhot" else range(keys // 2 + 1, keys + 1), M)
                    for a in picked:
                        if workload == "upd":
                            stmts.append("update t set pad = '%s' where a = %d" % ("y" * rnd.randint(30, 40), a))
                        else:
                            stmts.append("update t set b = %d where a = %d" % (rnd.getrandbits(62), a))
                if M > 1:
                    stmts = ["begin"] + stmts + ["commit"]
                f.write("\n".join(stmts) + "\n")
    m = master()
    before = metrics(m)
    c0 = cputicks()
    t0 = time.time()
    subprocess.run(["docker", "exec", DEV, "bash", "-c",
                    "for f in /scripts/c.w*.sql; do cdb2sql -s testdb default -f $f > /dev/null 2>&1 & done; wait"])
    secs = time.time() - t0
    c1 = cputicks()
    after = metrics(m)
    ev = master_events(m, t0, t0 + secs)
    cores = {n: (c1[n] - c0[n]) / 100.0 / secs for n in c0}
    cpu = "cpu(cores) master=%.2f replicants=%s" % (cores[m], ",".join("%.2f" % cores[n] for n in sorted(cores) if n != m))
    d = {k: after.get(k, 0) - before.get(k, 0) for k in METRICS}
    txns = K * T
    print("%-4s K=%-3d M=%-4d txns=%-6d secs=%7.2f txn/s=%9.0f rows/s=%9.0f ms/txn=%6.2f | commits=%d deadlocks=%d retries=%d "
          "lockwaits=%d lockwait_ms=%.0f lockreq/row=%.1f master=%s %s" %
          (workload, K, M, txns, secs, txns / secs, txns * M / secs, 1000.0 * secs * K / txns, d["commits"], d["deadlocks"],
           d["retries"], d["lockwaits"], d["lockwait_time"] / 1000.0, d["lockrequests"] / max(1, txns * M), m, cpu))
    print("     " + ev)
    sys.stdout.flush()


if __name__ == "__main__":
    main()

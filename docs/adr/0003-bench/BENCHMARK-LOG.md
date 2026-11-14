# Benchmark log for ADR 0003

Every benchmark run for `docs/adr/0003-ordered-apply-with-row-intents.md`, in
the order they were made, with what changed in the code before each one. Add
an entry here for every new run, including ones that fail.

All runs are on `xeon` (Xeon E5-2699, 72 hardware threads) in Docker, with
database files on tmpfs, `cachekb 4194304` and `maxwt 32`. The table is
`t(a bigint, b bigint, c bigint, pad varchar(64))` with a non-unique index on
each of `a`, `b` and `c`. "Ascending" (`mono`) means `a` increases across all
clients; "random" (`rand`) means all three keys are random on a table
pre-loaded with millions of rows. A run is written as clients × rows per
transaction. Results files are in `results/`.

Runs 1–5 were made in earlier sessions. Their entries are reconstructed from
the ADR, the results files and the commit history, not from notes taken at the
time, so their dates are the dates of the commits that recorded them.

## Summary

Single-row inserts, 32 clients, random keys, transactions per second. This is
the workload the later runs were chasing.

| Run | Variant                                   | Nodes | Page locks | Variant |
|----:|-------------------------------------------|------:|-----------:|--------:|
|   5 | Turn, serial commit                       |     3 |      8,408 |   2,813 |
|   5 | Turn, released after apply ("early")      |     3 |      8,408 |   3,514 |
|   6 | Turn, released after apply                |     1 |      8,745 |   3,851 |
|   8 | Early, no deadlock detection for the turn |     1 |      9,527 |   4,571 |
|  10 | Commit units                              |     1 |      9,399 |  10,415 |
|  10 | Commit units                              |     3 |      8,707 |   8,322 |
|  12 | Commit units, own plan when alone         |     1 |      9,530 |  10,298 |
|  12 | Commit units, own plan when alone         |     3 |      8,652 |   8,029 |
|  14 | Commit units, table with a unique key     |     1 |      9,741 |  10,844 |
|  14 | Commit units, table with a unique key     |     3 |      8,714 |   7,759 |

Single-row inserts, one client, ascending keys, one node, transactions per
second.

| Run | Variant                             | Page locks | Early | Variant |
|----:|-------------------------------------|-----------:|------:|--------:|
|  10 | Commit units, always queued         |      1,026 |   995 |     870 |
|  11 | Commit units, own plan when idle    |        962 |   930 |     921 |
|  12 | Commit units, own plan when alone   |      1,027 |   952 |     945 |

100-row inserts, 32 clients, ascending keys, rows per second.

| Run | Variant                         | Nodes | Page locks | Variant |
|----:|---------------------------------|------:|-----------:|--------:|
|   1 | Existing rowlocks mode          |     3 |      5,700 |   7,900 |
|   4 | Turn, one thread, serial commit |     3 |      6,732 |  11,350 |
|   5 | Turn, a file per thread, early  |     3 |      6,732 |  27,239 |
|  10 | Commit units, a file per thread |     1 |      7,293 |  49,059 |
|  10 | Commit units, a file per thread |     3 |      6,387 |  25,648 |

## Runs

### 1. Baseline: page locks and the existing rowlocks mode

- **When:** recorded 2026-11-11 (commit `f9de2e4b8`).
- **Code:** upstream HEAD `82cae2c56`, no changes.
- **Setup:** three nodes. Page locks, and the existing rowlocks mode
  (`init_with_rowlocks`). Also `maxwt 1` and `maxwt 8`, thread-state sampling
  and `perf` profiles.
- **Results:** `head-pg-4g.results`, `head-row-4g.results`; the tables under
  Context in the ADR.
- **Found:** adding clients adds no throughput under page contention (100
  rows, 32 clients: 5,700 rows/s, 64% deadlock replays; one writer thread
  does 11,500). Single-row inserts stop near 9,000 txn/s because of the commit
  path, not locking. Rowlocks: 2,900 rows/s for one client against 6,900.

### 2. Stage 0: async_dist_commit (PR 6144)

- **When:** recorded 2026-11-12 (commit `7abd18b80`).
- **Change before:** upstream PR 6144 merged locally (`dbdea3e20`). A
  committed request is handed to one thread that waits for the replicant
  acks, so the writer thread does not.
- **Setup:** three nodes, `async_dist_commit` off and on; one run with a
  replicant paused for 8 seconds under load.
- **Results:** `stage0-async-dist-commit.results`.
- **Found:** about a third less master CPU (13.0 → 8.4 cores at 32 clients);
  single-row throughput unchanged near 9,000 txn/s. It is on in every later
  run.

### 3. Stage 1.1, serial apply

- **When:** recorded 2026-11-12 (commit `7abd18b80`).
- **Change before:** the first ordered-apply code (`bdb/plan.c`). A request
  records its inserts in a plan, takes the apply turn (a lock in the lock
  manager), applies the plan on its own thread, and keeps the turn to commit.
- **Setup:** three nodes, `ordered_apply` off and on.
- **Results:** `stage1-serial-apply.results`.
- **Found:** no deadlocks; 100-row transactions reach the `maxwt 1` rate
  (about 11,000 rows/s); single-row transactions lose more than half.

### 4–5. Stage 1.1, four variants

- **When:** recorded 2026-11-12 (commit `7abd18b80`).
- **Change before:** plans of 64 or more operations applied a btree file per
  thread under child transactions (`ordered_apply_threads`,
  `ordered_apply_parallel_min_ops`); `ordered_apply_serial_commit` to release
  the turn after apply.
- **Setup:** three nodes. `pg` (page locks), `serial`, `par` (a file per
  thread), `par-early` (par, turn released after apply).
  `ordered-apply-variants.sh`.
- **Results:** `stage1-four-variants.results`.
- **Found:** 100 rows, 32 clients: 6,732 (pg) → 11,350 → 26,004 → 27,239
  rows/s. One row, 32 clients, random: 8,408 (pg) → 2,813 → 2,753 → 3,514
  txn/s. The turn is held about 300 µs per single-row transaction.

### 6. The same four variants on one node

- **When:** 2026-11-13 02:23–02:38 UTC.
- **Code:** `7abd18b80`, no code change. Bench scripts gained `NODES=1`: one
  standalone node with no `cluster` line, so no replication.
- **Why:** to see whether replication limits single-row apply.
- **Command:** `NODES=1 ./ordered-apply-variants.sh`.
- **Results:** `stage1-single-node.results`.
- **Found:** it does not. One row, 32 clients, random: 8,745 (pg), 3,105
  (serial), 3,095 (par), 3,851 (early), within 4–13% of three nodes.

### 7. Profile of a serial single-row run

- **When:** 2026-11-13 02:41 UTC.
- **Code:** `7abd18b80`. New scripts `perfunder.sh` and `foldperf.py`.
- **Setup:** one node, `serial`, random, 32 × 1.
- **Results:** first half of `stage1-single-row-profile.txt`.
- **Found:** 38% of the CPU under `bdb_tran_plan_apply` is the lock manager
  call for the turn, 32% the deadlock detector, which runs on every wait.

### 8. The turn without deadlock detection

- **When:** 2026-11-13 02:47–02:56 UTC.
- **Change before:** `ordered_apply_turn_detect` (on by default) and the
  `DB_LOCK_NODETECT` lock flag; off makes a wait for the turn skip the
  detector. Not committed at the time of the run; committed in `d9529dd95`.
- **Setup:** one node; serial and early, detection on and off.
  `ordered-apply-turn.sh`.
- **Results:** `stage1-turn-detect.results`; second half of
  `stage1-single-row-profile.txt` (a profile with detection off, 03:13 UTC).
- **Found:** 16–27% more for single rows (early, random, 32 clients: 3,910 →
  4,571 txn/s), still about half of page locks (9,527). The remaining time is
  the four puts at two to three times their cost inside a large plan. Not
  safe to leave off.

### 9. Commit units, first attempt: crashed

- **When:** 2026-11-13 03:25–03:40 UTC.
- **Change before:** commit units (`ordered_apply_unit`): one thread applies
  all waiting plans and their blkseq records under one transaction and commits
  once. Uncommitted at the time.
- **Setup:** one node, then three; `pg`, `par-early`, `unit`, `unit-1t`.
- **Results:** `stage1-commit-units-run1-crashed.results`. Not usable.
- **What happened:** on both, the master aborted at the first 32-client unit
  run with `__txn_begin_int_int insane number of open txns for this td`. A
  unit applied a file per thread begins a child transaction for each btree
  file on one thread, and `TXN_TD_MAX` in `berkdb/txn/txn.c` allowed ten;
  single-row inserts from 32 clients reach eight data stripes and three
  indexes. On three nodes a replicant took over and the remaining "unit" runs
  ran under page locks, because the script kept sending settings to the dead
  master.

### 10. Commit units

- **When:** 2026-11-13 11:39–12:00 UTC.
- **Change before:** btree files put in at most seven groups, so at most
  seven child transactions. `ordered-apply-variants.sh` takes `VARIANTS` and
  stops if the master stops answering. Committed after the run as
  `e27a9c411`.
- **Setup:** one node, then three; `pg`, `par-early`, `unit` (a file per
  thread from 64 operations), `unit-1t` (never).
- **Command:** `NODES=1 VARIANTS="pg par-early unit unit-1t"
  ./ordered-apply-variants.sh`, then the same without `NODES=1`.
- **Results:** `stage1-commit-units.results`; the Commit units tables in the
  ADR.
- **Found:** one row, 32 clients: 10,353–10,840 txn/s on one node against
  8,932–9,399 for page locks; 8,322–9,487 on three nodes against 7,425–8,707,
  with a third less master CPU. 100 rows, 32 clients, ascending: 49,059
  rows/s on one node. A lone client is 9–13% slower than early release on one
  node (870 against 995 txn/s). No deadlocks or unit retries.

### 11. Commit units, a transaction applies its own plan when nothing is queued

- **When:** 2026-11-13 12:08–12:33 UTC.
- **Change before:** `ordered_apply_unit_inline` (on by default): a request
  that finds nothing queued and no unit being applied applies its own plan on
  its own thread, as a unit of one. Variant `unit-q` turns it off, which is
  run 10's `unit`. Seven file groups as in run 10. Uncommitted.
- **Setup:** one node, then three; `pg`, `par-early`, `unit-q`, `unit`.
- **Results:** `stage1-commit-units-inline.results`.
- **Found:** a lone client gets back to the early-release rate on one node:
  ascending, one row, 921 txn/s against 874 without it (early 930, page locks
  962); 100 rows, 11,388 against 11,179 rows/s. On three nodes a lone client
  is 507 txn/s either way. With 32 clients it costs up to 5%: one node,
  ascending, one row, 9,403 against 9,893; three nodes 8,697 against 9,083.
  Under load many units are still applied by their own transaction, on a
  thread with cold caches (about 40% of them in a local 32-client run; on
  xeon 80 µs a transaction in apply against 67).
- **Caveat:** the second agent's cluster (`rlbench-ad`) came up on the same
  host at about 12:28 UTC, during the three-node random-key steps. Its nodes
  were using about one core when checked. The three-node random-key figures
  may be affected. (`unit` is 16% below `unit-q` for ascending 100-row
  transactions with 32 clients on three nodes, which ran before that.)

### 12. Commit units, own plan only when it is the only one; a child per file

- **When:** 2026-11-13 12:45–13:10 UTC.
- **Change before:** two changes. (a) A request applies its own plan only
  when no other plan exists anywhere, counted from the start of prepare,
  where run 11 asked only that nothing be queued. Locally, 32 clients then
  apply 8% of units themselves and a lone client all of them. (b)
  `TXN_TD_MAX` raised from 10 to 64 and the seven-group cap removed, so every
  btree file in a unit gets its own child transaction (a plan touching more
  than 48 files is applied on one thread). Uncommitted.
- **Setup:** one node, then three; `pg`, `par-early`, `unit-q`, `unit`.
  `unit-q` against run 11's `unit-q` shows (b); `unit` against `unit-q` in
  this run shows (a).
- **Results:** `stage1-commit-units-lone.results`.
- **Found:** (a) a lone client on one node: ascending, one row, 945 txn/s
  against 858 always queued (early 952, page locks 1,027). With 32 clients
  on one node the cost seen in run 11 is gone: 10,962 against 10,946
  ascending, 10,298 against 10,808 and 10,731 against 10,207 random (32 and
  64 clients). On three nodes `unit` against `unit-q` runs from 13% above
  (one client, one row) to 9% below (32 clients, 100 rows, ascending), which
  is no more than the same variant moves between runs. (b) No effect that
  can be told from noise: single-row units hold two or three transactions
  and rarely reach the 64 operations at which files are split.
- **Run-to-run spread:** the same variant on the same build differs by 5 to
  10% between runs 10, 11 and 12 (page locks, one node, ascending, 32 × 1:
  8,932, 8,940, 9,414; `unit-q`: 10,353, 9,893, 10,946). Differences smaller
  than that between variants should not be read as real.
- **Caveat:** the second agent's cluster was under load (about 13 cores) at
  12:50 UTC, near the end of the one-node half, and idle at 12:59. The
  three-node random-key phase is slower than in run 10 for every variant
  including `par-early` (23,171 against 28,781 rows/s at 32 × 100), so that
  phase was probably disturbed.

### 13. Test suite with ordered apply and commit units on

- **When:** 2026-11-13, started 15:17 UTC. Not a timed benchmark.
- **Change before:** two changes to commit units, uncommitted. (a) A
  transaction applying its own plan does it under its own Berkeley
  transaction with its own blkseq record, as the pre-unit code did, where
  runs 11 and 12 used a separate unit transaction on the same thread. (b)
  Every unit holds a write lock on one name, and a member that has waited
  `ordered_apply_unit_detect_ms` (100) for its unit waits for that name in
  the lock manager, so the deadlock detector can follow a cycle through the
  member's locks; a member chosen as victim aborts the transactions waiting
  on its locks.
- **Setup:** a native build on xeon (`-DCOMDB2_TEST=1`) in `/tmp/rltest`, a
  private pmux on port 19500, single-node databases. Two passes of the whole
  default suite (789 tests) at once, 20 tests at a time each: a baseline, and
  one with `ordered_apply 1` and `ordered_apply_unit 1` added to every test
  database through `CUSTOMLRLPATH`.
- **Command:** `PMUXPORT=19500 make -kj20` in `tests/`, and the same with
  `CUSTOMLRLPATH=/tmp/rltest/oa.lrl`.
- **Restarted:** the pass with ordered apply on was stopped after eight
  minutes and started again at 15:27 UTC from a second build
  (`/tmp/rltest2`) that adds (c): inserts into tables with unique keys are
  planned (`ordered_apply_unique`, on). Without that, few tables in the suite
  can be planned at all. The index is read for the key when the insert is
  recorded; if the key has appeared by the time the plan is applied, the
  request runs again, and in a unit only that member does. The baseline pass
  was left running from the first build.
- **Both passes stopped early.** The harness gives up once ten tests have
  failed (`MAX_TEST_FAILURES` in `tests/runtestcase`), and many tests fail on
  a single node for reasons of their own. The baseline ran 78 tests (65
  passed) and the ordered-apply pass 40 (26 passed) before skipping the other
  700. In that much, two tests failed only with ordered apply, `scindex` and
  `scindex_logicalsc_generated`, and showed a real bug: an insert made while
  a schema change runs adds its keys at the end of the request from the row
  read back from the data file, and a planned row is not there ("verify key
  constraint: record not found"). Such inserts, and inserts into tables with
  constraint checking on, are no longer planned.
- **Started again** at 16:10 UTC with that fix, as one pass with ordered
  apply on, `MAX_TEST_FAILURES=100000` and `-j40`, from `/tmp/rltest2`. No
  baseline pass: tests that failed were run again on a build of `main`
  (`983c0348d`, in `/tmp/rltest-main`), and the ones that failed only with
  ordered apply looked into.
- **Results:** `suite-run13.txt` lists every test named below.
  - 789 tests: 578 passed and 204 failed; 7 finished without the harness
    printing a result. Of the 199 failures other than `ordered_apply` itself
    (which checks that the feature is off by default), 180 fail on `main` as
    well, mostly for want of a cluster, and 19 pass on `main`.
  - Of those 19, run again on the same build, 4 passed (`sp`,
    `sql_func_mem_leak`, `systbl_proc_leak`, `random_osql_replay`) and 15
    failed again.
- **Bugs found, all fixed in the working tree:**
  1. **Deadlocks the detector did not see** (`insert_lots`, `guid`,
     `reorder_inserts`, their generated variants, `sp` now and then): the
     database stalled until its watchdog aborted it. The lock manager marks a
     transaction family as waiting with one flag on the master locker, set by
     a wait and cleared by the family's next grant. A unit applied a file per
     thread has several threads in one family; a grant to one cleared the
     flag while another waited, and the detector left that wait out of its
     graph. Reproduced on macOS, with the cycle visible in `comdb2_locks`
     and `st_ndeadlocks` at 0. The flag is now a count
     (`berkdb/lock/lock.c`, `lock_deadlock.c`, `dbinc/lock.h`). Plans applied
     a file per thread before units existed had the same exposure.
  2. **The duplicate-key read waiting on its own transaction**
     (`truncoplog`, `sp`): it ran under a lock owner of its own and waited
     for a page the request's parent transaction had written. The read now
     never waits. A locked page means the key is not looked for, and the put
     at apply decides; a request whose key is there at apply runs again
     without a plan, where the put reports the duplicate.
  3. **Inserts during a schema change** (`scindex`,
     `scindex_logicalsc_generated`), described above.
  4. **Row limits counted twice** (`max_wr_rows`): a request that gave up
     its plan kept the row counts of the abandoned attempt.
  5. **The log-bytes limit not applied** (`max_wr_rows`): a plan writes no
     log while the request runs. The limit now counts an estimate of what the
     plan will write.
  6. **`put counter ... increment` applied twice**
     (`manual_partition_lgcy`): a request with a bpfunc gave up its plan
     after the function had run and ran again. A plan is now given up before
     a bpfunc runs.
- **After the fixes**, run again with ordered apply on: `insert_lots`,
  `insert_lots_ssl_generated`, `guid`, `sp`, `reorder_inserts`, `truncoplog`,
  `truncoplog_timebased_generated`, `scindex`,
  `scindex_logicalsc_generated`, `max_wr_rows` and
  `max_wr_rows_partial_index_off_generated` pass. `ordered_apply` passes
  through the harness on Linux with the feature off by default, and the
  `tunables` test differs from its expected output only in the two pmux
  settings of this setup.
- **Still failing with ordered apply on, and why:**
  - `reqlog_logbyte` and `fingerprints`: they check the log bytes and the
    write counts reported for a request. A request whose plan goes into a
    unit writes nothing itself, so it reports none. An accounting gap, not
    fixed.
  - `reorder_deletes_noreorder_generated`,
    `reorder_inserts_noreorder_generated`,
    `nodeadlock_table_noreorder_generated`: they assert a minimum number of
    deadlocks, and there are fewer.
  - `upsert`, `manual_partition_lgcy`: rows come back in a different order
    from a select with no `ORDER BY`. Cause not established; probably which
    data stripe a row lands in.
- **Not covered:** this was single node, so nothing clustered ran; the 180
  tests that fail on `main` here say nothing either way. The whole suite was
  not run again after the fixes, only the tests listed.

### 14. Unique keys

- **When:** 2026-11-13 18:18–18:32 UTC.
- **Change before:** everything under run 13: unique-key inserts planned
  (`ordered_apply_unique`), the detector-visible wait for a unit, a
  transaction's own plan applied under its own transaction, and the six fixes
  from the test suite, among them the lock manager's count of waiting
  lockers. Uncommitted.
- **Setup:** the benchmark table with its index on `a` made unique
  (`UNIQUE=1`, new in `newcluster.sh`); the benchmark never repeats a value
  of `a`. One node, then three; `pg` and `unit`.
- **Command:** `UNIQUE=1 NODES=1 VARIANTS="pg unit"
  ./ordered-apply-variants.sh`, then the same without `NODES=1`.
- **Results:** `stage1-unique-keys.results`.
- **Found:** a unique key changes little. One node, one row, 32 clients:
  10,640 txn/s ascending and 10,844 random, against 7,303 and 9,741 for page
  locks; 100 rows, 32 clients: 39,066 and 33,268 rows/s against 7,541 and
  6,697. Three nodes, one row, 32 clients: 8,334 ascending against 7,027,
  and 7,759 random against 8,714 (9,452 against 8,704 with 64 clients); 100
  rows, 32 clients: 19,254 and 15,861 rows/s against 6,710 and 6,224. No
  request ran without a plan, and there were no deadlocks or unit retries. A
  lone client on one node: 876 txn/s against 993 for page locks.
- **Not comparable closely with run 10:** the three-node 100-row figures are
  lower than run 10's without a unique key (25,648 and 24,228), and this run
  cannot say how much of that is the unique key, the changes since, or the
  5 to 10% between runs. No run of this build without `UNIQUE=1` was made.

### 15. Test suite again, with the fixes

- **When:** 2026-11-13 18:42–21:00 UTC. Not a timed benchmark.
- **Code:** commit `065501ff6`, which has everything from runs 13 and 14.
- **Setup:** as run 13: a native build on xeon in `/tmp/rltest2`, private
  pmux on port 19500, single-node databases, `ordered_apply 1` and
  `ordered_apply_unit 1` added to every test database, `-j40`,
  `MAX_TEST_FAILURES=100000`. One script
  (`/tmp/rltest-keep/run15.sh`) runs the pass, then the failed tests on
  `main` (`983c0348d`), then the ones that pass on `main` once more with
  ordered apply, then `ordered_apply` with nothing added.
- **Results:** `suite-run15.txt`.
  - The pass: 584 passed and 198 failed of 789 (run 13: 578 and 204).
  - The 197 failures other than `ordered_apply` itself, on `main`: 186 fail
    there too and 11 pass.
  - Those 11 again with ordered apply, 20 at a time: 5 pass (`insert_lots`,
    `insert_lots_ssl_generated`, `sc_addfield_logicalsc_generated`,
    `maxtable`, `random_osql_replay`) and 6 fail.
  - `ordered_apply` with nothing added passes.
- **Found:** no new failure caused by ordered apply. The six that fail twice
  with it and pass on `main` are the differences recorded under run 13:
  `reqlog_logbyte` and `fingerprints` (a request in a unit reports no log
  bytes or writes), `reorder_inserts_noreorder_generated` and
  `nodeadlock_table_noreorder_generated` (they assert a minimum number of
  deadlocks), `upsert` and `manual_partition_lgcy` (row order from a select
  with no `ORDER BY`). `reorder_deletes_noreorder_generated`, the third
  deadlock-count test, failed on `main` as well this time. None of the hangs
  of run 13 came back.
- **Failures in the pass that were load:** `insert_lots` and its variant ran
  into their 15-minute limit with 40 tests running (one step took nine
  minutes), `maxtable` into its 40-minute limit, and
  `sc_addfield_logicalsc_generated` failed after 500 seconds. All pass at 20
  in parallel. Whether ordered apply makes them slower under load than
  `main` is not known: `main` was only run at lower load.
- **Watchdog stalls:** four databases logged "stalled, aborting"
  (`electbug2`, `logarchive`, `spemitrace`, `sslpartialwritehang`), each
  while shutting down at the end of its test; the three whose results were
  found passed. Not compared with `main`, which did not run those tests.
- **Not covered:** single node only.

### 16. Test suite with updates and deletes planned

- **When:** started 2026-11-13 22:38 UTC. Not a timed benchmark.
- **Change before:** updates and deletes in plan mode
  (`ordered_apply_update`, on by default). A plan now holds deletes and
  updates of rows, blobs and keys as well as adds. A row is read by genid
  with the plan laid over the data file, by a read that keeps no lock; the
  data file judges the plan at apply, by genid and updateid, and a plan
  that fails there sends its request back to run under page locks. A
  request that fails while it holds a page lock taken around the plan runs
  again without one. Inserts made with constraint checking on are planned
  too, since the row they read back at the end of the request is now found
  in the plan. Tables with a foreign key in either direction, or an index
  on a blob, are still not planned for updates and deletes. Uncommitted,
  on top of `67a7f016e`.
- **Setup:** as run 15: native build on xeon in `/tmp/rltest2`, private pmux
  on port 19500, single-node databases, `ordered_apply 1` and
  `ordered_apply_unit 1` added to every test database, `-j40`. Script:
  `suite-with-ordered-apply.sh run16`, output in `/tmp/rltest-keep/run16`.
- **Results:** none kept as a file: the run was stopped by hand at 23:53 UTC,
  after the first pass, because the code had changed under it. First pass:
  573 passed and 207 failed of 789. Of the failures, 14 had not failed in
  the pass of run 15.
- **Found:** five things, all fixed in the tree before run 18; each test
  named was run again on the fixed build (`/tmp/rltest3`) and passed unless
  said otherwise.
  - **A commit lock left behind**, from before this session. A request whose
    plan could not be applied went to the backout path still holding the
    block processor's commit lock, shared, and never gave it up. The next
    SELECTV or serializable request, which takes that lock exclusively,
    waited for ever, and its database stalled until the watchdog aborted it.
    Not seen before because those requests used to give up their plan at
    their first update, before the lock. `selectv_rcode` and its four
    variants.
  - **SELECTV rows not held.** A SELECTV row is checked by reading it, and
    the page lock that read takes holds the row until commit. A plan's read
    keeps no lock. Such a request now gives its plan up when it meets the
    record.
  - **A wait on the commit lock that the detector cannot see.** The lock is
    a pthread rwlock. A request with a plan takes its page locks after it,
    through its unit, so it could wait for a page held by a request that was
    waiting for the lock. With ordered apply on, a request now takes a lock
    of the same name and mode in the lock manager first. `selectv_rcode` in
    its serializable phase, `deadlock_load` (which failed with error 402
    after ten minutes).
  - **A duplicate-key error that page locks would not give.**
    `parenttran_fail` and `ptranfail_socksql` race to move a unique key on
    by one. A loser read the row, the winner committed, and the loser's
    check of the new key then found the winner's: "duplicate key", where
    under page locks the loser finds the row changed and the client library
    retries. A request with a plan that has read rows and then fails with
    anything but a verify error now runs again without a plan.
  - **A deadlock between a reader and two threads of one unit.**
    `recovdlock_emptyblob` hung with a unit applied a file per thread: the
    thread for a blob file was done and held a blob page; the thread for the
    data file waited for a page a reader held; the reader waited for the blob
    page. The detector did not break it; why not was not found. Plans that
    update or delete are now applied on one thread, which takes pages in the
    order a page-lock transaction does. Plans of adds alone are still
    applied a file per thread and may have the same exposure with readers.
  - **Still failing with ordered apply, passing without:**
    `selectv_rcode_disable_svonly_nop_generated`, alone or with others: in
    its snapshot phase one transaction reaches 50,001 statements
    ("transaction too big"). `commit_lsn_map_delete_generated`, three times
    of three: `comm` reports its input unsorted when comparing the
    transactions in the commit map after a log truncation; the lists
    looked right when printed. Both passed in run 15. Causes not found.
  - **Expected:** `reorder_updates_noreorder_generated` and
    `index_deadlock_loop_print` want deadlocks that planned updates no
    longer have. `ordered_apply` itself fails with the settings forced on.
  - **Load:** `selectv_deadlock` ("too many deadlocks") passed when run with
    fewer tests beside it.
  - **Elsewhere:** on the Mac, once, a database running without ordered
    apply aborted with "thd_req mismatch thd id 0x0" in `db/handle_buf.c`
    under 36 clients. That check reads the thread id that `pthread_create`
    stores for the new thread, from the new thread. Not looked into.

### 17. Updates and deletes

- **When:** started 2026-11-13 23:54 UTC (image build), three nodes.
- **Change before:** everything under run 16, with its fixes. Uncommitted.
- **Setup:** the benchmark table with 1,000,000 rows loaded with `a` =
  1..1,000,000 (`ordered-apply-updates.sh`, new). New workloads in
  `bench_cluster.py`: `upd` (a column with no index), `updk` (column `b`,
  indexed), `updhot` (every client picks from the same 200 rows), `del`.
  A transaction of 100 rows is 100 statements between `begin` and `commit`.
  `pg` and `unit`.
- **Command:** `./ordered-apply-updates.sh`.
- **Results:** none kept. The run finished (23:56–00:03 UTC) with the three
  nodes identical and verify clean, but its comparison is not valid: the
  delete workload of the `pg` pass removed the rows with `a` up to 256,000,
  among them the 200 that `updhot` uses, so in the `unit` pass `updhot`
  updated nothing and a quarter of the other updates found no row.
- **Found:** a mistake in the benchmark, fixed before run 18: updates now
  pick from the upper half of the keys, deletes take theirs from 1,001 to
  the middle, and `updhot` keeps 1 to 200.

### 18. Updates and deletes, with the key ranges apart

- **When:** 2026-11-14 00:03–00:12 UTC, three nodes.
- **Change before:** the benchmark only, as under run 17. Code as run 17.
- **Setup and command:** as run 17. The last cell, 100-row deletes with
  plans, ran out of delete keys and was run by hand afterwards with
  `KEYS=1100000`, which only lifts that limit.
- **Results:** `stage1-updates.results`.

  | 32 clients unless said        | Page locks    | Unit   |
  |-------------------------------|--------------:|-------:|
  | `upd`, 1 row, 1 client        | 474 txn/s     |    455 |
  | `upd`, 1 row                  | 10,468 txn/s  |  9,139 |
  | `upd`, 1 row, 64 clients      | 10,782 txn/s  | 11,061 |
  | `updk`, 1 row                 | 7,665 txn/s   |  7,512 |
  | `updhot`, 1 row               | 3,252 txn/s   |  6,323 |
  | `del`, 1 row                  | 6,792 txn/s   |  7,404 |
  | `upd`, 100 rows               | 6,191 rows/s  |  9,123 |
  | `updk`, 100 rows              | 3,811 rows/s  |  6,651 |
  | `del`, 100 rows               | 13,307 rows/s | 10,522 |

- **Found:**
  - Single-row updates of rows nobody else wants are level with page locks:
    13% behind with 32 clients on a column with no index, 2% behind on an
    indexed one, 3% ahead with 64 clients. The master uses 6.2 to 7.7 cores
    where page locks use 8.1 to 9.0. No request fell back; 8 plans of
    256,000 were stale.
  - Updates of the same 200 rows are 1.9 times as fast. Of 32,000 requests,
    1,111 plans were stale at apply and 199 gave up on a locked read; each
    ran again under page locks. Page locks had 53,779 lock waits there.
  - 100-row updates gain 47% and 75%, with a third fewer deadlocks, but
    most of the possible gain is lost: 711 and 607 of 1,920 transactions
    gave up their plan because a row they read stayed behind a page lock,
    and 324 and 437 units were aborted as deadlock victims and applied
    again. A unit holds its page locks to its commit, and a 100-row plan
    reads 100 rows while units are being applied.
  - 100-row deletes lose 21%: 497 of 1,920 gave up on a locked read. Page
    locks do well here because the keys are ascending and each client's
    rows are apart.
  - After the run the three nodes held identical tables and verify
    succeeded.

### 19. Test suite on the fixed tree, 20 tests at a time

- **When:** started 2026-11-14 00:12 UTC. Not a timed benchmark.
- **Change before:** the five fixes under run 16. Uncommitted.
- **Setup:** as run 16, but `-j20`: many tests depend on timing.
  `suite-with-ordered-apply.sh run19 20`, output in
  `/tmp/rltest-keep/run19`.
- **When ended:** 02:15 UTC, stopped by hand: the pass on `main` was down
  to seven long tests that need a cluster.
- **Results:** `suite-run19.txt`.
  - The pass: 571 passed and 212 failed of 789 (run 15: 584 and 198).
  - The 211 failures other than `ordered_apply` itself, on `main`: 22 pass
    there; of the rest 182 fail and seven were still running.
  - Those 22 again with ordered apply, eight at a time, on the second build
    of the same tree (`/tmp/rltest3`): 16 pass and 6 fail.
- **Found:** no failure caused by ordered apply beyond the differences
  already known. The six that fail twice with it and pass on `main`:
  `fingerprints` (a request in a unit reports no writes),
  `manual_partition_lgcy` (row order from a select with no `ORDER BY`),
  and four that want deadlocks which plans do not have
  (`reorder_inserts_noreorder_generated`,
  `nodeadlock_table_noreorder_generated`, and now
  `reorder_updates_noreorder_generated` and `index_deadlock_loop_print`).
  `upsert`, on that list in run 15, passed this time.
- **Failures in the pass that were not ordered apply:**
  - `deadlock_load` and `selectv_deadlock`: the harness found the
    `.failexit` files of run 16 in the test directory, which was not
    emptied between the runs. Both passed again in a clean directory.
  - `alias`, `consistent_hash_bench`, `diskspace_nollmeta` and its variant,
    `early_verify`, `manual_partition`, `pmux` and its two variants,
    `sc_rebuilds` and its two variants, `sp_twofiles_generated`: passed the
    second time. During the pass I was running other tests and the
    benchmark's control runs on the same machine and through the same pmux.
  - `reads_follows_writes_sc`: "failed physical verify" of a data file of a
    dropped table. Not a damaged file: the test creates and drops thousands
    of tables, and the database deleted the file while the harness was going
    through the directory. It fails the same way on this build without
    ordered apply when run alone.
- **The two tests left unexplained in run 16 passed here**
  (`selectv_rcode_disable_svonly_nop_generated` and
  `commit_lsn_map_delete_generated`), in the pass, once each. They failed
  three and four times of as many on the same code before, so they are not
  settled.
- **Watchdog stalls:** the same four databases as run 15, at shutdown
  (`electbug2`, `logarchive`, `spemitrace`, `sslpartialwritehang`).
- **Not covered:** single node only. This run's build predates commit
  `466c093bc`, after which `ordered_apply` is refused unless
  `disable_tagged_api_writes` is on; the settings file for the suite
  (`oa.lrl`) will need that line too.

## Elsewhere

- **Adaptive mode on the existing rowlocks code**, by a second agent, on
  branch `rowlocks-adaptive` (commits `cab218e61`, `5603cfb4b`, `ea2bf95a6`,
  from `e27a9c411`). Report and raw output:
  `docs/adr/0003-bench/results/rowlocks-adaptive.md` and
  `rowlocks-adaptive.results` on that branch.
  - **When:** 2026-11-13, about 12:28–13:40 UTC, three nodes, two runs per
    cell. It overlapped runs 11 and 12 here: the other cluster was above one
    core right after 29 of its 64 runs.
  - **Change:** tunable `rowlocks_adaptive`. A request that is alone in the
    block processor when it starts keeps one physical transaction across its
    operations (the rowlocks code's `micro_commit` cleared) but still takes
    row locks and writes logical records. A plain page-lock transaction
    cannot share a rowlocks database with logical ones.
  - **Found:** one client, 100 rows: 5,870 rows/s ascending against 3,360
    for rowlocks and 8,560 for page locks, so about 70% of page locks. With
    32 clients it equals rowlocks. The existing `rowlocks_micro_commit off`
    applied to every request matches it for one client and is 18–40% faster
    with 32.

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

### 20. The two unexplained tests, five times each way

- **When:** started 2026-11-14 13:01 UTC. Not a timed benchmark.
- **Change before:** none to the code. The build is the committed tree
  (`80d2ea75d`), the first to include `466c093bc`; `oa.lrl` now starts with
  `disable_tagged_api_writes 1`.
- **Setup:** xeon, one node, the tree shipped with `git archive` over
  `/tmp/rltest2/src` and `/tmp/rltest3/src` and rebuilt, old test
  directories removed. First the `ordered_apply` test with nothing added.
  Then `repeat-tests.sh run20 5 selectv_rcode_disable_svonly_nop_generated
  commit_lsn_map_delete_generated`: each test alone, five times with
  ordered apply and five times without, the two sides running side by side.
  Output in `/tmp/rltest-keep/run20`.
- **Then:** the same two tests five times on the build of `main`
  (`/tmp/rltest-main`, `983c0348d`), 13:30–13:53 UTC, since both sides
  failed.
- **Results:** `run20-two-tests.txt`. The `ordered_apply` test passed
  (`oa-own.out`). Failures of five:

  |                                              | With | Without | `main` |
  |----------------------------------------------|-----:|--------:|-------:|
  | `selectv_rcode_disable_svonly_nop_generated` |    4 |       4 |      5 |
  | `commit_lsn_map_delete_generated`            |    3 |       3 |      3 |

- **Found:** neither failure is ordered apply's. Both come from the test
  machine and fail as often on `main`.
  - `commit_lsn_map_delete_generated`: every failure is `comm: file 2 is
    not in sorted order` at line 356 of the test. xeon runs Ubuntu 25.10,
    whose `comm` is uutils coreutils 0.2.2, and that `comm` reports sorted
    input as unsorted when it comes through a pipe and is longer than 8,192
    bytes: `comm -12 <(seq 1101 2739 | sort) <(echo 5)` fails, with 2738 it
    does not, and from files it never does. The test passes when the list
    of transactions to be deleted is shorter than that. `gnucomm` is
    installed and does not have the fault.
  - `selectv_rcode_disable_svonly_nop_generated`: 12 of the 13 failures are
    "transaction too big" at commit in the snapshot phase; one, without
    ordered apply, ran into the seven-minute limit. Why `main` produces a
    50,001-statement transaction there on this machine was not looked into.
  - Run 16 had them as failing with ordered apply and passing without, from
    three and four runs with and fewer without. That was chance.

### 21. The cycle the detector missed, reproduced

- **When:** started 2026-11-14 13:42 UTC. Not a timed benchmark.
- **Change before:** tunable `ordered_apply_parallel_updates` (off by
  default): on, a plan with updates or deletes is applied a file per thread
  again, as it was when `recovdlock_emptyblob` hung in run 16. Uncommitted.
- **Setup:** xeon, one node, `/tmp/rltest3` rebuilt with the change.
  `recovdlock_emptyblob` alone with `oa.lrl` plus
  `ordered_apply_parallel_updates 1` (`/tmp/rltest-keep/run21/oa-par.lrl`),
  while `watch.sh` there saves `bdb lockinfo lockers` and the deadlock
  counters every 10 s. It overlaps the end of run 20's pass on `main`.
- **Results:** `run21-lock-table.txt`, `run21-recovdlock.txt`. Output in
  `/tmp/rltest-keep/run21`.
  - Reproduced at once: the counters stood still from 30 s in (339
    deadlocks, 157 unit retries, one unit applied) and the test ran into
    its five-minute limit.
  - With a detector run added after a grant (below), and nothing else:
    stalled again, twice of two.
  - With the detector waking all of a victim's family, and nothing else:
    stalled three times of four.
  - With both: passed five times of five.
  - By how plans that update are applied, final code, last counters of each
    120 s run. Every mode commits 73 to 106 transactions: the test's
    clients are slowed by a debug sleep.

    |                                 | Runs | Deadlocks     | Unit retries | Units   |
    |---------------------------------|-----:|--------------:|-------------:|--------:|
    | Page locks                      |    6 | 2,373–2,524   |              |         |
    | Unit, one thread                |    2 | 3,015, 3,088  | 953, 973     | 81, 75  |
    | A file per thread, limit 1      |    3 | 4,325–4,825   | 998–1,352    | 73–99   |
    | A file per thread, limit 2      |    2 | 4,618, 4,780  | 1,036, 1,124 | 93, 92  |
    | A file per thread, limit 4      |    2 | 4,796, 5,449  | 1,320, 1,452 | 88, 95  |
    | A file per thread, no fall-back |    1 | 22,311        | 8,679        | 42      |

- **Found:**
  - **The detector woke one thread of its victim.** In both stalls the unit
    had one thread waiting and another with no locks left, its wait
    aborted. A unit cannot abort until all its threads are back, so the
    cycle stood, and the detector runs only when a wait begins. One new
    reader made to wait freed the stalled database: the detector found the
    cycle at once (`run21-lock-table.txt`). Fixed: `__dd_abort_family()`
    aborts every waiting lock of the victim's family.
  - **A grant can close a cycle.** With that fix alone a stall was caught
    with a unit and a page-lock transaction each waiting for a page the
    other held and nothing aborted. The detector's graph has no edge from a
    waiter to a waiter ahead of it; the edge appears when that one is
    promoted, and no wait begins then. That the order of events was this is
    read from the lock table, not traced. Fixed: a lock granted after a
    wait runs the detector if another locker of its family is waiting,
    which cannot be so for a family with one thread.
  - **Then it deadlocks, a lot.** Several threads take data, blob and index
    pages in no order, and a transaction that only reads is never the
    victim. With no fall-back a unit is aborted about 200 times for each
    time it is applied. `ordered_apply_parallel_retries` (1): after that
    many deadlocks a file per thread, the unit is applied on one thread.
    1 and 2 measure the same, 4 a little worse.
  - Units on one thread are themselves aborted about twelve times for each
    time they are applied in this test, and have a fifth more deadlocks
    than page locks.
  - The first five runs of the last table were meant to have ordered apply
    on; the script looked for `oa.lrl` in the wrong directory. They are the
    page-lock rows.

### 22. One large transaction against ten small ones on its rows

- **When:** image build started 2026-11-14 14:17 UTC, three nodes.
- **Change before:** the two detector fixes of run 21; a unit that has been
  a deadlock victim `ordered_apply_parallel_retries` times (new, default 1)
  while applied a file per thread is applied on one thread when it runs
  again; counter `ordered_apply_unit_serial`. Uncommitted.
- **Setup:** 10,000 rows. One client updates all of them in one statement,
  which is one transaction (column `b`, indexed), again and again; ten
  clients each update one row of that set again and again, a statement a
  transaction. 60 s for each of `pg`, `unit` (updates on one thread), and
  `unit-p1`, `unit-p2`, `unit-p4` (a file per thread, falling back to one
  thread after 1, 2 or 4 deadlocks). New: `big-small.py`,
  `ordered-apply-bigsmall.sh`. It overlapped the test runs of run 21 on the
  same machine.
- **Command:** `./ordered-apply-bigsmall.sh`, 14:25–14:31 UTC. Two earlier
  attempts gave nothing: the first wrote the clients' output to a directory
  that is read-only in the container; the second had the large transaction
  as 10,000 statements between `begin` and `commit`, which take longer than
  60 s to send (about 6 ms each).
- **Results:** `stage1-big-small.results`.

  |           | Large, committed in 60 s | Small, txn/s | Slowest small client |
  |-----------|-------------------------:|-------------:|---------------------:|
  | `pg`      |                        0 |        3,001 |               17,642 |
  | `unit`    |                        0 |        3,733 |               20,894 |
  | `unit-p1` |                        0 |        3,799 |               21,506 |
  | `unit-p2` |                        0 |        3,626 |               20,750 |
  | `unit-p4` |                        0 |        3,872 |               22,070 |

- **Found:**
  - The large transaction never commits, with ordered apply or without. It
    takes 0.83 s alone. Under page locks, with ordered apply off, one small
    client (493 txn/s) is enough to keep it from committing for 40 s. The
    statement is run on a replicant, a row it read has changed by the time
    the master checks it, and it is run again: `verify_replays` on the
    replicants rose by 225 each in 80 s. `osql_verify_retry_max` is 499, so
    a client would wait several minutes and then get an error. This is how
    the system behaves today and ordered apply does not change it.
  - The small clients all make progress, 21 to 29% faster with units than
    under page locks. Of their 220,000 plans about 2,200 gave up on a
    locked read and ran again under page locks. No failures.
  - Nothing here was applied a file per thread (the large transaction never
    got past the master's check of the rows it read, and a single-row update is below
    `ordered_apply_parallel_min_ops`), so the three `unit-p` columns say
    nothing about `ordered_apply_parallel_retries`.
  - The three nodes held identical tables and verify succeeded.

### 23. Updates and deletes again, with the detector fixes

- **When:** started 2026-11-14 15:04 UTC, three nodes.
- **Change before:** as run 22. Uncommitted.
- **Setup:** as run 18, with a third pass: `unit-p`, units with plans that
  update or delete applied a file per thread
  (`ordered_apply_parallel_updates 1`, `ordered_apply_parallel_retries` at
  its default of 1). Nothing else running on the machine.
- **Command:** `KEYS=2000000 VARIANTS="pg unit unit-p"
  ./ordered-apply-updates.sh`. Two million rows where run 18 had one, so
  that three passes of deletes have keys enough; the numbers are to be
  compared within this run.
- **Then:** a second pass with the variants in the opposite order
  (`unit-p unit pg`), 15:19–15:32 UTC.
- **Results:** `stage1-updates-run23.results`. Rows a second, the two
  passes and their mean; 32 clients unless said.

  |                        | Page locks             | Unit, one thread       | Unit, a file per thread |
  |------------------------|-----------------------:|-----------------------:|------------------------:|
  | `upd`, 1 row, 1 client | 468, 464: 466          | 454, 437: 446          | 497, 484: 490           |
  | `upd`, 1 row           | 8,800, 9,160: 8,980    | 7,939, 8,042: 7,990    | 8,063, 7,995: 8,029     |
  | `updk`, 1 row          | 7,553, 7,646: 7,600    | 7,103, 7,061: 7,082    | 6,664, 6,793: 6,728     |
  | `upd`, 1 row, 64       | 11,339, 11,022: 11,180 | 10,618, 10,928: 10,773 | 11,431, 11,516: 11,474  |
  | `updhot`, 1 row        | 3,378, 3,406: 3,392    | 5,465, 6,000: 5,732    | 5,506, 6,013: 5,760     |
  | `del`, 1 row           | 6,329, 6,324: 6,326    | 7,129, 6,587: 6,858    | 6,678, 6,499: 6,588     |
  | `upd`, 100 rows        | 8,880, 8,654: 8,767    | 10,068, 9,808: 9,938   | 11,204, 10,844: 11,024  |
  | `updk`, 100 rows       | 4,383, 4,657: 4,520    | 7,510, 7,235: 7,372    | 6,635, 6,854: 6,744     |
  | `del`, 100 rows        | 10,134, 10,225: 10,180 | 9,020, 8,429: 8,724    | 10,256, 9,790: 10,023   |

- **Found:**
  - The two passes agree to within 4% in most cells and 10% in the worst
    (`updhot` and single-row deletes with units).
  - Units on one thread against page locks, as run 18 had it: 11%, 7% and
    4% behind on single-row updates of rows nobody else wants, 8% ahead on
    single-row deletes, 69% ahead on the 200 hot rows, 13% and 63% ahead on
    100-row updates, 14% behind on 100-row deletes. The detector changes
    cost this mode nothing that shows: for a unit on one thread neither
    new path has anything to do.
  - A file per thread against one thread: 11% more on 100-row updates of
    the column with no index and 15% more on 100-row deletes, which brings
    those deletes level with page locks; 9% less on 100-row updates of the
    indexed column, where it has as many deadlocks as page locks (1,434 and
    1,522 against 852 and 919 on one thread, 1,437 and 1,262 under page
    locks) and 484 and 580 units were aborted and applied again against 337
    and 364. Single-row cells are within 7% either way, few of their units
    being large enough to split.
  - So a file per thread is worth having where the files a transaction
    writes are not ones another transaction reaches in a different order,
    and costs where they are. It stays off by default.
  - After each pass node1 and node3 held identical tables and verify
    succeeded. node2 did not answer: it was 11 log files behind the master
    after the first pass and matched the others two and a half minutes
    later, when it had caught up. It ran most of the SQL (16 to 25 cores).

### 24. Test suite with the detector changes

- **When:** started 2026-11-14 15:51 UTC. Not a timed benchmark.
- **Change before:** as run 23: the two detector fixes, the fall-back to
  one thread, the two tunables. Uncommitted.
- **Setup:** as run 19: `suite-with-ordered-apply.sh run24 20` on xeon, one
  node, `oa.lrl` with `disable_tagged_api_writes 1`, test directories of
  earlier runs removed first. Output in `/tmp/rltest-keep/run24`.
  `ordered_apply_parallel_updates` is off, as it is by default, so what
  this run tries is the detector under every other transaction and under
  plans of adds, which are applied a file per thread.
- **Results:** none. Stopped by hand at 16:01 UTC, 42 tests into the first
  pass (9 failed, all tests that need a cluster or fail on any build
  here), and its test databases stopped. The suite is put off until the
  implementation is done; until then the tests of run 25 are what is run.

### 25. The tests written for this work, first run

- **When:** 2026-11-14 16:10–16:25 UTC. One node.
- **Change before:** none to the database (the tree of run 24, which is
  commit `6327ced0f`). New: `tests/oa_counters.test`, `oa_bank`,
  `oa_unique`, `oa_deadlock`, `oa_queue`, `oa_throughput`,
  `tests/tools/oa_common.sh` and `adr-tests.sh`, which runs them. Each test
  runs its workload under page locks (`pg`), with commit units (`unit`) and
  with units applied a file per thread (`unitpar`), and checks an
  invariant; see "Tests for iterating" in the ADR.
- **Setup:** xeon, the build in `/tmp/rltest3`, our pmux on 19500, nothing
  else running. `adr-tests.sh /tmp/rltest3/src`: the five checking tests
  five at a time, then `oa_throughput` alone.
- **Results:** `adr-tests-run25.txt`. All six pass in all three modes. The
  five checking tests take 93 s together and `oa_throughput` 319 s.
  `oa_throughput`, 16 clients, transactions a second:

  | Cell     | Page locks | Unit           | Unit, a file per thread |
  |----------|-----------:|---------------:|------------------------:|
  | `ins1`   |      9,732 | 11,158 (+15%)  | 11,220 (+15%)           |
  | `ins100` |         97 | 431 (+344%)    | 431 (+344%)             |
  | `upd1`   |      9,456 | 9,768 (+3%)    | 9,512 (+1%)             |
  | `updk1`  |      9,040 | 9,281 (+3%)    | 9,259 (+2%)             |
  | `updhot` |      1,735 | 2,889 (+67%)   | 2,955 (+70%)            |
  | `upd20`  |        418 | 543 (+30%)     | 507 (+21%)              |
  | `del1`   |      8,811 | 7,859 (−11%)   | 7,882 (−11%)            |

- **Found:**
  - Nothing wrong with the database. The first runs failed on the tests
    themselves: errors and output of `cdb2sql` are not written in step, so
    they are now kept apart; a count carried over from one client's file to
    the next; and `oa_counters` updated a row twice in one transaction,
    which cannot commit ("uncommittable txn").
  - A transaction of several statements that finds a row it read changed
    fails at commit and is not run again by `cdb2sql`. The tests count it
    as failed and check their invariants on what committed.
  - Single-row deletes with units are 11% behind page locks on one node,
    with 1,363 and 1,789 units aborted on a deadlock and applied again in
    80,000 deletes, where page locks had 776 deadlocks. Each client deletes
    rows of its own, sixteen keys apart, so they meet on pages. Not looked
    into.
  - One node, one run of each cell, 4 to 46 s a cell: good for seeing a
    change of a fifth, not of a twentieth. The three-node numbers are run
    23's.

### 26. The tests of run 25, with planning reads that wait for the unit

- **When:** started 2026-11-14 18:22 UTC. One node.
- **Change before:** a read made while planning (a row by genid, or a look
  for a unique key) that finds its page locked no longer gives the plan up
  after six tries over about 3 ms. If the request holds no page lock it
  waits for the commit unit being applied to end (a condition variable
  broadcast whenever a unit ends) and tries again, for up to
  `ordered_apply_read_wait_ms` (new tunable, 100) in all; with no unit being
  applied it sleeps a millisecond between tries. With a page lock held, or
  past the limit, the old six tries. New counters
  `ordered_apply_read_waits`, `ordered_apply_read_wait_us`. Uncommitted.
- **Setup:** as run 25: xeon, `/tmp/rltest3` with the change, our pmux on
  19500. `adr-tests.sh /tmp/rltest3/src`, then the `tunables` test.
- **Results:** `adr-tests-run26.txt`. All six pass in all three modes, in
  416 s. `tunables` differs from its expected output only in
  `portmux_port` and `portmux_bind_path`, which are our pmux's; the new
  tunable's line matches. `oa_throughput`, transactions a second (run 25 in
  brackets):

  | Cell     | Page locks    | Unit          | Unit, a file per thread |
  |----------|--------------:|--------------:|------------------------:|
  | `ins1`   | 8,772 (9,732) | 11,127 (11,158) | 10,870 (11,220)       |
  | `ins100` | 93 (97)       | 457 (431)     | 460 (431)               |
  | `upd1`   | 9,390 (9,456) | 9,206 (9,768) | 9,501 (9,512)           |
  | `updk1`  | 8,949 (9,040) | 9,238 (9,281) | 9,185 (9,259)           |
  | `updhot` | 1,647 (1,735) | 3,245 (2,889) | 3,313 (2,955)           |
  | `upd20`  | 397 (418)     | 539 (543)     | 536 (507)               |
  | `del1`   | 8,529 (8,811) | 7,812 (7,859) | 8,016 (7,882)           |

- **Found:**
  - Nothing broken: every invariant holds, and no database stopped
    committing.
  - These one-node cells give few plans up for a locked read in any case
    (`upd20` had 9 fallbacks in 3,991, the same 9 transactions that fail
    under page locks), so they say little about the change. `updhot` with
    units is 12% higher than in run 25, about what one run of a cell
    varies by here. The three-node 100-row cells of run 23, where a third
    of plans were given up, are what it is for: run 27.

### 27. Updates and deletes on three nodes, with planning reads that wait

- **When:** started 2026-11-14 18:31 UTC, three nodes.
- **Change before:** as run 26 (planning reads wait for the unit,
  `ordered_apply_read_wait_ms` 100). `ordered-apply-updates.sh` now also
  prints `read-waits` and the mean wait. Uncommitted.
- **Setup:** as run 23: image rebuilt from the tree (`setup-host.sh` in
  `/tmp/rlsrc`), `KEYS=2000000 VARIANTS="pg unit unit-p"
  ./ordered-apply-updates.sh`, then a second pass in the opposite order.
  Nothing else running on the machine but the idle leftover of run 24.
- **Then:** the second pass, 18:48–19:00 UTC.
- **Results:** `stage1-updates-run27.results`. Rows a second, the two
  passes and their mean, with run 23's mean in brackets; 32 clients unless
  said.

  |                        | Page locks                       | Unit, one thread                 | Unit, a file per thread          |
  |------------------------|---------------------------------:|---------------------------------:|---------------------------------:|
  | `upd`, 1 row, 1 client | 461, 464: 463 (466)              | 462, 480: 471 (446)              | 475, 420: 448 (490)              |
  | `upd`, 1 row           | 8,776, 10,437: 9,607 (8,980)     | 8,269, 9,295: 8,782 (7,990)      | 8,081, 7,960: 8,021 (8,029)      |
  | `updk`, 1 row          | 7,182, 8,367: 7,775 (7,600)      | 7,089, 7,665: 7,377 (7,082)      | 6,475, 7,227: 6,851 (6,728)      |
  | `upd`, 1 row, 64       | 11,180, 11,056: 11,118 (11,180)  | 10,688, 11,328: 11,008 (10,773)  | 11,429, 12,390: 11,910 (11,474)  |
  | `updhot`, 1 row        | 3,320, 3,188: 3,254 (3,392)      | 5,862, 6,462: 6,162 (5,732)      | 5,937, 6,619: 6,278 (5,760)      |
  | `del`, 1 row           | 6,291, 6,792: 6,542 (6,326)      | 6,642, 7,014: 6,828 (6,858)      | 6,447, 7,427: 6,937 (6,588)      |
  | `upd`, 100 rows        | 8,637, 7,649: 8,143 (8,767)      | 10,296, 11,606: 10,951 (9,938)   | 11,172, 16,282: 13,727 (11,024)  |
  | `updk`, 100 rows       | 4,189, 4,347: 4,268 (4,520)      | 8,121, 9,027: 8,574 (7,372)      | 9,385, 7,953: 8,669 (6,744)      |
  | `del`, 100 rows        | 10,319, 14,166: 12,243 (10,180)  | 7,450, 9,373: 8,412 (8,724)      | 9,748, 12,337: 11,043 (10,023)   |

  Plans given up for a locked read, of 1,920, in the 100-row cells, by
  pass (run 23's two passes in brackets):

  |              | Unit, one thread    | Unit, a file per thread |
  |--------------|--------------------:|------------------------:|
  | `upd`        | 0, 12 (319, 281)    | 0, 1 (56, 43)           |
  | `updk`       | 8, 18 (438, 452)    | 6, 79 (623, 565)        |
  | `del`        | 0, 0 (496, 518)     | 0, 0 (373, 302)         |

- **Found:**
  - The change does what it is for. Plans given up for a locked read in
    the 100-row cells fell from 43 to 623 a cell to 0 to 79. The reads
    that wait in those cells wait 6 to 40 ms on average (`read-wait-us`),
    under the 100 ms limit nearly always. In single-row cells a few
    thousand reads wait, 0.3 to 0.5 ms each, and none gives up.
  - The passes disagree more than in run 23: up to 46% (`upd` 100 rows a
    file per thread, 11,172 and 16,282) and 37% (`del` 100 rows under page
    locks). The second pass ran on a cluster freshly made, with the order
    reversed; I did not find which of the two moves it. Differences of a
    fifth between the means are not to be read as the change's.
  - With that said: 100-row updates with units are 10% (one thread) and
    25% (a file per thread) above run 23, and of the indexed column 16% and
    29% above; page locks moved −7% and −6% on the same cells. Single-row
    cells are where they were.
  - 100-row deletes on one thread are no better (8,412 against 8,724), and
    stay behind page locks. They used to give a quarter of their plans up
    and run those under page locks, beside the unit; now the unit thread
    applies all of them, at 5 to 7 ms a plan, one unit at a time. The
    unit thread is the limit there, not the reads: a file per thread
    (11,043) is what helps.
  - Tables identical on the nodes that answered, verify clean. After the
    first pass node2 did not answer within the check's retries, as in run
    23 (it was behind); after the second all three matched.


### 28. The ADR's tests with several unit threads

- **When:** started 2026-11-14 19:07 UTC. One node.
- **Change before:** commit `80b59d3e0` (run 27's code), then:
  - New tunable `ordered_apply_unit_threads` (1, at most 16): that many
    threads apply units, each one at a time as an ordinary transaction
    under page locks. Units that meet on a page wait for each other in the
    lock manager. Each thread's unit holds a lock name of its own
    (`ordered unit<n>`); a member that waits long asks for its unit's
    name, or, before a thread has taken it, for each thread's in turn.
  - The helper pool for a file per thread counted the files outstanding in
    one global. Two units applied a file per thread at once would each
    have waited for the other's files too, on a condition variable, and a
    file of one can wait for a page the other holds: a cycle the detector
    cannot see. The count is now per plan, and a thread draining the queue
    takes only its own plan's files.
  - `tests/tools/oa_common.sh`: two more modes, `units` and `unitspar`,
    which are `unit` and `unitpar` with four unit threads.
  Uncommitted.
- **Setup:** as run 26, `adr-tests.sh /tmp/rltest3/src` with the five
  modes, then `tunables`.
- **Results:** `adr-tests-run28.txt`. All six pass in all five modes, in
  645 s: every invariant holds with four unit threads, and nothing stalled.
  `oa_throughput`, transactions a second:

  | Cell     | Page locks | Unit           | Unit, file per thread | 4 unit threads | 4 threads, file per thread |
  |----------|-----------:|---------------:|----------------------:|---------------:|---------------------------:|
  | `ins1`   | 8,830      | 11,527 (+31%)  | 11,173 (+27%)         | 8,556 (−3%)    | 8,979 (+2%)                |
  | `ins100` | 95         | 429 (+352%)    | 460 (+384%)           | 96 (+1%)       | 103 (+8%)                  |
  | `upd1`   | 9,412      | 9,259 (−2%)    | 9,456 (0%)            | 8,949 (−5%)    | 8,919 (−5%)                |
  | `updk1`  | 9,238      | 9,479 (+3%)    | 9,040 (−2%)           | 8,511 (−8%)    | 8,602 (−7%)                |
  | `updhot` | 1,688      | 3,350 (+98%)   | 3,407 (+102%)         | 2,767 (+64%)   | 2,907 (+72%)               |
  | `upd20`  | 406        | 554 (+36%)     | 538 (+33%)            | 449 (+11%)     | 522 (+29%)                 |
  | `del1`   | 8,475      | 7,685 (−9%)    | 7,976 (−6%)           | 7,692 (−9%)    | 7,663 (−10%)               |

- **Found:**
  - Correct, and slow. Units side by side with no rule are page-lock
    transactions again: in `ins100` 1,058 units were aborted and applied
    again 5,958 times, and the cell fell to the rate of page locks. `ins1`
    had 1,407 unit retries where one thread had none. Every cell here
    writes one table, so units meet on its pages, in no order.
  - So units are not to be applied side by side on the same files. Next
    (run 29): a unit thread takes only members whose files no unit being
    applied writes.


### 29. Units side by side only on files apart

- **When:** started 2026-11-14 19:19 UTC. One node.
- **Change before:** as run 28, and: a unit thread takes a member only if
  no unit being applied writes any file the member's plan writes, and
  passes over one that shares a file with a member it has passed over
  already, so that members on a file keep their order. The blkseq file is
  not counted. `oa_throughput` has two more cells on four tables `m1` to
  `m4`, each client keeping to one: `ins100m`, 100-row inserts, and
  `upd100m`, updates of 100 rows by a range of the indexed column `a`; and
  it checks the four tables' counts and compares them across nodes.
  Uncommitted.
- **Setup:** as run 28.
- **Results:** `adr-tests-run29.txt`. All six pass in all five modes, in
  705 s. `oa_throughput`, transactions a second:

  | Cell      | Page locks | Unit          | Unit, file per thread | 4 unit threads | 4 threads, file per thread |
  |-----------|-----------:|--------------:|----------------------:|---------------:|---------------------------:|
  | `ins1`    | 9,217      | 11,142 (+21%) | 11,080 (+20%)         | 7,835 (−15%)   | 7,519 (−18%)               |
  | `ins100`  | 93         | 434 (+367%)   | 456 (+390%)           | 491 (+428%)    | 489 (+426%)                |
  | `upd1`    | 9,206      | 9,217 (0%)    | 9,479 (+3%)           | 7,797 (−15%)   | 8,016 (−13%)               |
  | `updk1`   | 8,949      | 9,050 (+1%)   | 9,281 (+4%)           | 6,606 (−26%)   | 6,723 (−25%)               |
  | `updhot`  | 1,622      | 3,119 (+92%)  | 3,251 (+100%)         | 2,882 (+78%)   | 2,817 (+74%)               |
  | `upd20`   | 408        | 542 (+33%)    | 553 (+36%)            | 460 (+13%)     | 524 (+28%)                 |
  | `del1`    | 8,377      | 7,874 (−6%)   | 7,843 (−6%)           | 6,168 (−26%)   | 6,042 (−28%)               |
  | `ins100m` | 332        | 608 (+83%)    | 602 (+81%)            | 1,026 (+209%)  | 994 (+199%)                |
  | `upd100m` | 272        | 333 (+22%)    | 307 (+13%)            | 488 (+79%)     | 486 (+79%)                 |

- **Found:**
  - Units no longer meet: no unit was aborted and applied again in any
    `oa_throughput` cell with four threads (5,958 in `ins100` in run 28).
    In the checking tests, retries with four threads are about what one
    thread has (`oa_bank` 153 against run 26's 157).
  - On four tables, four unit threads are 69% above one on 100-row inserts
    (1,026 against 608) and 47% above on 100-row updates (488 against
    333): three times page locks and 1.8 times.
  - On one table, single-row cells with four threads are 15 to 30% below
    one thread, though only one unit at a time can be on `t`. Their units
    hold twice as many transactions (23,793 for 80,000 inserts against
    45,998), so members waited longer to be taken. The guess: every member
    queued woke an idle unit thread, which took the queue's mutex, found
    nothing it could take and slept. Run 30 tests it.


### 30. Several unit threads, woken only for work they can take

- **When:** started 2026-11-14 19:33 UTC. One node.
- **Change before:** as run 29, and: a unit thread is woken only when one
  is free and a member is waiting that it could take. Before, every member
  queued woke one, and with four threads and every member on the same
  table the three idle ones woke, took the queue's mutex and slept again,
  thousands of times a second. Uncommitted.
- **Setup:** as run 29, `oa_throughput` alone with `OA_MODES="unit units"`.
- **Results:** `adr-tests-run30.txt`. Passes. Transactions a second, one
  unit thread against four: `ins1` 10,485 and 8,155; `upd1` 9,238 and
  7,828; `updk1` 8,979 and 6,700; `updhot` 3,062 and 2,837; `upd20` 539
  and 438; `del1` 7,505 and 5,874; `ins100` 411 and 409; `ins100m` 658 and
  792; `upd100m` 320 and 435.
- **Found:** the guess was wrong. Four threads on one table are still 7 to
  25% behind one, and their units still hold more transactions each.


### 31. The same, with where a unit's time goes

- **When:** started 2026-11-14 19:38 UTC. One node.
- **Change before:** none to the database (run 30's). The tests now report
  `ordered_apply_apply_us` (writing plans to the btrees), `held_us` (a
  unit's begin to the end of its commit) and the members' waits.
  Uncommitted.
- **Setup:** as run 30.
- **Results:** `adr-tests-run31.txt`, with a table at its end worked out
  from the counters: per unit, its transactions, the time writing to the
  btrees and committing, the members' wait, and how much of the cell's
  time a unit was being applied (`busy`).
- **Found:**
  - The unit threads are not waiting: on one table a unit is being applied
    86 to 94% of the time with four threads (74 to 82% with one), and
    never two at once (`busy` under 1, no unit retries). What changes is
    that the same writes take longer. Writing a transaction's plan to the
    btrees: `ins1` 70 µs with one thread and 103 µs with four, `upd1` 74
    and 101, `updk1` 78 and 130, `del1` 91 and 134, `upd20` 1,420 and
    1,906. Commits take 13 against 23 µs. Longer applies make the members
    wait longer, units grow, and the rate falls.
  - The likely reason: when the queue empties, the next member wakes
    whichever idle thread the condition variable picks, so units go from
    thread to thread and core to core, with cold caches. With one thread
    every unit is applied on one hot thread. Run 32 tests it.
  - Four tables: 4.5 and 8.2 ms a unit with four threads against 5.8 and
    11.3 with one, a unit applied 3.4 of the time: side by side, as meant.


### 32. Several unit threads, the last one idle woken first

- **When:** started 2026-11-14 19:43 UTC. One node.
- **Change before:** as run 31, and: each unit thread waits on a condition
  variable of its own, and work wakes the thread idle the shortest time,
  the one that applied the last unit. Uncommitted.
- **Setup:** as run 30.
- **Results:** `adr-tests-run32.txt`. Passes. Transactions a second, one
  unit thread against four, and writing a transaction's plan to the btrees
  (`apply_us` over transactions):

  | Cell      | One thread       | Four threads     | Against one |
  |-----------|-----------------:|-----------------:|------------:|
  | `ins1`    | 10,526 (70 µs)   | 9,445 (88 µs)    | −10%        |
  | `upd1`    | 9,143 (74 µs)    | 8,621 (90 µs)    | −6%         |
  | `updk1`   | 8,989 (82 µs)    | 8,000 (103 µs)   | −11%        |
  | `updhot`  | 3,056            | 2,973            | −3%         |
  | `upd20`   | 540              | 526              | −3%         |
  | `del1`    | 7,685 (93 µs)    | 6,717 (114 µs)   | −13%        |
  | `ins100`  | 409              | 492              | +20%        |
  | `ins100m` | 559              | 879              | +57%        |

- **Found:**
  - Waking the thread that applied the last unit halves the loss on one
    table: 3 to 13% behind one thread, where run 31 had 7 to 25%. So
    units going from thread to thread was most of it.
  - Not all: a transaction's writes still take a fifth more with four
    threads, though on one table the same thread now takes nearly every
    unit. Not found; a profile on xeon needs `perf`, which needs sudo
    there (`perf_event_paranoid` 4).
  - Four tables: still 57% above one thread on 100-row inserts.


### 33. The ADR's tests in five modes, on run 32's code

- **When:** started 2026-11-14 19:48 UTC. One node.
- **Change before:** none (run 32's tree). Uncommitted.
- **Setup:** as run 29, `adr-tests.sh /tmp/rltest3/src`, all five modes.
- **Results:** `adr-tests-run33.txt`. All six pass in all five modes, in
  696 s; no database stopped committing. `oa_throughput`, transactions a
  second, page locks / unit / unit a file per thread / four unit threads /
  four threads a file per thread: `ins1` 8,791 / 11,527 / 10,499 / 9,445 /
  8,999; `ins100` 89 / 461 / 489 / 456 / 461; `upd1` 9,434 / 9,467 / 9,467 /
  8,547 / 8,611; `updk1` 8,979 / 9,238 / 8,820 / 7,812 / 8,008; `updhot`
  1,616 / 3,147 / 3,321 / 3,075 / 3,077; `upd20` 396 / 555 / 524 / 541 /
  541; `del1` 8,122 / 7,692 / 7,700 / 6,975 / 6,975; `ins100m` 316 / 714 /
  658 / 788 / 800; `upd100m` 293 / 338 / 282 / 457 / 458.
- **Found:** correct in every mode. Four threads against one as in run 32:
  on one table 7 to 18% behind on single-row cells and level on the rest
  (`ins100` this time level, +20% in run 32); on four tables +10% on
  inserts and +35% on updates (+57% on inserts in run 32). Single runs of
  a cell here differ by a fifth from run to run.


### 34. Mixed writes on three nodes, page locks against units

- **When:** started 2026-11-14 21:20 UTC, three nodes.
- **Change before:** commit `8064adf64` (several unit threads, run 33's
  code). The image on xeon was run 27's, so it is rebuilt. New workload
  `mix` in `bench_cluster.py`: each statement is an insert of a new row
  with a random key above `KEYS` (40%), an update of `pad` (25%) or of the
  indexed `b` (15%) by a random key in the upper half, or a delete by key
  from the lower half, each key once (20%); a transaction of M rows mixes
  them. `ordered-apply-mixed.sh` runs it, as `ordered-apply-updates.sh`
  runs its cells. Uncommitted.
- **Setup:** working tree shipped to `xeon:/tmp/rlsrc`, `setup-host.sh`
  there, then `KEYS=2000000 VARIANTS="pg unit unit-p unit-m unit-pm"
  ./ordered-apply-mixed.sh`, and a second pass in the opposite order.
  Cells: 1 row 1, 32 and 64 clients; 10 rows 32 and 64 clients; 100 rows
  32 clients.
- **Then:** during pass 1's load, the script's initial load was changed to
  chunk transactions (`set transaction chunk 100` before each `begin`, one
  100,000-row `insert ... select` each, under page locks; before, 20,000-row
  `insert ... select` statements with ordered apply on). Chunking is done
  on the replicant, so the master applies 100-row transactions. Pass 1 loaded the
  old way, pass 2 the new. The rows loaded are the same; the load is not
  measured. The chunked load took 217 s, against 177 s for pass 1's (with
  ordered apply on); run 35 suggests chunks with ordered apply on would be
  faster still, not tried here.
- **Ended:** 21:41 UTC. Pass 1 21:16–21:28, pass 2 21:28–21:41.
- **Results:** `stage1-mixed-run34.results`. Transactions a second, the two
  passes and their mean, against page locks:

  | Clients × rows | Page locks       | Unit                  | Unit, file per thread  | 4 unit threads        | 4 threads, file per thread |
  |----------------|-----------------:|----------------------:|-----------------------:|----------------------:|---------------------------:|
  | 1 × 1          | 422, 474: 448    | 430, 467: 448 (0%)    | 470, 462: 466 (+4%)    | 527, 483: 505 (+13%)  | 486, 470: 478 (+7%)        |
  | 32 × 1         | 8,147, 8,577: 8,362 | 7,445, 8,268: 7,856 (−6%) | 8,059, 8,234: 8,146 (−3%) | 7,049, 7,747: 7,398 (−12%) | 6,802, 7,332: 7,067 (−15%) |
  | 64 × 1         | 8,802, 8,701: 8,752 | 9,741, 9,854: 9,798 (+12%) | 9,390, 10,110: 9,750 (+11%) | 9,692, 10,075: 9,884 (+13%) | 9,569, 10,160: 9,864 (+13%) |
  | 32 × 10        | 1,451, 1,485: 1,468 | 1,211, 1,212: 1,212 (−17%) | 1,383, 1,343: 1,363 (−7%) | 1,199, 1,249: 1,224 (−17%) | 1,292, 1,316: 1,304 (−11%) |
  | 64 × 10        | 1,612, 1,394: 1,503 | 1,410, 1,446: 1,428 (−5%) | 1,755, 1,969: 1,862 (+24%) | 1,374, 1,469: 1,422 (−5%) | 1,848, 1,954: 1,901 (+26%) |
  | 32 × 100       | 61, 82: 72       | 113, 104: 108 (+52%)  | 157, 147: 152 (+113%)  | 111, 115: 113 (+58%)  | 149, 149: 149 (+108%)      |

  Deadlocks, both passes: page locks 8, 13, 183 and 277 in the 32 × 1,
  64 × 1, 32 × 10 and 64 × 10 cells, 1,399 in 32 × 100; with ordered apply
  none outside 32 × 100 but one (unit, 64 × 10), and in 32 × 100 16 (unit),
  60 (a file per thread), 34 (four threads) and 43 (both). In 32 × 100,
  plans that fell back 1 to 9 a pass, stale retries 11 to 29, unit retries
  0 to 12. Tables identical on the three nodes and `verify` clean after
  each pass.
- **Found:**
  - The passes agree better than run 27's (mostly within 10%), so the
    means can be read to about that.
  - Large mixed transactions are where ordered apply wins: 100-row
    transactions 1.5 times page locks with one unit thread and 2.1 times
    with a file per thread, with a twentieth to a thirtieth of the
    deadlocks.
  - 10-row transactions with one unit thread are 5 to 17% behind page
    locks; a file per thread turns 64 clients into +24% and leaves 32
    clients 7% behind. The unit thread applies a 10-row plan in about
    0.6–0.7 ms and is the limit there.
  - Single-row: +11 to +13% at 64 clients in every variant; at 32 clients
    3 to 15% behind, worst with four unit threads (run 32's slower writes
    with several threads, on one table).
  - Four unit threads gain nothing on one table, as runs 29–33 said they
    would not; the variant to use here is one thread with a file per
    thread.


### 35. Chunk transactions under ordered apply

- **When:** 2026-11-14 21:27 UTC, on the Mac, one scratch node, while run 34
  ran on xeon.
- **Change before:** none (commit `8064adf64`, local build).
- **Setup:** three tables like `t` (indexes on `a`, `b`, `c`), each loaded
  with 500,000 rows in five transactions of `set transaction chunk 100`,
  `begin`, one 100,000-row `insert ... select`, `commit` (run 34's load at a
  quarter the size): `t0` with `ordered_apply` 0, `t1` with units, `t2` with
  four unit threads. One after the other, once each.
- **Results:** `t0` 11.9 s, `t1` 8.0 s, `t2` 6.1 s. All three the same
  count and sums, `verify` clean. With ordered apply each load was 5,000
  planned transactions in 5,000 units: no fallbacks, unit retries or stale
  retries.
- **Found:** chunk transactions work under ordered apply. Each chunk reaches
  the master as a transaction of its own, and each is planned. The times
  are single runs in a fixed order on a laptop, so the ordering between
  them isn't established.


### 36. Mixed writes with no row conflicts

- **When:** started 2026-11-14 21:50 UTC, three nodes.
- **Change before:** none to the database (run 34's image, `8064adf64`).
  New workload `mixnc` in `bench_cluster.py`: `mix` with each client
  updating only rows in a slice of the upper half of its own, so no row is
  touched by two clients (deletes already take each key once, inserts make
  new rows). Pages are still shared. Checked offline: in `mixnc` no update
  or delete key appears in two clients' scripts; in `mix` 332 (32 × 1),
  1,240 (64 × 10) and 2,777 (32 × 100) rows did. `ordered-apply-mixed.sh`
  takes `WORKLOAD`. Uncommitted.
- **Setup:** as run 34 (chunked load under page locks in both passes),
  `KEYS=2000000 WORKLOAD=mixnc VARIANTS="pg unit unit-p unit-m unit-pm"
  ./ordered-apply-mixed.sh`, then the opposite order.
- **Ended:** 22:15 UTC. Pass 1 21:50–22:02, pass 2 22:02–22:15.
- **Results:** `stage1-mixnc-run36.results`. Transactions a second, the two
  passes and their mean against page locks, with run 34's mean (row
  conflicts allowed) in brackets:

  | Clients × rows | Page locks            | Unit                       | Unit, file per thread      | 4 unit threads             | 4 threads, file per thread |
  |----------------|----------------------:|---------------------------:|---------------------------:|---------------------------:|---------------------------:|
  | 1 × 1          | 473, 482: 478 (448)   | 502, 514: 508, +6% (448)   | 468, 482: 475, −1% (466)   | 497, 477: 487, +2% (505)   | 485, 520: 502, +5% (478)   |
  | 32 × 1         | 8,301, 8,364: 8,332 (8,362) | 7,742, 8,166: 7,954, −5% (7,856) | 8,196, 8,255: 8,226, −1% (8,146) | 7,443, 7,538: 7,490, −10% (7,398) | 7,100, 7,116: 7,108, −15% (7,067) |
  | 64 × 1         | 8,426, 8,546: 8,486 (8,752) | 10,433, 9,924: 10,178, +20% (9,798) | 9,950, 10,014: 9,982, +18% (9,750) | 9,494, 10,159: 9,826, +16% (9,884) | 9,910, 9,904: 9,907, +17% (9,864) |
  | 32 × 10        | 1,296, 1,509: 1,402 (1,468) | 1,236, 1,196: 1,216, −13% (1,212) | 1,547, 1,396: 1,472, +5% (1,363) | 1,158, 1,237: 1,198, −15% (1,224) | 1,304, 1,497: 1,400, 0% (1,304) |
  | 64 × 10        | 1,324, 1,559: 1,442 (1,503) | 1,492, 1,421: 1,456, +1% (1,428) | 1,840, 1,772: 1,806, +25% (1,862) | 1,419, 1,458: 1,438, 0% (1,422) | 1,806, 1,869: 1,838, +27% (1,901) |
  | 32 × 100       | 64, 84: 74 (72)       | 146, 115: 130, +76% (108)  | 155, 167: 161, +118% (152) | 157, 147: 152, +105% (113) | 167, 151: 159, +115% (149) |

  Deadlocks, both passes: page locks 5, 8, 150, 230 and 1,342 (32 × 1 to
  32 × 100); ordered apply none outside 32 × 100, and 31 to 63 there.
  Tables identical on the three nodes and `verify` clean after each pass.
- **Found:**
  - Taking row conflicts away changes almost nothing: every cell is within
    run-to-run noise of run 34 except 32 × 100 with one unit thread (+20%,
    +35% with four). `mix` had few row conflicts to begin with (314 to
    2,686 rows shared between clients in a cell). Page locks deadlock as
    much without them: their deadlocks are on pages.
  - **A flaw in `mixnc` as run:** each client's slice is 31,250 rows, so
    46 to 58 of the 1,920 100-row transactions updated a row twice. Under
    the default block isolation a transaction does not see its own
    writes, so the second update names the row's old genid and the
    transaction fails at commit (`uncommittable txn on upd ... rc=404`) in
    every variant, page locks included. With ordered apply these are the
    35 to 65 "fallbacks" in 32 × 100 (1 to 9 in run 34): planned, found
    the row gone, run again under page locks, failed. Checked on the Mac
    scratch database: the same five same-row transactions fail or lose a
    write identically with `ordered_apply` 0 and 1, and all commit
    correctly, with no fallbacks, under `set transaction read committed`.
    The scripts send client errors to `/dev/null`, which hid it. About
    2.5% of the 32 × 100 transactions did no work in every variant; the
    smaller cells had almost none. The generator now never writes a row
    twice in a transaction (both `mix` and `mixnc`, checked offline); run
    36's 32 × 100 numbers are not rerun.


### 37. Run 36's 100-row cell again, with no row written twice

- **When:** started 2026-11-15 02:00 UTC, three nodes.
- **Change before:** none to the database (run 34's image, `8064adf64`).
  `bench_cluster.py` no longer writes a row twice in a transaction (run
  36's flaw) and now keeps each client's output and reports
  `client-errors`, the lines with "failed" or "error" in them.
  `ordered-apply-mixed.sh` takes `ONLY100=1` to run only the 32 × 100
  cell. Uncommitted.
- **Setup:** as run 36, with `ONLY100=1`: `KEYS=2000000 WORKLOAD=mixnc
  ONLY100=1 VARIANTS="pg unit unit-p unit-m unit-pm"
  ./ordered-apply-mixed.sh`, then the opposite order.
- **Ended:** 02:11 UTC. **Failed, nothing measured.**
- **Results:** `stage1-mixnc100-run37.results`, not to be used. Every cell
  took 0.12 s at "16,000 txn/s" with no commits on the master, and the
  tables held exactly the 2,000,000 loaded rows after each pass.
- **Found:** my change sent each client's output to `/scripts/*.err`, but
  `/scripts` is mounted read-only in the dev container, so the redirect
  failed and no client ran. The error count read no files and reported 0.
  Fixed: client output goes to `/tmp/cw` in the container and is counted
  there; `bench_cluster.py` fails if fewer files than clients come back or
  the master saw no commits, and `ordered-apply-mixed.sh` stops when it
  fails. Checked with a 4 × 10 cell on the idle cluster: 80 commits, row
  count up 170.


### 38. Run 37 again, with the clients running

- **When:** started 2026-11-15 02:32 UTC, three nodes.
- **Change before:** the fix described under run 37. Uncommitted.
- **Setup:** as run 37.
- **Ended:** 02:45 UTC. Pass 1 02:32–02:39, pass 2 02:39–02:45.
- **Results:** `stage1-mixnc100-run38.results`. 32 clients × 100 rows,
  transactions a second, the two passes and their mean against page locks,
  with run 36's mean (some transactions writing a row twice and failing)
  in brackets:

  | Page locks      | Unit                  | Unit, file per thread  | 4 unit threads        | 4 threads, file per thread |
  |----------------:|----------------------:|-----------------------:|----------------------:|---------------------------:|
  | 50, 65: 58 (74) | 153, 161: 157, +173% (130) | 136, 161: 149, +158% (161) | 153, 154: 154, +167% (152) | 163, 144: 154, +167% (159) |

  Every cell 1,920 commits and no client errors. Page locks 1,187 and 734
  deadlocks; ordered apply none, with no fallbacks, stale retries or unit
  retries in any cell. Tables identical on the three nodes and `verify`
  clean after each pass.
- **Found:**
  - With no row conflicts at all, ordered apply runs 100-row mixed
    transactions with no deadlocks and nothing run again, at 2.6 to 2.7
    times page locks, which still deadlock on pages about every other
    transaction.
  - All four ordered-apply variants are within 6% of each other here; the
    passes differ by up to 17%, so no ranking between them follows.
  - Page locks are 22% below run 36 and one thread of units 21% above.
    Page locks' own passes differ by 26% (50 and 65), so how much of
    that is the fix is not clear.


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

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

## Elsewhere

- A second agent is testing an adaptive mode on the existing rowlocks code
  (page locks while one write is in flight, rowlocks otherwise) on branch
  `rowlocks-adaptive`. Its results will be in
  `results/rowlocks-adaptive.md` on that branch.

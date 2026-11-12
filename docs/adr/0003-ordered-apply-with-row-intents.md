# Ordered apply with row intents

## Status

Proposed — 2026-11-11

Nothing here is implemented. The measurements describe the current system;
the numbers given for the proposal are estimates built from them.

## Context

Writes on the master take page locks through the Berkeley lock manager and
hold them until commit. Transactions that touch different rows on the same
pages block each other and deadlock; a deadlock victim is rolled back and its
whole operation list is run again.

The existing rowlocks mode avoids this by committing every btree operation as
its own Berkeley transaction, with a logical undo record, and holding row locks
until the logical commit. Its latency was never acceptable and it is not
enabled anywhere.

There is a design that keeps SQL on replicants largely lock free: snapshot
cursors read page copies unrolled to a snapshot LSN
(`berkdb/mp/mp_versioned.c`), so readers do not hold page locks. That leaves
writer-against-writer contention on the master as the problem to solve. This
document proposes a replacement for both locking modes on the master.

### How this was measured

- HEAD `82cae2c56`, three nodes in Docker on one host (Xeon E5-2699, 72
  hardware threads), database files on tmpfs, `cachekb 4194304`, `maxwt 32`
  unless a row says otherwise.
- Table `t(a bigint, b bigint, c bigint, pad varchar(64))` with one non-unique
  index on each of `a`, `b` and `c`. Each transaction is one multi-row
  `INSERT`. "Ascending" means `a` increases across all clients, so every client
  inserts into the same right-edge leaf of that index, and `b` and `c` are
  random. "Random" means all three are random, on a table pre-loaded with 4.26
  million rows.
- Clients are `cdb2sql` processes connected to the replicants.
- Thread states come from sampling every thread's stack on the master about
  every two seconds, from the master's request-queue counters
  (`sys.cmd.send('stal')`), and from `perf` with full call graphs.
- One host, a virtual network and no disk I/O: the absolute numbers are low
  and the relative effects are the point. ADR 0001 reports about 100k rows/s
  for uncontended runs on other hardware.

### What limits the master today

#### Multi-row transactions that share pages

| Workload, 100 rows per transaction | Clients | Rows/s | ms per txn | Deadlock replays |
|------------------------------------|--------:|-------:|-----------:|-----------------:|
| Ascending                          |       1 |  6,900 |         15 |                0 |
| Ascending                          |       8 |  7,200 |        111 |              28% |
| Ascending                          |      32 |  5,700 |        565 |              64% |
| Ascending, `maxwt 8` (the default) |      32 |  6,500 |        490 |              67% |
| Ascending, `maxwt 1`               |      32 | 11,500 |        279 |                0 |
| Random, 4.26M-row table            |       1 |  6,400 |         16 |                0 |
| Random, 4.26M-row table            |       8 |  7,200 |        110 |              24% |
| Random, 4.26M-row table            |      32 |  5,400 |        591 |              51% |

Adding clients does not add throughput, and one writer thread on the master
(`maxwt 1`) is twice as fast as 32. Random keys on a multi-million-row table
behave the same as a hot right edge once a transaction has a hundred rows.

Time spent blocked on page locks is small: about half a thread on average.
The cost is in what happens after a deadlock:

1. The victim is rolled back and replayed from the start.
2. Before the replay it sleeps for `rand() % maxwt` times the average
   transaction time, capped at 25 ms per unit (`db/sltdbt.c:231`). With
   `maxwt 32` and these transactions that averages 200–300 ms.
3. Each deadlock raises `gbl_maxwthreadpenalty`, which lowers the number of
   writer threads allowed to take work (`db/sltdbt.c:114`).

In steady state with 32 clients and `maxwt 32`, the master holds 20–22
transactions in its request queue, has about 8.5 writer threads asleep in the
backoff, about half a thread waiting on a page lock, and about one thread
executing. With the default `maxwt 8`, 27–29 of the 32 transactions are in the
request queue and three to five writers are admitted. The master uses about one
core in both cases.

So under page contention the system already runs close to serially, and pays
for discovering that through deadlocks.

#### Single-row transactions

| Workload, 1 row per transaction | Clients | Txn/s | ms per txn | Master cores |
|---------------------------------|--------:|------:|-----------:|-------------:|
| Ascending                       |       1 |   540 |        1.9 |          1.3 |
| Ascending                       |      32 | 7,300 |        4.4 |         10.4 |
| Random, 4.26M-row table         |      32 | 9,200 |        3.5 |         13.6 |
| Random, 4.26M-row table         |      64 | 8,800 |        7.2 |         15.5 |
| Random, `maxwt 8`               |      32 | 7,500 |        4.3 |          7.7 |

Master cores include the trickle thread described under
[Other observations](#other-observations).

With random keys there is almost no lock waiting or deadlocking, and
throughput still stops near 9,000 transactions per second, with 8 writer
threads or 32. The limit is the commit path:

- **Writer threads wait for replicant acks.** The block processor thread that
  ran the transaction waits for the acks itself (`db/glue.c:864`). At 32
  clients about 23 of the 32 writer threads are in that wait at any instant.
- **Every ack wakes every waiter.** All committers wait on one condition
  variable, and each new sequence number from a replicant broadcasts on it
  (`bdb/rep.c:2721`). A third of the master's CPU, 4.7 of 14 cores, is spent
  inside `bdb_wait_for_seqnum_from_node_int`; each "waiting" writer thread
  uses a third of a core.
- **One thread per replicant receives the log.** That thread is at 85% of a
  core on each replicant at 8,700 transactions per second, about 100 µs per
  transaction. Most of that is per commit rather than per log record:
  receiving a similar volume of log records as 100-row transactions takes 23%
  of a core. The master's network writer thread for each replicant is at 65%.
- **The log region mutex.** About one thread at any instant is waiting for
  it, in `__log_put` and in transaction begin, which takes it to read the
  current LSN. `__log_put` also sends each record to the replicants on the
  writer's thread; that send is 6–7% of master CPU.

None of this is page locking, and a new locking scheme does not fix it by
itself. The commit stage of the proposal does; see [Staging](#staging).

#### Cost of a row

For a single client inserting 100 rows per transaction, the block processor
thread spends 60–70 µs of CPU per row, and that is about half of the 14 ms
transaction latency; the ack wait is most of the rest. Of that CPU:

- 74% is btree puts, about 12 µs each; there are four per row, one for the
  data and one for each key.
  - 36% is the search from the root. It includes most of the lock manager
    calls (12% of the thread, 20–25 lock requests per row) and buffer pool
    gets (10%).
  - 30% is inserting the item and logging it. Within that, sending the log
    records to replicants is 12% and compressing the key and data on the page
    is 10%.
- The remaining 26% is reading the operation list, forming keys, and commit.

A row with three keys writes eight `__db_addrem` log records
(`berkdb/db/db_dup.c:241` logs each key item and each data item separately)
and about 1.0 KB of log.

#### The existing rowlocks mode

| Ascending, 100 rows per transaction | Clients | Rows/s | ms per txn |
|-------------------------------------|--------:|-------:|-----------:|
| Page locks                          |       1 |  6,900 |         15 |
| Rowlocks                            |       1 |  2,900 |         35 |
| Rowlocks                            |       8 |  8,100 |         99 |
| Rowlocks                            |      32 |  7,900 |        407 |

A 100-row transaction writes 401 commit records and 400 logical undo records
in addition to the same 803 `__db_addrem` records: 16 log records and 2.0 KB
per row. Each of those commit records is a transaction for the replicant to
apply, and the replicant's single receive thread sits at 93% of a core. On the
master, 18 of 32 writer threads are waiting for acks in the final physical
commit, which happens before the row locks are released
(`bdb/tran.c:1943`–`1959`).

#### Other observations

- Under any steady write load the trickle thread never sleeps: it repeats
  immediately whenever a pass wrote at least one page (`bdb/threads.c:136`),
  and every pass scans every hash bucket of the buffer pool. With a 4 GB cache
  that is 0.6–0.8 of a core on each node at 500 transactions per second.
- With the default 64 MB cache this test spends much of its CPU reading pages
  back in. The numbers above use 4 GB.

### What page locks are needed for

Three pieces of the system depend on page locks being held to commit, and all
three depend on the same property:

> On every page, log records appear in commit order, and the records of
> uncommitted transactions are the most recent ones.

- **Replicant apply.** A replicant applies whole transactions in commit-record
  order, after taking the page locks listed in the commit record. A page
  record can be redone only when the page LSN equals the LSN stored in the
  record.
- **Physical undo.** Recovery and log truncation undo a record only when the
  page LSN equals that record's own LSN (`berkdb/db/db_rec.c:285`), so an
  uncommitted record can be undone only while it is the newest on its page.
- **Snapshot unroll.** `__mempv_fget` walks a page's records backwards, undoing
  them, and stops at the first one whose transaction committed at or before
  the snapshot (`berkdb/mp/mp_versioned.c:372`).

Holding page locks until commit is one way to get that property. Ordering the
commits is another, and it does not require any writer to wait for a page.

## Proposal

The master has a transaction's complete operation list before it starts. That
lets it settle every logical conflict before it touches a page, so the part
that modifies pages can no longer fail and never needs to be undone in normal
operation.

A transaction goes through four stages.

### 1. Prepare: parallel, nothing held

The operation list runs through the existing record-level code
(`db/record.c`, `db/indices.c`, `db/constraints.c`) with a new transaction
class at the `bdb/ll.c` layer, the same place where `TRANCLASS_LOGICAL`
diverts to `bdb/phys.c` today (`bdb/ll.c:246`).

- Writes (`ll_dta_add`, `ll_key_add`, `ll_dta_upd`, and so on) are recorded in
  a per-transaction **plan** instead of being applied. The plan is the exact
  list of btree operations with their keys and payloads.
- Reads go to the btree for the latest state, with the transaction's own plan
  laid over the result, so a transaction sees its own earlier operations.
- Each read of a row or key that another transaction could change records a
  **version**: the sequence number of the last transaction that wrote it,
  taken from a fixed-size in-memory array indexed by a hash of the row or key.

Verify errors, duplicate keys, constraint failures and cascades are all found
here. Nothing has been written, so a failure has nothing to roll back, and a
verify error goes back to the replicant as it does today. The page reads also
bring the pages the apply stage will need into the buffer pool.

### 2. Sequence: a short critical section

Under one mutex:

1. Check that every version recorded during prepare is unchanged.
2. Take the next sequence number. It exists only in memory and is never
   logged.
3. Write that number into the version slot of every row and key the
   transaction changes. These are its **intents**.
4. Append the plan to the queue of each btree it touches.

Intents take the place of row locks:

| What                    | Intent on                         | Taken by                     |
|-------------------------|-----------------------------------|------------------------------|
| A row                   | (table, genid)                    | update, delete               |
| A unique key            | (index, key)                      | insert, delete, key change   |
| A foreign key reference | (child index, parent key prefix)  | child insert, delete, update |

Entries in non-unique indexes need no intent of their own; they belong to the
row. An insert into a table with no unique keys and no foreign keys takes no
intents at all.

If a version has changed, another transaction sequenced a change to that row
or key first. The transaction waits for that one to finish applying, reads
again, and either carries on or returns the verify or duplicate error. The
transaction being waited on is past the point where it can fail and never
waits for an unsequenced transaction, so these waits cannot form a cycle.
There is no deadlock detector involved.

A hash collision makes two keys share a slot. The effect is a spurious re-read,
never a missed conflict.

### 3. Apply: one thread at a time per btree, in sequence order

Each btree file has a queue of plans. Whichever thread finds the queue
unattended drains it, so under light load a transaction's own thread applies
its operations and under load one thread applies a batch for many
transactions. Operations use the ordinary btree code and write the ordinary
log records under the owning transaction's id.

- A page lock is held for one operation and then released. The page is
  remembered for the commit record's lock list, which replicants still need.
- Data stripes, indexes and blob files are separate btrees and are applied in
  parallel.
- With one writer per btree there are no writer-against-writer page deadlocks.
  A reader can still deadlock with a split; readers always lose and retry
  their lookup.

A hot leaf is now held for one operation, about 12 µs today, instead of for
the rest of a transaction.

### 4. Commit: ordered by dependency, batched, off the writer thread

A transaction may write its commit record when all its operations are applied
and every transaction it depends on has committed. It depends on:

1. the last uncommitted transaction to modify each page it modified;
2. any uncommitted transaction whose write it read during prepare;
3. any uncommitted transaction that validated a read of a key it then wrote.

The first keeps page records in commit order. The second and third keep the
commit order a valid serial order, which page read locks held to commit
provide today. Because each btree is applied in sequence order, dependencies
always point to earlier transactions and cannot form a cycle.

Two further changes belong to this stage and address the single-row limits
measured above:

- **No thread waits for acks.** A committed transaction is handed to a waiter
  list ordered by LSN. When the acknowledged LSN advances, only the
  transactions it covers are completed.
- **Commits that are ready together are written together.** When several
  transactions are ready at once they can be committed as one unit: one commit
  record, with the members attached as child transactions. Replicants already
  collect child transactions through `__txn_child` records
  (`berkdb/rep/rep_record.c:6762`) and apply the whole unit in LSN order, and
  the commit-LSN map already gives children the parent's commit LSN. One unit
  costs the replicant's receive thread one commit instead of many.

### An example

T1 and T2 each insert rows whose `a` key lands on the right-edge leaf P of the
ascending index, and whose other keys land elsewhere.

1. Both prepare at the same time. Neither reads anything the other writes.
2. T1 is sequenced as 41, T2 as 42. Both plans are appended to the queue of
   each btree they touch.
3. The thread draining the index on `a` applies T1's entries and then T2's on
   P, a few microseconds apart. T2 records a dependency on T1 because T1 was
   the last uncommitted writer of P.
4. T1's other operations finish; T1 commits. T2 finishes; its dependency is
   satisfied; T2 commits. If both are ready together they commit as one unit.

Under page locks T2 would have waited for T1's commit before touching P, and
if T1 had then needed a page T2 already held, one of them would have been
rolled back and replayed.

### Readers on the master

SQL running on the master must not sit on a page lock, or it stalls the apply
thread for that btree. It uses the same snapshot cursors as on replicants,
taking page copies and unrolling records of transactions that have not
committed. Pages changed by a sequenced but uncommitted transaction are
unrolled exactly as pages changed by an uncommitted transaction are today.

### Cases that need more than the four stages

- **Master downgrade or loss of mastership during apply.** Sequenced
  transactions without a commit record have log records that no replicant has
  applied. The old master must undo them together, in reverse LSN order across
  all of them; undoing one transaction at a time is not valid when their
  records interleave on a page. Crash recovery already works this way and
  needs no change.
- **Two-phase commit.** A prepared participant holds its intents in a pending
  state and applies nothing until the coordinator decides. Transactions that
  conflict with a pending intent wait for the decision. The prepare record
  must carry the plan so a new master can restore it; this is the one new log
  record type.
- **Serializable validation.** The transaction is sequenced in the pending
  state, the existing read-set check runs once every earlier transaction is in
  the log, and only then does apply begin.
- **Errors during apply.** These are I/O-class failures. The master treats
  them as a replicant treats a failed apply, and gives up mastership.
- **Large transactions.** In stage 1 a large transaction delays every later
  transaction queued behind it on the same btree. Stage 2 applies it in
  slices, so later small transactions can go ahead; a small transaction that
  ends up mutually dependent with it commits in the same unit.
- **Other writers.** Schema-change converters, queue consumers and any other
  code that writes table btrees must go through the same stages.

## Why the rest of the system is unaffected

| Relies on page locks today             | What it needs                                           | How it is provided                          |
|----------------------------------------|---------------------------------------------------------|---------------------------------------------|
| Replicant apply                        | Page records in commit order; page list in the commit   | Dependency 1; apply records the pages       |
| Physical undo in recovery, truncation  | Uncommitted records are the newest on each page         | Nothing commits ahead of what it stacked on |
| Snapshot unroll                        | The same                                                | The same                                    |
| Isolation between writers              | No lost updates; uniqueness; foreign keys               | Intents checked at sequencing               |
| Commit order is a serial order         | Reads stay valid until commit                           | Dependencies 2 and 3                        |

The log produced is one a page-locked execution could have produced. Apart
from the new prepare record for distributed transactions, stage 1 changes no
log record, no page format and nothing on replicants, so it needs no import and
can be rolled out node by node.

## Expected effect

These are estimates from the measurements above, not results.

- **Page contention.** Deadlock replays on the master go to zero. The floor
  for the contended case is the serial rate measured with `maxwt 1`, 11,500
  rows/s, twice what 32 writer threads achieve now. Beyond that, prepare runs
  in parallel and apply runs in parallel across btrees. One apply thread per
  btree at today's 12 µs per put allows about 80,000 operations per second
  through a single index on this hardware, so that is not expected to be the
  first limit; replicant apply and log volume are.
- **Single-writer latency** stays at page-lock levels: the same log records,
  no commit per operation, and one extra search for each updated or deleted
  row. It should not show the 2.3 times penalty of the existing rowlocks mode.
- **Single-row throughput.** Taking the ack wait off the writer threads
  removes the cap of writer threads divided by ack latency and the third of
  master CPU spent in wake-ups. Committing ready transactions as one unit
  divides the replicant's per-commit cost by the size of the unit.
- **Lock manager.** Stage 1 keeps the same number of lock manager calls.
  Stage 2 removes them from the write path.

## Staging

0. **Commit path, independent of locking.** Wake only the committers whose LSN
   has been acknowledged, and stop holding a writer thread through the ack
   wait. This is small, addresses the single-row limits directly, and is worth
   doing whatever happens to the rest.
1. **Ordered apply.** The four stages, using short page locks through the
   existing lock manager. The first slice is inserts into tables with no
   unique keys or foreign keys: it needs no intents, only the queues, early
   lock release and ordered commit, and it would show whether the hot-leaf
   workload scales as estimated.
2. **Latches and finer scheduling.** Page latches in the buffer pool instead
   of lock manager locks; slicing of large transactions; commit units; several
   apply threads per btree, partitioned by key range.
3. **Row-level log records.** One record per row operation instead of eight
   `__db_addrem` records. This is the stage where an incompatible format would
   pay, and it is separable from the rest.

## Risks and open questions

- **Plan mode for the block processor is the bulk of the work.** Deferred key
  adds, the constraint temp tables, cascades, triggers and live schema-change
  mirroring all have to produce plan entries, and every read they do has to go
  through the plan and record a version. A missed read is a correctness bug,
  so the list of reads needs to be exhaustive and the rules need a model or
  proof as well as the Jepsen suite.
- **A page missing from the buffer pool stalls an apply thread**, and with it
  every transaction queued on that btree. Prepare touches the same pages just
  before, so this should be rare, but it has to be measured on real disks.
- **Single-thread apply rate per btree** is estimated from today's cost per
  put. If a production workload pushes one index past it, partitioned apply
  moves from stage 2 to stage 1.
- **Master SQL depends on the snapshot cursor work** being complete enough
  that no reader on the master holds a page lock between calls.
- **Commit units make the replicant's unit of apply larger.** That is good for
  the receive thread and needs a size limit so one unit does not hold too many
  page locks on the replicant at once.
- **Memory.** A plan holds keys and row images until applied. Large plans need
  to spill to temp tables as the operation list does today.

## Alternatives considered

- **Row locks with page latches and logical undo**, which the existing
  rowlocks mode approximates. Uncommitted changes from different transactions
  interleave on a page, so uncommitted records are no longer the newest on
  each page. Replicant apply, snapshot unroll and physical undo would all need
  rewriting, replicants would hold uncommitted data, and row-level deadlocks
  would remain.
- **Taking every row lock up front in a fixed order.** This needs the same
  prepare pass to learn the lock set, and leaves page contention as it is.
- **Running one writer thread.** This is the `maxwt 1` measurement: twice the
  contended throughput with no code change, and worth considering as an
  interim setting for workloads known to deadlock. It serializes whole
  transactions including their ack waits, and one large transaction stalls
  everything.
- **Row-versioned storage, or an in-memory table in front of the btrees.**
  Either removes the problem by replacing the storage layer, replication and
  the snapshot design.

## Reproducing the measurements

The scripts are in `docs/adr/0003-bench/`. They need a Linux host with Docker
(with the Compose plugin) and `python3`, and enough cores that three nodes and
the clients do not compete; the numbers above used 72 hardware threads. All
state lives in `/tmp/rlbench`, which should be tmpfs to match those numbers.

```sh
cd docs/adr/0003-bench
./setup-host.sh          # copies git HEAD to /tmp/rlbench/src and builds the image
cd /tmp/rlbench

# Create a 3-node cluster and the test table.
#   arg 1: lrl lines used at create time ("" or "init_with_rowlocks")
#   arg 2: runtime lrl lines, ';'-separated
./newcluster.sh "" "maxwt 32;cachekb 4194304"

# One run: <ascending: mono | random: rand> <clients> <rows per txn> <txns per client>
python3 bench_cluster.py mono 32 100 60
python3 bench_cluster.py rand 16 500 375      # pre-load ~3M random rows (several minutes)
python3 bench_cluster.py rand 32 1 2000

# Change the writer-thread limit on the running master (node name from newcluster.sh):
docker exec rlbench-dev-1 cdb2sql testdb --host node3 "exec procedure sys.cmd.send('maxwt 1')"

# Thread states on the master and one replicant, plus request-queue snapshots:
#   <label> <workload> <clients> <rows per txn> <txns per client> [samples] [interval s]
./prof.sh contended mono 32 100 100 25 2.0
python3 fold.py prof/contended/master 20      # states, then the 20 most common stacks
python3 fold.py prof/contended/rep
cat prof/contended/queue.*.txt

# CPU profiles: by thread and symbol, or with full call graphs
./perfflat.sh flat rand 32 1 5000 master      # or: replicant
./perfdwarf.sh graph mono 1 100 1100 master   # see prof/graph/perfd.children.txt

./logrecs.sh             # log bytes and record types for one 100-row transaction

/path/to/repo/docs/adr/0003-bench/teardown-host.sh   # removes cluster, image, /tmp/rlbench
```

Notes:

- `setup-host.sh` builds from the committed tree; uncommitted changes are not
  in the image. The image is `contrib/docker/Dockerfile.dev` with `gdb`,
  `elfutils` and `perf` added. Containers run with all capabilities, which
  `gdb` and `perf` need.
- `bench_cluster.py` prints throughput, the master's deadlock and lock-wait
  counters, CPU cores used by each node, and per-transaction times from the
  master's event log. The event log's queue time under-reports time spent in
  the request queue; use the `queue.*.txt` snapshots from `prof.sh` for that.
- `prof.sh` pauses each sampled process for about 0.2–0.5 s per sample, so
  throughput during a profiled run is lower than in a plain run. Take
  throughput from plain runs.
- A workload must run long enough to cover the samples: about
  `samples × (interval + 0.5)` seconds plus 20 seconds.
- Per-thread CPU came from `top -H -b -d 6 -n 2 -p 1` inside a node container
  during a run.
- `results/` holds the plain-run output behind the tables above, for page
  locks and for the existing rowlocks mode.

## References

- Deadlock backoff and writer penalty: `db/sltdbt.c:114`, `db/sltdbt.c:231`
- Ack wait and its wake-up: `bdb/rep.c:2721`, `bdb/rep.c:3243`,
  `db/glue.c:864`
- Rowlocks final commit: `bdb/tran.c:1943`
- Trickle loop: `bdb/threads.c:136`
- Page-LSN rules for redo and undo: `berkdb/db/db_rec.c:285`
- Snapshot unroll: `berkdb/mp/mp_versioned.c:372`
- Child transactions on replicants: `berkdb/rep/rep_record.c:6762`
- Transaction-class dispatch: `bdb/ll.c:246`
- ADR 0001, multiple freelists per btree, for uncontended throughput and an
  earlier profile of the commit path

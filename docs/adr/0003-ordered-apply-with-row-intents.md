# Ordered apply with row intents

## Status

Proposed — 2026-11-11

Stage 0, the first step of stage 1.1, a first form of commit units, inserts
into tables with unique keys, and updates and deletes of rows in tables with
no foreign keys are implemented and measured on the `rowlocks-ideas` branch,
behind tunables; see
[Implementation status](#implementation-status) for what exists, what it
measured and where to resume. The measurements under Context describe the
system before that work, and the numbers under
[Expected effect](#expected-effect) are estimates built from them.

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

> For any one page, the log records that modify it appear in the log in the
> commit order of their transactions, and records of uncommitted transactions
> come after all committed ones.

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

The master can work out a transaction's complete set of btree operations
before it modifies any page. The operation list it receives is not complete by
itself, because foreign key checks and cascades add to it as it runs, but all
of that is done by reading. So the master can settle every logical conflict
before it touches a page, and the part that modifies pages can no longer fail
and never needs to be undone in normal operation.

A transaction goes through four stages. The first two are logical and touch
only memory. The last two are physical: they modify pages and write the log.

### Names and stamps

Conflicts are detected on **names**. A name identifies something two
transactions can conflict over:

| What                    | Name                              | Written by                   |
|-------------------------|-----------------------------------|------------------------------|
| A row                   | (table, genid)                    | update, delete               |
| A unique key            | (index, key)                      | insert, delete, key change   |
| A foreign key reference | (child index, parent key prefix)  | child insert, delete, update |

Entries in non-unique indexes need no name of their own; they belong to the
row. An insert into a table with no unique keys and no foreign keys writes no
names at all.

Assume for now a table with unlimited room, the **stamp table**, that maps
every name to the sequence number of the last transaction that wrote it. That
number is the name's **stamp**. A name that was never written has stamp 0.
[A stamp table that fits in memory](#a-stamp-table-that-fits-in-memory)
replaces this with something that can be built.

A stamp is not a lock. It has no owner, it is never released and nothing
queues on it; it records only which transaction wrote the name last. The names
a transaction stamps are its **intents**.

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
- Each read of a row or key that another transaction could change also notes
  the stamp of its name. If that stamp belongs to a transaction that has not
  finished applying to the file being read, the read first waits until it has,
  so that the stamp noted and the data read agree.
- Foreign key checks and cascades run here, and add operations to the plan
  from what they read.

Verify errors, duplicate keys, constraint failures and cascades are all found
here. Nothing has been written, so a failure has nothing to roll back, and a
verify error goes back to the replicant as it does today.

Prepare also **prefetches**: it searches for every page the plan will write,
so those pages are in the buffer pool before the transaction is sequenced. The
reads above do not cover this by themselves. An insert into a non-unique index
needs no read, so without the prefetch its leaf page is first touched in
apply. A second prefetcher walks each file's queue ahead of the apply thread
and brings back pages evicted since prepare.

### 2. Sequence: a short critical section

Under one mutex:

1. Check that every name read during prepare still has the stamp that was
   noted.
2. Take the next sequence number. It exists only in memory and is never
   logged.
3. Write that number as the stamp of every name the transaction changes.
4. Append the plan to the queue of each file it touches.

A transaction **conflicts** when a name it read has a different stamp by the
time it reaches the mutex. Another transaction was sequenced with a change to
that name after the read, so what was read may be stale. Every write is
preceded by a read of the same name (a delete reads the row; an insert reads
the unique key to check for a duplicate), so two writers of one name are
caught the same way.

The first transaction to reach the mutex wins. The other waits for it to
finish applying, repeats the reads that were stale, and either tries the mutex
again or returns the verify or duplicate error. Repeating a read can change
the plan as well as the verdict: a cascade may find a child row that was not
there before.

The transaction being waited on is past the point where it can fail and never
waits for an unsequenced transaction, and the waiter holds nothing. These
waits cannot form a cycle, and there is no deadlock detector involved.

### A stamp table that fits in memory

The stamp table as described cannot be built: it would hold every row and key
ever written. It can be made finite because most stamps are dead.

A stamp matters only to a transaction that noted it during prepare and has not
yet been sequenced. Once the transaction that wrote a stamp has been applied,
and every prepare still running started after it was sequenced, nothing can
tell that stamp from 0. With no transactions running and the queues empty, the
whole table could be emptied.

That allows two constructions:

- **Exact, with eviction.** A hash table keyed by name. An entry is removed
  once it is dead by the rule above, and a missing entry reads as 0. Each
  prepare notes the sequence counter when it starts and treats every stamp at
  or below that as 0, so an eviction during the prepare is not mistaken for a
  change. There are no false conflicts. The cost is allocation and eviction,
  some of it under the sequence mutex.
- **Lossy, fixed size.** An array of sequence numbers indexed by a hash of the
  name. The name is not stored and nothing is evicted; a slot is simply
  overwritten. Two names that hash to the same slot share a stamp, so a write
  to one looks like a write to the other. The effect is a read repeated for
  nothing, never a missed conflict.

The array is a lossy compression of the exact table. Everything else in the
design is the same for both, and the rest of this document says "stamp table"
for either.

The size of the array sets the rate of false conflicts. As a rough guide,
using assumed figures: a transaction reads 100 names during a 5 ms prepare
while the system stamps 200,000 names per second, so about 1,000 stamps land
during its prepare. The chance that one of them hits a slot the transaction
read is about 100 × 1,000 ÷ slots: 10% with a million slots (8 MB), 0.6% with
16 million (128 MB). Large transactions are affected most.

Either way the table exists only in memory on the master. A new master starts
with an empty one, so a prepare that is running across a change of master is
discarded and run again.

### 3. Apply: one thread at a time per file, in sequence order

A **file** here is one table's data, one index or one blob. Some of these are
several files on disk, such as data stripes; whether each of those gets its
own queue is left open.

Each file has a queue of plans. Whichever thread finds the queue unattended
drains it, so under light load a transaction's own thread applies its
operations and under load one thread applies a batch for many transactions.
Operations use the ordinary btree code and write the ordinary log records
under the owning transaction's id.

- A page lock is held for one operation and then released. The page is
  remembered for the commit record's lock list, which replicants still need.
- Separate files are applied in parallel.
- With one writer per file there are no writer-against-writer page deadlocks.
  A reader can still deadlock with a split; readers always lose and retry
  their lookup.

A hot leaf is now held for one operation, about 12 µs today, instead of for
the rest of a transaction.

#### Transaction boundaries

The queues are per file; the transaction is not. Each sequenced transaction
has one Berkeley transaction, and every apply thread that executes one of its
operations logs under that transaction's id. An apply thread moving from one
plan in its queue to the next switches transaction; it begins and commits
nothing itself.

A transaction is **applied** when the last queue holding any of its operations
has finished them. Only then, and only when its dependencies have committed,
is its one commit record written, with the pages collected by all the apply
threads.

Replicants see no difference. A replicant does nothing with a transaction's
records until the commit record arrives. It then collects all of them, takes
the page locks listed in the commit record, applies the records in LSN order
and releases the locks. It cannot apply a transaction's index write and
release locks before the data write, because it applies neither until the
commit record, and that is written after both.

One thing does change on the master. A transaction's log records are chained
through their previous-LSN field, which replicants follow back from the commit
record to collect the transaction (`berkdb/rep/rep_record.c:6708`) and which
undo follows too. Each log write reads and updates the transaction's last LSN
with no lock, because today one thread writes all of a transaction's records.
With one apply thread per file, several threads write them at once. Two ways
to keep the chain intact:

- A latch around each log write. Log writes are serialized already, by the log
  region mutex, so this may need no new latch: the record's previous LSN would
  be filled in and the transaction's last LSN updated while that mutex is
  held, where today the record is built before taking it.
- A child transaction for each file the transaction touches, each with its own
  chain, joined to the parent by a `__txn_child` record before the commit.
  Replicants already collect children this way. It costs one more log record
  for each file in each transaction.

### 4. Commit: ordered by dependency, batched, off the writer thread

A transaction may write its commit record when all its operations are applied
and every transaction it depends on has committed. It depends on:

1. the last uncommitted transaction to modify each page it modified;
2. any uncommitted transaction whose write it read during prepare;
3. any uncommitted transaction that validated a read of a key it then wrote.

The first keeps page records in commit order. The second and third keep the
commit order a valid serial order, which page read locks held to commit
provide today. Because each file is applied in sequence order, dependencies
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

#### Failed requests wait too

A request that fails is also a commit. Its error reply is written to blkseq
and committed (`db/toblock.c:5984`), and the reply goes out only after
replicants acknowledge that commit. Today this is what keeps an error from
outliving its cause:

1. T1 commits on the master and releases its page locks. No replicant has
   acknowledged it yet.
2. T2 finds T1's key and fails with a duplicate. Its blkseq commit is later
   in the log than T1's commit.
3. Replicants apply commits in LSN order, so a replicant that acknowledges
   T2's commit has applied T1.
4. If the master is lost before those acknowledgements arrive, T2's reply is
   never sent. The client retries on the new master, where T1 does not exist,
   and the insert succeeds.

Nothing states this as a rule today. It follows from writing the commit
record before releasing locks: a change can be seen only once its commit is in
the log, so whatever fails because of it commits later in the log.

With page locks released after each operation, T2 can see T1's key before T1
has a commit record. T2's blkseq commit can then be in the log ahead of T1's.
A replicant acknowledges it without having applied T1, because it applies
nothing of T1 until T1's commit record arrives. T2's error goes to the
application. If the master is now lost, the new master rolls T1 back, and an
application that looks the key up does not find it.

So the dependencies above apply to every commit a request makes, whether the
request succeeds or fails. **A request does not write its commit record,
including the blkseq record of an error reply, until every transaction whose
writes it read has committed.** The same holds for a verify error, a missing
parent key or any other error built from what prepare read. The rule is
transitive: if T2 read from T1 and T1 read from T0, T1 cannot commit before
T0, so T2 cannot either. Since a reply waits for acknowledgements of its own
commit, every node that acknowledged it has everything the reply depended on.

The guarantee is only as strong as the ack wait, the same as today. If
replicants that do not answer in time are made incoherent and the reply goes
out anyway, losing the master can lose T1 after T2's error has been returned,
just as it can lose a commit that was reported as successful. `durable_lsns`
turns such a reply into `NOT_DURABLE`, and the client retries.

The current implementation holds page locks to commit, so a request never
reads a change before it commits, and none of this applies yet. The read
`uniq_read` makes in prepare does not wait for a locked page, and so does not
see a key added by a transaction that has not committed.

### An example

T1 and T2 each insert rows whose `a` key lands on the right-edge leaf P of the
ascending index, and whose other keys land elsewhere.

1. Both prepare at the same time. Neither reads anything the other writes.
2. T1 is sequenced as 41, T2 as 42. Both plans are appended to the queue of
   each file they touch.
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
thread for that file. It uses the same snapshot cursors as on replicants,
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
  transaction queued behind it on the same file. Stage 2 applies it in
  slices, so later small transactions can go ahead; a small transaction that
  ends up mutually dependent with it commits in the same unit.
- **Other writers.** Schema-change converters, queue consumers and any other
  code that writes table btrees must go through the same stages. Tagged-api
  (non-SQL `OP_BLOCK`) writes do not; they are excluded by requiring
  `disable_tagged_api_writes` with `ordered_apply`.

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
  in parallel and apply runs in parallel across files. One apply thread per
  file at today's 12 µs per put allows about 80,000 operations per second
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
   existing lock manager, in three steps that differ only in when the next
   transaction may start applying. Prepare, stamps and sequencing, where a
   mistake is a correctness bug, are the same in all three.

   1. **Serial apply.** Transaction N+1 does not start applying until N has
      written its commit record. Its files are still applied in parallel. At
      most one uncommitted transaction is on the pages at any time, so the
      commit dependencies hold without being tracked and losing mastership
      means aborting one ordinary transaction. Estimated from 12 µs per put
      and four puts per row: about 20,000 rows/s applied on one thread, up to
      about 80,000 for large transactions with the four files in parallel,
      and about 20,000 single-row transactions per second. The cost is that
      every write waits behind the transaction being applied, whether or not
      it is related. Inserts into tables with no unique keys or foreign keys
      can come first within this step, since they write no names.
   2. **Serial per file.** N+1 may start if it shares no file with an earlier
      uncommitted transaction. Different files have different pages, so there
      is still one uncommitted transaction per page and each can be aborted
      alone. Transactions on unrelated tables no longer wait for each other.
   3. **Ordered per page.** The design as described above: transactions stack
      on a page and commit in that order, with the dependency tracking and
      the group undo that requires.
2. **Latches and finer scheduling.** Page latches in the buffer pool instead
   of lock manager locks; slicing of large transactions; commit units; several
   apply threads per file, partitioned by key range.
3. **Row-level log records.** One record per row operation instead of eight
   `__db_addrem` records. This is the stage where an incompatible format would
   pay, and it is separable from the rest.

## Implementation status

As of 2026-11-14. Everything below is on the `rowlocks-ideas` branch. PR 6144
is merged there as a local merge commit, and the ordered-apply work is
committed on top of it. Nothing has been pushed. Do not push this work to
public GitHub; pushing to xeon is fine.

### Stage 0: the ack wait, from PR 6144

Stage 0 is upstream PR 6144 (`async_dist_commit`, on by default), used as it
is. The block processor hands a committed request to one background thread,
which checks the acks for every outstanding commit and sends the reply.

Checked: the change was read against the inline wait it replaces; sections
1–5 of `tests/async_dist_commit.test` pass on the three-node cluster; with one
replicant paused for 8 seconds under load, all 96,000 transactions committed,
every node had the same row count and all nodes returned to coherent. Sections
6–11 of that test need the test harness (ssh, killing and restarting nodes)
and were not run.

| Workload                         | Off               | On           | Master cores, off → on |
|----------------------------------|------------------:|-------------:|-----------------------:|
| Random, 1 row, 32 clients        | 8,970–9,083 txn/s | 8,419–8,908  | 13.0 → 8.4             |
| Random, 1 row, 64 clients        | 8,246–8,725 txn/s | 9,117–9,202  | 13.2–14.1 → 8.9        |
| Random, 1 row, 128 clients       | 8,187–9,922 txn/s | 9,291–9,299  | 13.2–14.5 → 9.3–9.5    |
| Ascending, 1 row, 32 clients     | 7,035–7,181 txn/s | 6,326–7,535  | 9.5–9.9 → 6.5–7.7      |
| Ascending, 100 rows, 32 clients  | 5,418–5,623 rows/s | 6,770–6,788 | 1.0–1.1 → 1.3–1.4      |

It removes about a third of the master's CPU. Single-row throughput stays near
9,000 transactions per second, so the writer threads were not what capped it.

### Stage 1.1, first step: inserts that write no names

Tunables, all experimental: `ordered_apply` (off by default),
`ordered_apply_threads` (3), `ordered_apply_parallel_min_ops` (64),
`ordered_apply_serial_commit` (on), `ordered_apply_turn_detect` (on; off is
for measurement only, see
[One node, and what the turn costs](#one-node-and-what-the-turn-costs)),
`ordered_apply_unit` (off), `ordered_apply_unit_max_ops` (4096),
`ordered_apply_unit_inline` (on) and `ordered_apply_unit_detect_ms` (100),
for [Commit units](#commit-units), `ordered_apply_unique` (on), for
[Unique keys](#unique-keys), and `ordered_apply_update` (on), for
[Updates and deletes](#updates-and-deletes). The code is `bdb/plan.c`, with
hooks in `bdb/ll.c`, `bdb/tran.c`, `db/toblock.c`, `db/sltdbt.c`,
`db/glue.c`, `db/osqlcomm.c` and `db/record.c`. The first step is described
as it was built; later sections say what has changed since.

`ordered_apply` can be turned on only while `disable_tagged_api_writes` is
on, and `disable_tagged_api_writes` cannot be turned off while
`ordered_apply` is on (`db/db_tunables.c`). Tagged-api writes have no OSQL
session, so they never enter plan mode. Today they would only wait on page
locks alongside plans, but once plans release page locks early they would
read changes that are not committed, and they write no stamps. A database
that takes tagged writes keeps ordered apply off. An lrl that uses legacy
defaults, which allow tagged writes, has to set `disable_tagged_api_writes`
before `ordered_apply`.

What it does:

- **Prepare.** A plain SQL write request starts in plan mode. `ll_dta_add` and
  `ll_key_add` record the operation, with its key and its payload already
  packed and compressed, instead of writing. Only adds to data files, blob
  files and indexes that allow duplicates are planned. These write no names,
  so there is no stamp table yet.
- **Fallback.** Anything else abandons the plan before a page is written and
  the request runs again at once under page locks held to commit: a unique
  key, a foreign key, an update, a delete, a table under schema change, a
  distributed transaction, or DDL. A transaction found holding a page lock
  when it is about to wait for its turn falls back the same way; that catches
  writes and reads that do not go through `ll.c`, such as queue adds for
  triggers.
- **Sequence.** The apply turn is a write lock in the Berkeley lock manager.
  Waiters are served in order, and the deadlock detector sees a wait for the
  turn, which matters when a transaction outside plan mode sits between two
  that are in it.
- **Apply.** The plan is replayed through the same `ll_*` routines under the
  request's own Berkeley transaction, so the log records are the ordinary
  ones. A plan of 64 or more operations is split by btree file, and each file
  is applied on its own thread under its own child transaction. The children
  are begun and committed on the owning thread.
- **Turn release.** With `ordered_apply_serial_commit` on, the turn is kept
  until the transaction commits, which is serial apply as described under
  [Staging](#staging). Off releases it when apply is done. Page locks taken
  in apply are held to commit either way, so a later plan waits in the lock
  manager for any page it shares with one that is still committing.

What differs from the proposal: each transaction applies its own plan on its
own thread, there are no per-file queues drained by whichever thread finds
them, there is no prefetch, and page locks are held to commit, not for one
operation.

Checked: `tests/ordered_apply.test` (new) passes on a single node on macOS;
`tests/tunables.test` expectations are updated; a kill -9 under load with
parallel apply recovered to whole transactions with verify clean; after every
benchmark phase the three nodes held identical tables and verify succeeded.
Not run: the wider test suite with `ordered_apply` on, any test on a real
disk, and the Jepsen suite.

#### Results

Same cluster and workloads as [How this was measured](#how-this-was-measured),
with `async_dist_commit` on throughout. "Serial" applies on one thread and
keeps the turn to commit. "Parallel" adds a thread per file. "Early" is
parallel with `ordered_apply_serial_commit` off.

| Ascending keys          | Page locks        | Serial | Parallel | Early  |
|-------------------------|------------------:|-------:|---------:|-------:|
| 100 rows, 1 client      | 6,726 rows/s      |  6,036 |    7,301 |  6,543 |
| 100 rows, 8 clients     | 8,132 rows/s      | 11,245 |   23,430 | 25,382 |
| 100 rows, 32 clients    | 6,732 rows/s      | 11,350 |   26,004 | 27,239 |
| 1 row, 1 client         | 560 txn/s         |    525 |      528 |    529 |
| 1 row, 32 clients       | 7,001 txn/s       |  2,975 |    3,070 |  3,511 |

| Random keys, 4.2M rows  | Page locks        | Serial | Parallel | Early  |
|-------------------------|------------------:|-------:|---------:|-------:|
| 100 rows, 1 client      | 6,280 rows/s      |  6,743 |    6,771 |  6,854 |
| 100 rows, 8 clients     | 7,075 rows/s      |  8,540 |   20,468 | 22,000 |
| 100 rows, 32 clients    | 6,610 rows/s      |  8,973 |   22,343 | 23,654 |
| 500 rows, 16 clients    | 8,179 rows/s      | 10,262 |   26,513 |      — |
| 1 row, 32 clients       | 8,408 txn/s       |  2,813 |    2,753 |  3,514 |
| 1 row, 64 clients       | 8,748 txn/s       |  2,876 |    2,754 |  3,435 |

The 500-row figures for page locks and serial come from the earlier runs in
`results/stage0-async-dist-commit.results` and
`results/stage1-serial-apply.results`.

- **Deadlocks.** None in any ordered-apply run. Under page locks the same
  runs had 930 to 1,251 for 1,920 transactions of 100 rows.
- **Large transactions.** Serial apply lands at the `maxwt 1` rate, as
  expected. A thread per file gives 3.4 to 4 times the page-lock rate at 8
  and 32 clients. Master CPU goes from about 1.5 cores to about 5.5.
- **Single-row transactions lose more than half.** The turn is busy about 90%
  of the time, so this is the serial section itself. With 32 clients it is
  about 300 µs per transaction: 180–210 µs applying four puts and about
  120 µs from the end of apply to the end of commit. The same four puts take
  120–134 µs with one client, and about 85 µs as part of a 100-row plan
  (8.4 ms for 400). Releasing the turn early removes the commit part and
  gains about 20%, but apply itself then takes 235 µs.
- **A single client is unchanged**, within run-to-run noise.

The estimate under [Staging](#staging) of 20,000 single-row transactions per
second for serial apply assumed 12 µs per put. Measured inside the turn, a
put costs 20 µs in a large plan and 45–60 µs in a single-row one.

#### One node, and what the turn costs

These are the first two items of the earlier resume list. The question was
whether replication limits single-row throughput in this scheme, since each
log record is sent to the replicants on the thread that writes it and that
thread holds the turn.

It does not. The same four variants on one standalone node, with no
replication of any kind (`results/stage1-single-node.results`):

| One node                         | Page locks   | Serial | Parallel | Early  |
|----------------------------------|-------------:|-------:|---------:|-------:|
| Ascending, 1 row, 1 client       | 1,062 txn/s  |    897 |      906 |    871 |
| Ascending, 1 row, 32 clients     | 7,207 txn/s  |  3,352 |    3,466 |  3,863 |
| Random, 1 row, 32 clients        | 8,745 txn/s  |  3,105 |    3,095 |  3,851 |
| Random, 1 row, 64 clients        | 9,574 txn/s  |  3,009 |    2,952 |  3,779 |
| Ascending, 100 rows, 32 clients  | 7,488 rows/s | 13,153 |   30,774 | 37,967 |
| Random, 100 rows, 32 clients     | 6,768 rows/s |  9,725 |   28,560 | 31,472 |

Single-row throughput with 32 clients is 4–13% above the three-node
figures in every variant, and the 100-row figures keep their shape. On one
node the SQL runs on the same process, so its CPU is not comparable with the
master's above. Moving the log send off the applying thread would not help.

A call-graph profile of the serial single-row run
(`results/stage1-single-row-profile.txt`) shows where the time goes:

- **The deadlock detector, for the turn.** 38% of the CPU under
  `bdb_tran_plan_apply` is the lock manager call that takes the turn, and 32%
  is the detector. Automatic detection runs the detector on every lock wait,
  and nearly every transaction waits for the turn. Each run walks every
  locker with the lock region held. A wait also marks the region as needing
  a run, so the transaction that has the turn runs the detector again from
  its own page-lock calls and at commit.
- **The puts themselves, slower than in a large plan.** The rest is the four
  puts, all of it on CPU, with no single hot spot: the search from the root
  is 57–65% of a put, buffer pool gets 17%, compression 14%, logging 8%. A
  put costs 38–50 µs here, 26 µs in a single-row plan with one client, and
  18–25 µs in a 100-row plan. This fits the turn moving to a thread that has
  just been woken, on another core, for four puts.

`ordered_apply_turn_detect` off makes a wait for the turn skip the detector
and not ask for a run (`DB_LOCK_NODETECT`). On one node
(`results/stage1-turn-detect.results`):

| 1 row per transaction, one node | Page locks  | Serial | Serial, no detect | Early | Early, no detect |
|---------------------------------|------------:|-------:|------------------:|------:|-----------------:|
| Random, 32 clients              | 9,527 txn/s |  3,130 |             3,639 | 3,910 |            4,571 |
| Random, 64 clients              | 9,931 txn/s |  3,113 |             3,809 | 3,744 |            4,681 |
| Ascending, 32 clients           |           — |  3,184 |             3,751 | 3,620 |            4,614 |

That is 16–27% more, and 100-row transactions do not change. It is not safe
to leave off: with automatic detection there is no periodic detector thread,
so a deadlock whose last edge is a wait for the turn would be found only when
some other lock wait happened to run the detector.

With the detector out of the way a single-row transaction still holds the
turn for about 155 µs when it releases at the end of apply, and the next one
starts about 60 µs after that, which is the time to wake it. Page locks
reach 9,500 transactions per second on 13 cores, so a serial section has to
come in under 105 µs to match that, hand-off included. Handing the turn from
thread to thread cannot do it. One thread that keeps the turn and applies
queued plans pays no hand-off and keeps its caches; at the 18–25 µs per put
measured in large plans that is 70–100 µs for a row with three keys, or
10,000 to 14,000 single-row transactions per second on one core. A thread
per file, as the proposal describes, is what would go well past page locks.

### Commit units

`ordered_apply_unit` on. This is the second direction of the earlier resume
list, taken as far as it goes without ordering commits per page.

What it does:

- **Prepare** is unchanged. At the point where a transaction used to take
  the turn it does nothing, and its plan moves to its parent transaction
  when the child commits.
- **A unit.** Where the block processor would write the request's blkseq
  record, it hands the plan and the record to one thread and sleeps. That
  thread takes every plan that is waiting, up to
  `ordered_apply_unit_max_ops` operations, begins one Berkeley transaction,
  writes each member's blkseq record and applies each member's plan under
  it, and commits once. A member whose blkseq key is already there is a
  replay: none of its plan is applied and it gets the stored reply.
- **The members' own commits** write nothing. Each takes the unit's commit
  LSN as its own and waits for acks on it as before (stage 0).
- **A file per thread.** A unit with `ordered_apply_parallel_min_ops` or more
  operations is split by btree file and applied as large plans were, under
  child transactions of the unit.
- **A transaction on its own applies its own plan.** With
  `ordered_apply_unit_inline` on, a request whose plan is the only one in
  existence, counted from the start of prepare, does not wake the unit
  thread: it applies the plan itself, under its own Berkeley transaction
  with its own blkseq record, and any that arrive meanwhile queue until it
  is done.
- **A wait the deadlock detector can follow.** A member sleeps on a
  condition variable while it waits for its unit, and it holds table locks.
  A unit could wait for a page held by a transaction that waits for one of
  those locks, and nothing would see the cycle. Every unit therefore holds a
  write lock on one name, and a member that has waited
  `ordered_apply_unit_detect_ms` asks for that name in read mode, in the
  lock manager. A member chosen as the victim cannot give up its locks, so
  it aborts the transactions waiting for them and waits again. That last
  step has never run: no SQL was found that produces the cycle, since a
  transaction mixing an insert with DDL on another table is refused. Forcing
  every member to wait in the lock manager was tried, and works.
- **Deadlocks.** All page locks in a unit belong to one transaction, so its
  members cannot wait for each other. If the unit is the victim of a
  deadlock with a transaction outside it, it aborts, which also takes its
  blkseq records back out, and the same members are applied again.

To the log, to replicants and to recovery a unit is one ordinary transaction,
so none of the three properties under
[What page locks are needed for](#what-page-locks-are-needed-for) is in
question. What is given up is that members share fate: a unit commits or
aborts as a whole.

What differs from the proposal: there is one queue, not one per file; the
applier is a dedicated thread unless a transaction is alone; members are joined into one transaction, where
the proposal has child transactions under one commit record; and page locks
are held to the unit's commit.

Checked: `tests/ordered_apply.test` has a commit-unit section and passes on a
single node on macOS; two kill -9 rounds under commit-unit load recovered to
whole transactions with verify clean; 64 writers into a table with twelve
indexes and a blob; after each benchmark phase the nodes held identical
tables and verify succeeded. Not checked: the replay path, which no test
reaches, and the wider test suite.

A bug found on the way: Berkeley allows a thread ten open transactions, and
applying a file per thread begins one child for each file. Single-row inserts
from many clients touch eight data stripes and three indexes, and the master
aborted. Plans applied in turn had the same exposure for a table with many
indexes. The limit is `TXN_TD_MAX` in `berkdb/txn/txn.c`, which is Comdb2's
and not Berkeley's: it sizes a per-thread list used to check that a request
leaves no transaction open. It is now 64. The results below were measured
with a stopgap of at most seven groups of files; with one child for each file
again the numbers do not change by more than they vary between runs.

#### Results

`results/stage1-commit-units.results`. "Early" is parallel apply with the
turn released after apply, the best earlier variant. "Unit" applies a file
per thread from 64 operations; "one thread" never does.

| One node                         | Page locks   | Early  | Unit   | Unit, one thread |
|----------------------------------|-------------:|-------:|-------:|-----------------:|
| Ascending, 1 row, 1 client       | 1,026 txn/s  |    995 |    870 |              904 |
| Ascending, 1 row, 32 clients     | 8,932 txn/s  |  3,932 | 10,353 |           10,399 |
| Random, 1 row, 32 clients        | 9,399 txn/s  |  3,966 | 10,415 |           10,840 |
| Random, 1 row, 64 clients        | 9,449 txn/s  |  3,815 | 10,425 |           10,800 |
| Ascending, 100 rows, 1 client    | 9,265 rows/s | 13,077 | 11,410 |           10,305 |
| Ascending, 100 rows, 32 clients  | 7,293 rows/s | 34,742 | 49,059 |           32,734 |
| Random, 100 rows, 32 clients     | 6,539 rows/s | 33,031 | 35,137 |           24,728 |

| Three nodes                      | Page locks   | Early  | Unit   | Unit, one thread |
|----------------------------------|-------------:|-------:|-------:|-----------------:|
| Ascending, 1 row, 1 client       | 563 txn/s    |    479 |    490 |              520 |
| Ascending, 1 row, 32 clients     | 7,425 txn/s  |  3,416 |  9,101 |            9,487 |
| Random, 1 row, 32 clients        | 8,707 txn/s  |  3,553 |  8,322 |            8,389 |
| Random, 1 row, 64 clients        | 9,554 txn/s  |  3,623 |  9,863 |            9,887 |
| Ascending, 100 rows, 1 client    | 6,280 rows/s |  7,478 |  6,864 |            6,347 |
| Ascending, 100 rows, 32 clients  | 6,387 rows/s | 27,468 | 25,648 |           18,366 |
| Random, 100 rows, 32 clients     | 6,328 rows/s | 28,781 | 24,228 |           15,014 |

- **Single-row transactions are back at the page-lock rate or above it.** On
  one node 10,400 to 10,800 a second against 8,900 to 9,400. On three nodes
  between 4% below and 28% above, with the master using 5.6 to 6.8 cores
  where page locks use 7.8 to 10.7. The applier spends 63 to 77 µs on a
  transaction on one node and 83 to 94 µs on three, against 190 to 240 µs
  when the turn is handed from thread to thread.
- **The applier is not saturated.** With 32 clients on one node it is busy
  73 to 85% of the time and a unit holds 1.6 to 2.4 transactions. On three
  nodes a unit holds 5 to 15. The single-row rate is now set by what is
  outside the applier.
- **A lone client loses 9 to 13% on one node** against early release on
  ascending keys, for one row or a hundred, when every plan goes to the unit
  thread: it waits for another thread to be woken and then to wake it. On
  three nodes there is no clear difference. These tables were measured that
  way. With `ordered_apply_unit_inline` the loss is gone: 945 single-row
  transactions a second for one client on one node against 858 without it
  and 952 for early release, and no change that can be told from noise with
  32 clients (`results/stage1-commit-units-lone.results`). Page locks do
  1,027, so recording a plan and applying it afterwards costs a lone client
  4 to 8% on one node, and nothing measurable on three.
- **Large transactions.** On one node units are ahead of early release, by
  41% on ascending keys and 6% on random ones. On three nodes they are 7 to
  16% behind it.
  Not looked into; master and replicant CPU are both lower in the unit runs.
- **Applying a file per thread** matters for 100-row transactions and not for
  single rows, where a unit rarely reaches 64 operations.
- **Deadlocks and unit retries:** none in any run.

### Unique keys

`ordered_apply_unique` on. This is the first part of names and stamps, and
it turned out to need neither.

For an insert, the btree can stay the judge of uniqueness, as it is under
page locks:

- **When the add is recorded**, the plan is searched for an earlier add of
  the same key, and the index is read for it. The read is under a
  transaction of its own that does not wait for locks and is over at once.
  A key that is found gives the usual duplicate error at the usual point in
  the request. A page that is locked gives nothing, and the add is
  recorded.
- **When the plan is applied**, the put still refuses a duplicate. That
  happens when the key was behind a lock at the time of the read, or was
  added afterwards, by another transaction or by an earlier member of the
  same unit. The request runs again without a plan, under page locks, where
  the put reports the duplicate. In a unit only that member goes back; the
  unit is aborted and the others are applied again.

So a plan can be wrong about a unique key only in the direction of running
again, and no stamp is needed. A stamp would save the aborted unit; stamps
are still needed for updates and deletes, where the btree cannot arbitrate.

The read must not wait. A first version waited, and a request could then
wait for a page its own parent transaction had written, under a different
lock owner, which nothing detects.

Not planned, and run under page locks as before: `ON CONFLICT` inserts;
tables with foreign keys; any insert made with constraint checking on, which
includes every insert while a schema change runs, because those add their
keys at the end of the request from the row read back from the data file;
and requests that contain a bpfunc, which can act outside the transaction
and must not run twice.

Checked: `tests/ordered_apply.test` has sections for unique keys (sixteen
clients inserting the same 300 keys leave 300 rows and 4,500 duplicate
errors; multi-row transactions are all or nothing; NULLs do not collide)
and for inserts during schema changes, and passes on macOS and through the
harness on Linux.

#### Results

`results/stage1-unique-keys.results`: the benchmark table with its index on
`a` made unique. The benchmark never repeats a key, so this measures the
cost of the checks, not of duplicates.

| Table with a unique key          | Nodes | Page locks   | Unit   |
|----------------------------------|------:|-------------:|-------:|
| Ascending, 1 row, 1 client       |     1 | 993 txn/s    |    876 |
| Ascending, 1 row, 32 clients     |     1 | 7,303 txn/s  | 10,640 |
| Random, 1 row, 32 clients        |     1 | 9,741 txn/s  | 10,844 |
| Ascending, 100 rows, 32 clients  |     1 | 7,541 rows/s | 39,066 |
| Random, 100 rows, 32 clients     |     1 | 6,697 rows/s | 33,268 |
| Ascending, 1 row, 32 clients     |     3 | 7,027 txn/s  |  8,334 |
| Random, 1 row, 32 clients        |     3 | 8,714 txn/s  |  7,759 |
| Random, 1 row, 64 clients        |     3 | 8,704 txn/s  |  9,452 |
| Ascending, 100 rows, 32 clients  |     3 | 6,710 rows/s | 19,254 |
| Random, 100 rows, 32 clients     |     3 | 6,224 rows/s | 15,861 |

No request ran without a plan, and there were no deadlocks or unit retries.
The shape is that of the tables without a unique key. The three-node
100-row figures are lower than those above (25,648 and 24,228), and this
run cannot say whether that is the unique key, the other changes since, or
variation between runs.

### Updates and deletes

`ordered_apply_update` on. This is the rest of stage 1.1 for tables with no
foreign keys. It needed reads laid over the plan. It did not need stamps.

**The data file is the judge.** A genid names one version of one row. An
update gives the row a new genid, and no genid is used twice. A plan updates
or deletes a row by genid, and when it is applied the data file has that
genid or it does not. If it has, the row is the one the plan read, and the
keys the plan worked out from it are the keys in the indexes. If it has not,
the request runs again without a plan and gets its answer under page locks:
a verify error, as a rule. So an update or a delete is judged at apply as a
unique key is, by the btree, and a plan can still be wrong only in the
direction of running again. A stamp would save the aborted unit and nothing
else. Foreign keys are different: a check that a parent key exists, or that
no child row does, is a read of something the plan does not write, and the
btree cannot judge it at apply.

What it does:

- **Reads over the plan.** A row is read by genid through
  `bdb_tran_plan_fetch_genid()`, which stands in for the ordinary fetch
  while a plan is recorded. The plan keeps a table of the rows it has met:
  one it added or updated is returned from the plan, one it deleted or
  updated away is not found, and otherwise the data file is read. That read
  is under a transaction of its own that does not wait and is over at once.
  A page that is locked is tried six times over about 1.5 ms, and then the
  plan is given up.
- **What is recorded.** A delete of a row, of each of its blobs and of each
  key. An update in place as one operation with the new genid already
  chosen, since the request builds keys from it. An update that moves the
  row (the updateid has run out, or the row grows and page-order scans are
  on) as a delete and an add, which can be in two stripes. A key whose value
  does not change as an update of its genid. A blob update as it was called,
  to be decided at apply, because what it does depends on whether the row
  has that blob.
- **Unique keys.** The plan remembers the last thing it did with each unique
  key. A key it has deleted can be added again without looking in the
  index, which is how a row gives its key to another in one transaction.
- **Apply.** The same `ll_*` routines. A row is updated or deleted only if
  its updateid is still the one in the plan's genid. `DB_NOTFOUND` from any
  operation is treated as `DB_KEYEXIST` already was: in a unit, that member
  goes back and the rest are applied again without it.
- **One thread.** A plan with an update or a delete in it is not applied a
  file per thread unless `ordered_apply_parallel_updates` is on; see the
  detector finding below.
- **Inserts with constraint checking on** are planned now. Every insert in
  a request that also updates or deletes runs that way, and adds its keys
  at the end of the request from the row read back by genid, which the plan
  now answers.

What a plan must not be trusted with, each found by a test:

- **An error it finds for itself**, once it has read rows. Nothing keeps a
  row as the plan read it, so two reads can disagree. Two requests raced to
  move a unique key from 179 to 180; the loser read the row, the winner
  committed, and the loser's look for key 180 found the winner's. It
  reported a duplicate key. Under page locks it would have found the row
  changed, which the client library retries without the application seeing
  it. A request that has read rows and fails with anything but a verify
  error now runs again without a plan. A verify error can be believed: a
  genid that is gone does not come back.
- **An error after a read that went round the plan.** A read under the
  request's transaction that is not by genid (a trigger's, a blob for an
  index) takes a page lock and does not see the plan. A request that holds
  a page lock when it fails runs again without a plan.
- **`SELECTV`.** The rows it names must stay as they are until commit,
  which the page lock of the check sees to. A request with a `SELECTV`
  record gives its plan up.

Not planned: tables with a foreign key in either direction, tables with an
index on a blob, cascades, tagged requests, anything during a schema change
of the table, and `replicate_local`.

Three things found here were wrong before this step and are fixed:

1. **The commit lock was left held.** The block processor takes a lock,
   shared, from the point where a request can no longer fail to the end of
   its commit; a serializable or `SELECTV` request takes it exclusively. A
   request whose plan could not be applied went to the backout path with it
   and never gave it up. The next exclusive request waited for ever.
2. **A wait for that lock the detector cannot see.** It is a pthread
   rwlock, which was safe while a request took it with all its page locks in
   hand. A request with a plan takes its page locks afterwards, through its
   unit, so it can wait for a page whose holder waits for the lock. With
   ordered apply on, a request now takes a lock of the same name and mode in
   the lock manager first (`bdb_tran_commit_gate()`), under its top-level
   transaction.
3. **A plan given up without an error reaching the block processor** was
   applied anyway. `bdb_tran_plan_apply()` now refuses it.

And one in the deadlock detector, found later (run 21). With a unit applied
a file per thread, `recovdlock_emptyblob` stalled: a reader, or a
transaction under page locks, and two threads of the unit stood in a cycle
that nothing broke. The lock manager assumes one thread works for a
transaction family, and that was wrong in two more places:

- **The detector woke one thread of its victim.** Aborting a victim means
  aborting one waiting lock, which is the whole transaction when it has one
  thread. A unit cannot abort until all its threads have come back; the
  others stayed in their waits, with the pages of the cycle still held.
  `__dd_abort_family()` now aborts every waiting lock of the victim's
  family.
- **A grant can close a cycle, and the detector ran only when a wait
  began.** Its graph has an edge from a waiter to each holder and none to a
  waiter ahead of it in the queue. When that waiter is promoted to holder
  the edge appears, with no new wait to run the detector. With one thread
  to a family that is harmless: the promoted thread is running, and its
  family is in no cycle until it waits again. With several, a sibling may
  be waiting already. A lock granted after a wait now runs the detector if
  another of its family is waiting. It costs a transaction with one thread
  a comparison after each wait and nothing else.

One new waiter was enough to free a stalled database, which is how the
first was found; the lock tables and the detector's trace are in
`results/run21-lock-table.txt`.

With both fixed, a unit applied a file per thread no longer stalls, but it
deadlocks far more than one thread does. One thread takes a data page and
then the blob page behind it, as a reader does. Several threads take them in
no order, and under the master's policy a transaction that only reads is
never the victim, so the unit loses every time. So plans that update or
delete are still applied on one thread unless
`ordered_apply_parallel_updates` is on, and a unit that has been a deadlock
victim `ordered_apply_parallel_retries` times (1) while applied a file per
thread is applied on one thread when it runs again; that covers plans of
adds as well.

Measured on `recovdlock_emptyblob`, whose five writers and three readers
are slowed by a debug sleep, so every mode commits about 90 transactions in
its 120 s and the difference is in what is thrown away (run 21,
`results/run21-recovdlock.txt`): page locks have 2,400 to 2,500 deadlocks;
units on one thread 3,000, with about 960 unit retries for 80 units; a file
per thread with the limit at 1 or 2, 4,300 to 4,800 and 1,000 to 1,350; at
4, 4,800 to 5,400 and 1,300 to 1,450; and with no fall-back 22,300 and
8,700. So the limit is 1. On the benchmark's 100-row transactions, where
nothing reads, a file per thread gives 11% more on updates of a column with
no index and 15% more on deletes, and 9% less on updates of an indexed
column (run 23).

Checked: `tests/ordered_apply.test` has a section for this. The same
statements (updates of plain, indexed and unique columns, a shift of a unique
key along a range, rows that grow, deletes, a row that takes the key of one
deleted in the same transaction, a transaction that fails and rolls back,
blobs set, changed and cleared) are run on one table under page locks and on
others with plans, in turn and in units, and must tell the client the same
and leave the same table. Sixteen clients update the same 20 rows and
overlapping ranges, and every update reported is in the table once; eight
delete the same 400 rows, and each is deleted once; sixteen race to move a
unique key and none is told of a duplicate; a `SELECTV` transaction after
many failed applies finishes. It passes on macOS. Three kill -9 rounds under
planned updates, deletes and blob changes, in units, a file per thread and in
turn, recovered with data and indexes agreeing and verify clean.

#### Results

`results/stage1-updates.results`, run 18: three nodes, the benchmark table
with a million rows, 32 clients unless said. `upd` changes a column with no
index, `updk` an indexed one, `updhot` picks from the same 200 rows. A
transaction of 100 rows is 100 statements.

| Three nodes                   | Page locks    | Unit   |
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

- **Single rows that nobody else wants are level**, between 13% behind and
  3% ahead, with the master on 6.2 to 7.7 cores where page locks use 8.1 to
  9.0. Nothing fell back, and 8 plans of 256,000 were stale.
- **The same rows from many clients are 1.9 times as fast.** 1,111 plans of
  32,000 were stale at apply and 199 gave up on a locked read.
- **Large updates gain 47% and 75%, and lose most of what they could
  gain.** A third to two fifths of the transactions gave their plan up
  because a row they read stayed behind a page lock, and 324 and 437 units
  were aborted as deadlock victims. A unit holds its page locks until it
  commits, and a 100-row plan reads 100 rows meanwhile. Short page locks in
  apply, as the proposal has them, are what would remove this.
- **Large deletes of ascending keys lose 21%**, for the same reason, on a
  workload where page locks have little to wait for.

### The test suite

The default suite, 789 tests, was run on one node with `ordered_apply`,
`ordered_apply_unit` and `ordered_apply_unique` on for every test database
(`results/suite-run13.txt`, and run 13 in `BENCHMARK-LOG.md`). 578 passed.
Of the 204 that failed, 180 fail on `main` in the same setup, mostly for
want of a cluster. The other 19 led to six bugs, all fixed:

1. **Deadlocks the detector did not see.** The lock manager marked a
   transaction family as waiting with one flag on its master locker, set by
   a wait and cleared by the family's next grant. That holds while one
   thread works for a transaction. A unit applied a file per thread has
   several threads in one family, so a grant to one cleared the flag while
   another waited, and the detector left that wait out. Databases stalled
   until their watchdog aborted them. The flag is now a count of waiting
   lockers (`berkdb/lock/lock.c`, `lock_deadlock.c`, `dbinc/lock.h`). Plans
   applied a file per thread had the same exposure before units existed.
2. **The duplicate-key read waiting on its own transaction**, above.
3. **Inserts during a schema change** failing with "verify key constraint:
   record not found", above.
4. **Row limits counted twice** when a request gave up its plan and ran
   again.
5. **The log-bytes limit not applied** to a plan, which writes no log while
   the request runs. The limit now counts an estimate of what the plan will
   write.
6. **`put counter ... increment` applied twice**: a bpfunc ran, the request
   gave up its plan, and it ran again.

Tests that still fail with ordered apply on:

- `reqlog_logbyte`, `fingerprints`: the log bytes and write counts reported
  for a request. A request whose plan goes into a unit writes nothing
  itself and reports none. Not fixed.
- Three `noreorder` tests that assert a minimum number of deadlocks.
- `upsert`, `manual_partition_lgcy`: rows in a different order from a
  select with no `ORDER BY`. Cause not established.

The whole suite was then run again from the commit with the fixes
(`results/suite-run15.txt`, run 15): 584 passed and 198 failed. Of the 197
failures other than the feature's own test, 186 fail on `main` too. Of the
11 that pass on `main`, five pass with ordered apply when run at lower load;
they had run into their time limits with 40 tests in parallel. The other six
are the tests listed above, less one deadlock-count test that failed on
`main` as well. None of the hangs came back. Nothing clustered has been
run.

With updates and deletes planned the suite was run twice more. The first
run (run 16) is what found the five things under
[Updates and deletes](#updates-and-deletes). The second, on the tree with
them fixed and 20 tests at a time (`results/suite-run19.txt`, run 19): 571
passed and 212 failed. 22 of the failures pass on `main`; run again with
ordered apply, 16 of those pass and six fail. The six are `fingerprints`
and `manual_partition_lgcy`, as before, and four tests that want deadlocks,
two of them new to the list because their updates are now planned
(`reorder_updates_noreorder_generated`, `index_deadlock_loop_print`).
`upsert` passed. Two tests that failed repeatedly on the same code while it
was being fixed passed in this run. Run 20 ran each five times with ordered
apply, without it and on `main`, and they fail as often in all three
(`results/run20-two-tests.txt`): `commit_lsn_map_delete_generated` because
the test machine's `comm` (uutils coreutils 0.2.2) reports sorted input of
more than 8,192 bytes from a pipe as unsorted, and
`selectv_rcode_disable_svonly_nop_generated` with "transaction too big" on
`main` as well. Neither is ordered apply's.

### Tests for iterating

While this work is going on, only the tests written for it are run. The
whole suite takes two hours a pass on one node and several passes to tell
its own noise from a real failure, and most of it is about other things. It
is to be run, a few times, once the implementation is done; run 24, the
suite on the detector changes, was stopped after 42 tests for that reason.

The tests are `tests/oa_*.test`, with `tests/tools/oa_common.sh`, and
`docs/adr/0003-bench/adr-tests.sh <tree>` runs them: about seven minutes on
one node. Each runs its workload three times in one database, under page
locks, with commit units, and with commit units applied a file per thread,
and checks something that must hold whichever it is:

- `oa_counters`: clients add one to counters, alone and five to a
  transaction. The counters must add up to the updates the clients were told
  committed: a lost update is one fewer, a request applied twice one more.
- `oa_bank`: transfers between accounts with a ledger row each. The money
  adds up, the ledger has a row for each transfer that committed and no
  other, and readers under snapshot isolation see the same total every time.
- `oa_unique`: every client tries to insert every key, then to delete it.
  One wins each key, the rest are told it is a duplicate; one deletes each.
- `oa_deadlock`: transactions that update four rows in orders of their own,
  with an index entry and a blob each, against readers by data file and by
  index. Counters as above.
- `oa_queue`: producers add at the high end of a key while consumers race to
  delete the low end. Each row is deleted once and nothing produced is lost.
- `oa_throughput`: single-row and 100-row inserts, updates, hot rows,
  20-row transactions and deletes, timed. It prints transactions a second by
  mode and against page locks, and fails only if something fails.

All of them end by verifying their tables and comparing them across nodes,
and all are watched: a database that commits nothing for 60 s while clients
are running fails its test, with the lock table kept. That is the check
that would have caught the stall of run 21 at once. The mode applied a file
per thread splits a unit from four operations up in the five checking
tests, where the default is 64, so that small transactions run on several
threads; `oa_throughput` leaves the default.

What they do not cover: schema changes, foreign keys and the other things a
plan cannot hold, which `tests/ordered_apply.test` covers by statement; any
failure of a node; and everything the suite found that was not about
transactions under contention.

### Where to resume

State at the end of the second session of 2026-11-14:

- **Code:** branch `rowlocks-ideas`, all committed. The commit on top of
  `80d2ea75d` (updates and deletes) has the two deadlock-detector fixes
  (`berkdb/lock/lock.c`, `lock_deadlock.c`), the tunables
  `ordered_apply_parallel_updates` and `ordered_apply_parallel_retries`
  with the counter `ordered_apply_unit_serial`, and the benchmark scripts
  of runs 20 to 23. The feature's own test passed on `80d2ea75d` (run 20).
  The commit after that adds the tests under
  [Tests for iterating](#tests-for-iterating), which pass (run 25). Nothing
  is pushed.
- **To measure, use these settings:** `ordered_apply 1`,
  `ordered_apply_unit 1`, with `disable_tagged_api_writes` on as
  `ordered_apply` now requires. Everything else is at its default. The turn
  variants (`serial`, `par`, `par-early`) are kept only for comparison.
- **Every run is in `docs/adr/0003-bench/BENCHMARK-LOG.md`**, runs 1 to 25,
  with what changed before each. Add an entry for each new run when it
  starts.
- **A second branch, `rowlocks-adaptive`**, has the experiment on the
  existing rowlocks code. It is finished and nothing follows from it.
- **On xeon** (all under `/tmp`, a RAM disk that a reboot empties): the
  benchmark cluster in `/tmp/rlbench` (containers `rlbench-*`, idle, built
  from this tree), builds for the test suite in `/tmp/rltest2` and
  `/tmp/rltest3` (this tree) and `/tmp/rltest-main` (`main`), and a pmux of
  our own on port 19500. All can be removed; `teardown-host.sh` removes the
  benchmark cluster, and `suite-with-ordered-apply.sh` says how to set the
  suite up again.
- **On the Mac** the disk was 97% full, which makes comdb2 refuse schema
  changes on a scratch database (`sc_headroom`, 10% by default; set it to 1
  on the scratch database).

Things learned the hard way, which the next step must respect:

- **A transaction recording a plan must not wait for a lock.** The lock may
  be held by a unit that waits for this transaction, or by this
  transaction's own parent under another lock owner. Reads made while
  planning use a transaction that does not wait, and treat a locked page as
  "not known".
- **A request can run twice**, once with a plan and again without. Anything
  it does outside its transaction must not happen in the first attempt:
  bpfuncs, counters, per-request limits and statistics.
- **A plan may be wrong only in the direction of running again.** That
  covers what it does (the btree judges it at apply) and now also what it
  reports: an error found while planning is believed only if nothing the
  plan read could have changed under it. Any new kind of read needs the
  same question asked.
- **Every wait has to be one the deadlock detector can follow**, or has to
  be made by something that holds no lock. The commit lock was neither.
  Look for other pthread locks taken between a request's first page lock
  and its commit.
- **Children of one transaction on several threads** are new to this code
  base, and the lock manager assumed one thread for each transaction family
  in three places found so far: the waiting flag, the detector waking one
  waiter of its victim, and the detector running only when a wait begins.
  The deadlock detector is not run on a timer, so a cycle it does not see
  when the last wait begins stands until some unrelated wait comes along.
  When a database stalls, make one new reader wait and see if it frees.
- **The test suite finds what the benchmarks do not**, and many of its
  tests depend on timing: run it 20 at a time, and run a test alone before
  believing that it fails.

1. **Run the test suite, when the implementation is done.** The detector
   changes are in the lock manager, under every transaction, and the suite
   has not run on them: see [Tests for iterating](#tests-for-iterating).
   Until then, `adr-tests.sh` after each change.
2. **Locked reads.** A third of 100-row update transactions give their plan
   up because a page stayed locked by a unit. Either units stop holding page
   locks to their commit, which is the proposal's short page locks and needs
   commits ordered per page, or a read waits for the unit in a way that
   cannot deadlock: a planning transaction holds nothing a unit can want
   except table locks.
3. **Foreign keys.** These are the reads the btree cannot judge at apply,
   and where stamps, or a check under the unit's transaction, come in. It
   needs a decision on how writers outside plan mode take part.
4. **`SELECTV` as a planned check**: an operation that asserts at apply,
   under the unit's transaction, that a genid is still there.
5. **Run the suite on a cluster.**
6. **Per-request accounting under units**: log bytes and write counts for
   request logging and fingerprints.
7. **Decide whether a lone transaction should skip the plan altogether.**
   It applies its own plan, which leaves it 4 to 12% behind page locks on
   one node for single-row inserts and level on three nodes.
8. **Retire the turn.** With units on, the turn is used only by plans that
   need logical log records. Once those go through units too, the turn, its
   lock, `ordered_apply_serial_commit` and `ordered_apply_turn_detect` can
   go.
9. **Three nodes, large transactions:** find why units are behind early
    release for inserts there, and whether a unique key costs anything.
10. **Open items in what exists:** no prefetch; the replay path through a
    unit and the victim path of a member's wait are untested; a failed member
    fails its whole unit; a request that cannot be planned pays one aborted
    attempt each time; a stale plan or a duplicate found at apply costs an
    aborted unit; units are not used when logical logging (`llog`) or a
    logical live schema change is on; "thd_req mismatch" seen once on macOS
    without ordered apply (run 16 of the log).
11. **Not yet run:** any test on a real disk, and the Jepsen suite.
12. **A unit starts every attempt with no deadlock priority.** The block
    processor carries a transaction's priority from one attempt to the
    next; a unit does not, so it loses to a page-lock transaction every
    time as well as to readers. Run 23 had 337 to 580 unit retries in 1,920
    100-row updates of an indexed column.
13. **A lone transaction that applies its own plan a file per thread** does
    not fall back to one thread after a deadlock: the request runs again
    and plans again. Only units count their deadlocks.
14. **The test machine's `comm`** fails `commit_lsn_map_delete_generated`
    about three times in five (run 20). Put `gnucomm` first in the path
    there, or expect it.

### Noticed, not addressed here

Things seen while measuring that were so before this work and are not part
of it:

- **A large transaction starves** (run 22). One statement that updates
  10,000 rows never commits while a single client keeps updating one of
  those rows: a row it read has changed when the master checks, and it is
  run again, up to `osql_verify_retry_max` (499) times. Alone it takes
  0.83 s. Page locks do the same; ordered apply neither causes nor cures
  it.

## Risks and open questions

- **Plan mode for the block processor is the bulk of the work.** Deferred key
  adds, the constraint temp tables, cascades, triggers and live schema-change
  mirroring all have to produce plan entries, and every read they do has to go
  through the plan and note a stamp. A missed read is a correctness bug,
  so the list of reads needs to be exhaustive and the rules need a model or
  proof as well as the Jepsen suite. A missed read also escapes the
  dependency rules, so a request could reply before what it read is durable
  (see [Failed requests wait too](#failed-requests-wait-too)).
- **Requests without a blkseq record.** A request without one fails with a
  plain abort, with no commit to order and no acknowledgement to wait for.
  OSQL requests always have one: for `BLOCK2_SOCK_SQL`, `BLOCK2_RECOM`,
  `BLOCK2_SNAPISOL` and `BLOCK2_SERIAL` the master builds the key itself from
  the session's uuid or rqid (`osql_bplog_set_blkseq`,
  `db/osqlblockproc.c:734`), whatever the replicant sent. Remote (fdb) writes
  are OSQL sessions whose uuid is the fdb transaction id
  (`db/fdb_bend_sql.c:352`). The exceptions are the `use_blkseq` switch
  turned off, and old tagged requests whose `BLOCK_SEQ` is all zeros, which
  plan mode does not handle. Plan mode already refuses a request without a
  blkseq record: it starts only when there is a parent transaction
  (`db/toblock.c:3294`), and the parent is begun only for a request with a
  blkseq record (`db/toblock.c:3282`). That check has to stay when plan mode
  grows.
- **A page missing from the buffer pool stalls an apply thread**, and with it
  every transaction queued on that file, or every write on the master under
  serial apply. Prefetching in prepare and from the queue is meant to make
  this rare. It cannot help a transaction larger than the buffer pool, whose
  apply runs at the disk's rate, and it has to be measured on real disks.
  Waits for a free buffer or for the log buffer to flush stall apply the same
  way.
- **A large transaction under serial apply stalls every write** for as long
  as it takes to apply: 12 to 50 seconds for a million rows at the estimated
  rates. Today it blocks only the transactions that contend with it. Serial
  per file confines the stall to the tables it touches. Within one table only
  slicing, in stage 2, removes it.
- **Single-thread apply rate per file** is estimated from today's cost per
  put. If a production workload pushes one index past it, partitioned apply
  moves from stage 2 to stage 1.
- **Master SQL depends on the snapshot cursor work** being complete enough
  that no reader on the master holds a page lock between calls.
- **Commit units make the replicant's unit of apply larger.** That is good for
  the receive thread and needs a size limit so one unit does not hold too many
  page locks on the replicant at once.
- **Memory.** A plan holds keys and row images until applied. Large plans need
  to spill to temp tables as the operation list does today.
- **Which stamp table.** The exact table with eviction has no false conflicts
  but does more work under the sequence mutex; the fixed array is simpler and
  its false-conflict rate depends on its size and on transaction size. Not
  decided. The array also changes what a dependency is. A slot holds the last
  writer of any name that hashes to it, not necessarily the writer of the
  name that was read: T5 writes A, T7 writes B into the same slot, and T9
  reads A and notes stamp 7. Waiting for T7 is not enough, because T7 may
  share nothing with T5 and commit before it. With the array, a stamp of N
  has to mean waiting until every transaction numbered N or less has
  committed, which is one watermark. With the exact table, the last writer
  of a name read that name first, so it depends on every earlier writer and
  waiting for it alone is enough.
- **One transaction's log records written by several threads.** A latch on
  the transaction or a child transaction per file; see
  [Transaction boundaries](#transaction-boundaries). Not decided.

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

## Related work

Each stage has close precedents. They mostly assume main-memory storage,
logical logging, or a write set known in advance. This proposal keeps
physical page logging, its undo and the replicant apply path as they are, so
the constraint it adds is the one in
[What page locks are needed for](#what-page-locks-are-needed-for): log
records for any one page stay in commit order.

**Settling the order before execution.** Calvin [1] puts transactions in a
global order before running them, so execution cannot deadlock. If a
transaction's read and write sets depend on data, Calvin first runs a
read-only reconnaissance pass (OLLP), then checks at execution that the sets
are unchanged and restarts the transaction if they are not. Prepare and
sequence follow the same pattern. The difference is that the whole plan,
payloads included, is built during prepare, not just the set of locks. Bohm
[2] also knows the write sets up front. It puts in placeholder versions so
that a read waits for a pending writer, which corresponds to a prepare
waiting until a stamp's transaction has applied to the file being read. PWV
[3] lets a write become visible before commit and tracks the commit
dependencies this creates. Aria [4] runs a batch, validates it and then
commits, without needing read or write sets in advance.

**Queues per partition drained in order.** QueCC [5] has planner threads
split each transaction into a queue per partition, and executor threads
drain those queues in a fixed order with no locking. The apply stage works
the same way, with files in place of partitions. Here a queue has no
dedicated executor: any thread that finds it unattended drains it.

**Threads that own data.** DORA [6] sends each part of a transaction to the
thread that owns that data, and PLP [7] extends this so that each partition's
btree pages are latch free. H-Store [8] runs each partition serially. In the
apply stage a file has one owner at a time, but the owner is whoever is
draining the queue, not a fixed thread.

**Validation against stamps.** Checking that every stamp read is unchanged
at the sequence mutex is backward-validation optimistic concurrency control
[9], in the form Silo [10] uses with a transaction id on each record. The
lossy stamp array is the versioned lock array of TL2 [11]. In TL2 names are
hashed into a fixed array of version numbers, a collision causes a needless
abort but never a missed conflict, and the false-abort rate is set by the
size of the array. The size estimate in
[A stamp table that fits in memory](#a-stamp-table-that-fits-in-memory) is
the same calculation.

**Commit ordered by dependency.** Releasing locks before commit and making
later transactions wait on earlier commits is early lock release [12], used
alongside group commit and flush pipelining in Aether [13]. Controlled lock
violation [14] records the commit dependency each early release creates and
enforces commit order with it, which is what the commit stage does with its
three kinds of dependency. Hekaton [15] tracks commit dependencies the same
way for speculative reads.

The papers above also show where these designs tend to break down: Calvin's
restart rate when the reconnaissance reads change, a long transaction holding
up queues in QueCC and in serial apply generally, and tuning the stamp array
for false aborts. These are covered under
[Risks and open questions](#risks-and-open-questions).

1. A. Thomson, T. Diamond, S.-C. Weng, K. Ren, P. Shao, D. J. Abadi.
   *Calvin: Fast Distributed Transactions for Partitioned Database Systems.*
   SIGMOD 2012.
2. J. M. Faleiro, D. J. Abadi. *Rethinking Serializable Multiversion
   Concurrency Control.* VLDB 2015.
3. J. M. Faleiro, D. J. Abadi, J. M. Hellerstein. *High Performance
   Transactions via Early Write Visibility.* VLDB 2017.
4. Y. Lu, X. Yu, L. Cao, S. Madden. *Aria: A Fast and Practical
   Deterministic OLTP Database.* VLDB 2020.
5. T. Qadah, M. Sadoghi. *QueCC: A Queue-oriented, Control-free Concurrency
   Architecture.* Middleware 2018.
6. I. Pandis, R. Johnson, N. Hardavellas, A. Ailamaki. *Data-Oriented
   Transaction Execution.* VLDB 2010.
7. I. Pandis, P. Tözün, R. Johnson, A. Ailamaki. *PLP: Page Latch-free
   Shared-everything OLTP.* VLDB 2011.
8. R. Kallman et al. *H-Store: a High-Performance, Distributed Main Memory
   Transaction Processing System.* VLDB 2008.
9. H. T. Kung, J. T. Robinson. *On Optimistic Methods for Concurrency
   Control.* ACM TODS 6(2), 1981.
10. S. Tu, W. Zheng, E. Kohler, B. Liskov, S. Madden. *Speedy Transactions
    in Multicore In-Memory Databases.* SOSP 2013.
11. D. Dice, O. Shalev, N. Shavit. *Transactional Locking II.* DISC 2006.
12. D. J. DeWitt, R. H. Katz, F. Olken, L. D. Shapiro, M. R. Stonebraker,
    D. Wood. *Implementation Techniques for Main Memory Database Systems.*
    SIGMOD 1984.
13. R. Johnson, I. Pandis, R. Stoica, M. Athanassoulis, A. Ailamaki.
    *Aether: A Scalable Approach to Logging.* VLDB 2010.
14. G. Graefe, M. Lillibridge, H. Kuno, J. Tucek, A. Veitch. *Controlled
    Lock Violation.* SIGMOD 2013.
15. P.-Å. Larson, S. Blanas, C. Diaconu, C. Freedman, J. M. Patel,
    M. Zwilling. *High-Performance Concurrency Control Mechanisms for
    Main-Memory Databases.* VLDB 2011.

## Reproducing the measurements

The scripts are in `docs/adr/0003-bench/`. `BENCHMARK-LOG.md` there lists every
run in order, with what changed in the code before it and which results file
it produced. They need a Linux host with Docker
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
- `ordered-apply-variants.sh` runs the comparison under
  [Implementation status](#implementation-status); its output and the two
  earlier runs are in `results/stage*.results`. Figures for one variant move
  by 5 to 10% from run to run. `setup-host.sh` copies it to
  `/tmp/rlbench` with the other scripts.
- `VARIANTS` in the environment picks what `ordered-apply-variants.sh` runs;
  the commit-unit results are `VARIANTS="pg par-early unit unit-1t"`, on one
  node and on three. The script stops if the master stops answering.
- `UNIQUE=1` in the environment makes `newcluster.sh` create the index on `a`
  as a unique one.
- `NODES=1` in the environment makes `newcluster.sh` and
  `ordered-apply-variants.sh` use one standalone node, with no `cluster` line
  in its lrl, in place of the three-node cluster. The other scripts work
  against either. `ordered-apply-turn.sh` is the one-node comparison of
  `ordered_apply_turn_detect` on and off.
- `perfunder.sh <label> <workload> <clients> <rows> <txns> <function>...`
  records call graphs on the master and prints, for each function named, the
  cores spent under it and what it called (`foldperf.py`). The profile above
  is `./perfunder.sh serial rand 32 1 2500 bdb_tran_plan_apply apply_op`
  after setting the serial variant by hand.

## References

- Deadlock backoff and writer penalty: `db/sltdbt.c:114`, `db/sltdbt.c:231`
- Ack wait and its wake-up: `bdb/rep.c:2721`, `bdb/rep.c:3243`,
  `db/glue.c:864`
- Rowlocks final commit: `bdb/tran.c:1943`
- Trickle loop: `bdb/threads.c:136`
- Page-LSN rules for redo and undo: `berkdb/db/db_rec.c:285`
- Snapshot unroll: `berkdb/mp/mp_versioned.c:372`
- Child transactions on replicants: `berkdb/rep/rep_record.c:6762`
- Collecting a transaction's records by previous LSN:
  `berkdb/rep/rep_record.c:6708`; the chain is kept in `txnid->last_lsn` by
  the generated log functions (`berkdb/dist/gen_rec.awk:515`)
- Transaction-class dispatch: `bdb/ll.c:246`
- ADR 0001, multiple freelists per btree, for uncontended throughput and an
  earlier profile of the commit path

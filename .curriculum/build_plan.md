# 01 · BUILD PLAN — what to build, what to ship, in what order

Build-focused. The reasoning behind these choices is settled; this doc is the map you execute. Goal in one line: **a from-scratch, crash-safe, concurrent OLTP database in pure C++20 (`-fno-exceptions -fno-rtti`, no deps except optionally liburing) that proves principal-architect-level command of WAL, ARIES recovery, buffer management, B+Tree latching, 2PL, and MVCC — the topics quackdb only grazed.**

chronos-db is the OLTP half of a two-project curriculum. The sibling, `../quackdb`, covers the OLAP half (columnar storage, compression, vectorized execution, cost-based optimization, distribution, SIMD). chronos deliberately does NOT re-cover those; the overlap map at the bottom states, for every shared topic, how chronos's treatment differs.

Calibration: 🔨 full build · 🧩 reimplement core / scoped build · 📖 conversant (no build).
**Every phase ships the one artifact named in bold. No phase is "done" without its harness green against an oracle the engine code cannot see.**

---

## TIER 0 — The Metal: durability primitives, memory, async I/O

**0.1 The durability contract 🔨** — O_DIRECT + fsync/fdatasync semantics, torn writes, sector vs page atomicity, hand-rolled CRC32 (slice-by-8), an atomic page-write primitive. **Ship: torn-write demonstrator + checksummed atomic-page-write primitive + a written durability-contract doc, proven by a SIGKILL torture loop with an independent raw-format verifier.** → `phase_specs/durability_contract.md`

**0.2 Arena & slab allocators 🔨** — page-aligned slab allocator for O_DIRECT-able frames, bump arenas with lifetime classes (frame / txn / query), intrusive freelists. **Ship: allocator suite, fuzz-clean under ASan/UBSan vs a malloc-backed shadow ledger, + cost-per-alloc microbenchmark vs malloc.** → `phase_specs/allocators.md`

**0.3 io_uring async I/O engine 🔨** — SQ/CQ management, registered buffers + fds, read/write/fsync SQEs, completion callbacks, short-I/O resubmission, queue-depth backpressure; 📖 SQPOLL / IOPOLL. **Ship: `chronos::io` engine passing a randomized differential fuzz vs synchronous pread/pwrite, + a syscalls-per-op measurement proving batching.** → `phase_specs/io_uring_engine.md`

## TIER I — Storage: pages, files, buffer pool

**1.1 Slotted pages & tuple layout 🔨** — 4 KB page (pageLSN slot reserved now), slot directory, intra-page compaction, stable RIDs; tuple format with null bitmap and varlena columns. **Ship: `Page`/`Tuple` codec + fuzz harness + golden hex page images checked in (format frozen).** → `phase_specs/slotted_pages.md`

**1.2 Disk manager & heap files 🔨** — page alloc/dealloc, file extension, free-space map (rebuildable cache, not truth), heap file scan/insert over the io engine, per-page CRC on every read. **Ship: `DiskManager` + `HeapFile` with close/reopen invariance + a crash-mid-allocate torture pass.** → `phase_specs/disk_manager_heap_files.md`

**1.3 Buffer pool 🔨** — frame/page tables, RAII pin guards, clock then LRU-K, scan resistance, write-back, miss-in-flight states, recLSN/flushedLSN seam left for Tier III. **Ship: thread-safe `BufferPool`, TSAN-clean, with trace-replay differential vs a standalone cache simulator + pin-leak detector + hit-rate table (clock vs LRU-K vs sequential flood).** → `phase_specs/buffer_pool.md`

## TIER II — Indexes & in-memory structures

**2.1 B+Tree, single-threaded 🔨** — disk-resident on buffer-pool pages: splits, redistribution/merge, point lookup, range iterator over the leaf chain, variable-length keys, duplicate policy. **Ship: `BPlusTree` passing 10M-op fuzz vs `std::map` + an independent structural-invariant walker.** → `phase_specs/btree.md`

**2.2 Concurrent B+Tree 🔨** — reader-writer page latches, pessimistic latch crabbing, then optimistic lock coupling with version counters + restart; epoch-based reclamation for deleted pages; 📖 B-link trees. **Ship: concurrent B+Tree, TSAN-clean N-thread fuzz, + throughput table crabbing vs OLC at 1 and 8 threads.** → `phase_specs/btree_concurrency.md`

**2.3 Extendible hash index 🔨** — directory with global depth, buckets with local depth, splits, directory doubling; 📖 linear hashing. **Ship: `ExtendibleHashIndex` passing differential fuzz + directory-invariant checker.** → `phase_specs/hash_index.md`

**2.4 Concurrent skiplist 🧩** — latched concurrent skiplist (insert/lookup/range, no persistence) sized for Tier IV's version store and lock table. **Ship: `SkipList` passing concurrent differential fuzz, TSAN-clean, + memory-overhead-per-entry number.** → `phase_specs/skiplist.md`

## TIER III — Durability: WAL, checkpoints, ARIES (the heart of chronos)

**3.1 Write-ahead log 🔨** — physiological records, LSN + per-txn prevLSN chains, pageLSN stamping, log buffer with wraparound, segment files with epochs, group commit, flushedLSN protocol wired into 1.3's eviction. **Ship: `WalManager` + standalone `waldump` (built from the format spec, zero shared code) + group-commit knob with a commits/sec-vs-latency curve + crash-tail torture pass.** → `phase_specs/wal.md`

**3.2 Fuzzy checkpoints 🔨** — begin/end checkpoint records carrying ATT + DPT (recLSNs), master record updated via the 0.1 atomic-write primitive, log truncation. **Ship: fuzzy checkpointing + truncation, + a measured recovery-time-vs-checkpoint-interval mini-study.** → `phase_specs/checkpoints.md`

**3.3 ARIES recovery 🔨** — analysis / redo (repeat history, pageLSN-gated) / undo (prevLSN chains), CLRs with undoNextLSN, crash-during-recovery re-entrancy; 📖 logical undo & nested top actions. **Ship: chronos survives a kill -9 torture loop — every acked-committed txn present, every uncommitted txn absent, across hundreds of randomized crash points — + a recovery-internals writeup.** → `phase_specs/aries_recovery.md`

## TIER IV — Concurrency control: transactions, locks, MVCC

**4.1 Transaction manager & 2PL lock manager 🔨** — txn lifecycle wired to WAL, lock table with per-object request queues, S/X + intention modes (IS/IX/SIX), strict 2PL, waits-for deadlock detection + victim selection, 🧩 lock escalation. **Ship: `LockManager` + `TransactionManager` passing an offline conflict-serializability check over thousands of randomized concurrent txns + scripted deadlock scenarios with deterministic victims.** → `phase_specs/txn_lock_manager.md`

**4.2 MVCC 🔨** — per-tuple version chains (2.4 skiplist keyed by RID), snapshots, SI visibility rules, first-committer-wins write-write conflict, vacuum off the oldest-active-snapshot horizon. **Ship: SI reads that never block writers (and vice versa), proven by a visibility-oracle harness — including a mandatory write-skew reproduction — + a vacuum that provably reclaims all dead versions.** → `phase_specs/mvcc.md`

**4.3 Isolation levels & serializable SI 🔨/🧩** — 🔨 RC / RR / SI selectable per txn · 🧩 SSI (SIREAD tracking, rw-antidependency detection, dangerous-structure aborts) · phantom handling via next-key tracking through 2.1 iterators · 📖 Ports & Grittner (PostgreSQL SSI). **Ship: an anomaly battery — lost update, write skew, phantom, read-only anomaly — with a per-level required-outcome matrix (anomalies must OCCUR where permitted and be PREVENTED where forbidden), + an offline DSG cycle checker for SSI soaks.** → `phase_specs/isolation_ssi.md`

## TIER V — SQL surface: catalog, frontend, execution

**5.1 Catalog 🔨** — system tables (`chronos_tables`, `chronos_columns`, `chronos_indexes`) stored as ordinary heap files in the engine itself, bootstrap via hardcoded page-0 roots, transactional DDL, schema cache invalidation. **Ship: a self-describing catalog that survives kill -9 mid-CREATE TABLE.** → `phase_specs/catalog.md`

**5.2 SQL lexer & parser 🔨** — hand-rolled lexer + recursive-descent/Pratt parser for the OLTP subset (CREATE TABLE/INDEX, INSERT, SELECT w/ WHERE·join·ORDER BY·LIMIT, UPDATE, DELETE, BEGIN/COMMIT/ROLLBACK, SET TRANSACTION), `Result<T>` errors with positions. Second rep after quackdb L20-21 — budget it short. **Ship: parser + golden-AST suite + grammar-derived random generator that round-trips + must-fail corpus with asserted positions.** → `phase_specs/sql_parser.md`

**5.3 Binder, planner & Volcano executor 🔨** — bind against the catalog; heuristic index-vs-seq planning; pull-based row iterators (SeqScan, IndexScan, Filter, Project, NestedLoopJoin, Sort+Limit, Insert/Update/Delete) wired to snapshots and locks; 📖 cost-based optimization (quackdb L25-26 territory — out of scope). **Ship: end-to-end SQL, green on a seeded statement-stream differential suite vs SQLite.** → `phase_specs/planner_executor.md`

## TIER VI — Surface area & proof: server, torture, benchmarks, capstone

**6.1 Network server 🔨** — io_uring connection loop (multishot accept), session = txn context, resumable message-framing state machine, 🧩 PostgreSQL simple-query protocol subset so real `psql` connects. **Ship: `psql -h localhost` running transactions against chronos, surviving a chaos-client soak with an orphan-txn/lock invariant scan.** → `phase_specs/network_server.md`

**6.2 Torture rig 🔨** — fault-injecting shim at the `io::` seam (torn writes, dropped writes / fsync lies, short reads, EIO), seeded deterministic schedules, SIGKILL orchestration generalized to full-SQL workloads. **Ship: `chronos-torture` — one command, N seeded crash/fault schedules, invariant violations reported with a replayable seed.** → `phase_specs/torture_rig.md`

**6.3 Benchmarks 🔨** — YCSB-style A–F + TPC-C-lite (NewOrder, Payment, OrderStatus), hand-rolled latency histograms, coordinated-omission-safe load gen; ablations: group-commit window, pool size, isolation level. **Ship: RESULTS.md with tpmC-style numbers, latency distributions, ablation curves, + a SQLite-WAL-mode baseline column — TPC-C consistency conditions green after every run.** → `phase_specs/benchmarks.md`

**6.4 Capstone 🔨** — integration only, no new subsystems: server → psql → TPC-C load → kill -9 mid-benchmark → timed ARIES recovery → consistency checks green → benchmark resumes. **Ship: `./demo.sh` green end-to-end on a stranger's Linux box + ARCHITECTURE.md + design-decision log + Postgres/InnoDB/SQLite comparison + the exam-question self-test answered from memory.** → `phase_specs/capstone.md`

---

## Order (execute this)

0.1 → 0.2 → 0.3 → 1.1 → 1.2 → 1.3 → 2.1 → 2.2 → 2.3 → 3.1 → 3.2 → 3.3 → 2.4 → 4.1 → 4.2 → 4.3 → 5.1 → 5.2 → 5.3 → 6.1 → 6.2 → 6.3 → 6.4

(2.4 floats — it needs only Tier 0; doing it as a palate-cleanser after ARIES works well. 2.3 can also float anywhere after 1.3.)

## Hard dependencies

```
0.1 ──► 0.3 ──► 1.2 ──► 1.3 ──► 2.1 ──► 2.2 ─────────┐
0.2 ──► 0.3     ▲       │ └──► 2.3                    │
        1.1 ────┘       └────► 3.1 ──► 3.2 ──► 3.3 ──►├─► 4.1 ──► 4.2 ──► 4.3
0.1 ───────────────────────────► 3.1            2.4 ──┘            │
                                                                   ▼
                       2.1 ─────────────────────────► 5.3 ◄── 5.2 ◄── 5.1
                                                       │
                                          6.1 ◄────────┘
                                          6.2 ◄── 3.3 + 5.3
                                          6.3 ◄── 6.1 + 6.2
                                          6.4 ◄── 6.3
```

Load-bearing seams to leave open early: 1.1 reserves the pageLSN slot · 1.3 leaves the recLSN/flushedLSN eviction hook · 2.1's iterator-validity contract becomes 2.2's latching contract and 4.3's phantom tracking · 0.3's `io::` interface is the seam 6.2 injects faults through.

## quackdb overlap map (don't rebuild what the sibling taught — rebuild what it grazed)

| Topic | quackdb (OLAP, Rust) | chronos (OLTP, C++) — the difference |
|---|---|---|
| Arena allocator (L01) | bump arena for column vectors | page-aligned slabs with O_DIRECT alignment + async-I/O-owned lifetimes |
| Pages / buffer pool (L09-10) | single-threaded read cache over immutable columnar blocks | concurrent, pin-counted, write-back, WAL-coupled eviction |
| SQL frontend (L20-21) | Pratt parser, panics available | smaller transactional grammar, `Result<T>` error discipline — second rep, go fast |
| Catalog / binder (L23) | in-memory metadata map | durable transactional system tables that survive ARIES recovery |
| Planning / execution (L13-24) | vectorized push pipelines over columns | pull-based Volcano over rows with visibility checks + locks in the scan path |
| MVCC (L27) | coarse snapshot versioning for analytics | per-tuple version chains, visibility rules, write-conflict detection, vacuum |
| WAL (L28) | logical redo-only log | physiological records, LSN protocol, group commit, full ARIES |

Deliberately NOT re-covered here: compression encodings (L05-08), vectorized operators (L13-19), cost-based optimization (L25-26), distribution (L31-33), SIMD (L35). Replication / 2PC: 📖 future-work notes in the 6.4 writeup only.

## Definition of done

1. Every phase's named build artifact is shipped: harness green against its independent oracle, RESULTS.md published, a stranger can rerun it.
2. `./demo.sh` (6.4) runs green on a machine that isn't yours.
3. You can answer every spec's exam questions at a whiteboard, from memory, and defend the design decisions in ARCHITECTURE.md against the interview attack.
4. The torture rig (6.2) has run overnight at least once with zero unexplained reds.

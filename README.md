# chronos-db

A from-scratch OLTP database engine in C++20. No frameworks, no storage libraries, no exceptions, no RTTI — every component below is hand-written and proven against an independent oracle before the next one builds on it.

The end state: a database server that real `psql` connects to, running transactions at snapshot isolation and above, that survives `kill -9` mid-benchmark and recovers via ARIES — demonstrated by a single `./demo.sh`.

## What gets built (in order)

| Layer | Components |
|---|---|
| The metal | durability contract (O_DIRECT, fsync, torn writes, CRC) · slab/arena allocators · io_uring async I/O engine |
| Storage | slotted pages + tuple codec · disk manager + heap files · concurrent buffer pool (clock / LRU-K, write-back) |
| Indexes | B+Tree (then latch crabbing + optimistic lock coupling) · extendible hash index · concurrent skiplist |
| Durability | write-ahead log (physiological, group commit) · fuzzy checkpoints · ARIES recovery (redo/undo, CLRs) |
| Concurrency | 2PL lock manager + deadlock detection · MVCC with vacuum · isolation levels through serializable SI |
| SQL | self-hosted catalog · lexer/parser · binder, planner, Volcano executor |
| Proof | io_uring network server (Postgres wire subset) · fault-injection torture rig · YCSB + TPC-C-lite benchmarks · the capstone demo |

Each component ships with a harness judged by an oracle the engine code cannot see: shadow models, differential references (`std::map`, synchronous I/O, SQLite), spec-derived decoders (`waldump`), seeded SIGKILL torture loops, and TPC-C's consistency conditions. Green-but-unproven doesn't ship.

## Relationship to quackdb

This is the OLTP half of a two-project deep dive. The sibling, [quackdb](../quackdb), covers the OLAP half (columnar storage, compression, vectorized execution, cost-based optimization, distribution). chronos goes deep on what an analytical engine can afford to graze: write-ahead logging, crash recovery, buffer management under writes, index latching, lock management, and multi-version concurrency.

## Layout

```
include/chronos/   public headers
src/               implementations
test/<phase>/      independent-oracle harnesses, one per build phase
RESULTS/           per-phase results writeups (harness tables, numbers, traps)
.curriculum/       the build plan, phase specs, and operating contract
```

The roadmap and the spec for every phase live in [`.curriculum/build_plan.md`](.curriculum/build_plan.md) and [`.curriculum/phase_specs/`](.curriculum/phase_specs/).

## Build discipline

- C++20, `-Wall -Wextra -Werror -fno-exceptions -fno-rtti`
- Errors via a `Result<T>` type; no exception paths anywhere
- No external dependencies for engine logic (liburing optionally, for the I/O layer)
- ASan/UBSan/TSAN builds are acceptance gates, not afterthoughts
- Durability claims are only made after seeded crash-injection runs, never after clean exits

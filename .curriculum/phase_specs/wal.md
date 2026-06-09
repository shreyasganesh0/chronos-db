# Tier III · Phase 3.1 — Write-ahead log (scaffold)

**Deliverable of this phase:** a `WalManager` — physiological log records, LSN protocol, group commit, the flushedLSN/pageLSN contract wired into the 1.3 buffer pool — plus a standalone `waldump` tool built from your written format spec with **zero shared code**, a crash-tail torture pass, and a commits/sec-vs-latency curve for the group-commit knob.
**What you'll own afterward:** the durability substrate of the entire engine. Every later phase that says "survives kill -9" is really saying "the WAL protocol from 3.1 held." You'll be able to reason about any production database's redo log — pg_wal, InnoDB's ib_logfile — at the level of "which LSN invariant is violated," not "the log got corrupted somehow."
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is a log manager, not a recovery system.** What you build here is an ordered, durable, self-describing byte stream with one load-bearing property: **strict prefix validity** — at any crash instant, the on-disk log is a valid prefix of everything ever appended, records are individually checksummed, and the first invalid record marks the exact end of history. Nothing in this phase reads the log to *do* anything with it. Analysis, redo, undo — all of that is Phase 3.3. If you catch yourself writing code that replays a record onto a page, stop; you're three phases early and building the wrong artifact.

It is also **not a journal of SQL statements or operations**. quackdb's L28 WAL was a logical redo-only log — "re-run this insert" — with no LSN protocol, which is fine for append-only analytics and useless for ARIES. chronos logs **physiologically**: each record names a specific page and describes the change *within* that page logically (which slot, what bytes), so redo is page-targeted and idempotence can be decided per-page by comparing LSNs. The physiological choice, the LSN chain, and the flushedLSN/pageLSN handshake with the buffer pool are precisely the parts quackdb skipped — and they are the parts that make ARIES possible at all.

The right artifact test: **"a log manager plus an independent dump tool, not a recovery system."** When a stranger can run `waldump` over a SIGKILLed log directory and get a clean, complete, machine-readable account of exactly what survived — that's done.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

The ability to design and defend a durability protocol — to say, for any sequence of appends, stamps, flushes, and evictions interleaved across threads, *exactly* what is guaranteed to be on disk after power loss at any instant, and to prove it with a torture loop rather than assert it with prose. Engineers who own this can debug "data loss after crash" tickets at the level of "the eviction beat the flush" instead of re-reading fsync man pages and hoping.

### Why "from scratch" is the right call here

Use an embedded WAL library (or read Postgres source instead of building) and the following go permanently opaque:

- **Why group commit exists** — you'll never feel the fsync wall (Phase 0.1's measured numbers) forcing N committers onto one flush, so 6.3's group-commit ablation will be a curve you can't explain.
- **Why the buffer pool needs a WAL hook at all** — "WAL-before-data" sounds like a slogan until you've personally watched an eviction write a page whose log record wasn't durable yet. Phase 3.3's torture loop will fail mysteriously if 3.1's hook is wrong, and you won't know which phase to blame.
- **Why recovery can trust pageLSN** — the stamping discipline (every mutation, before unlatch) is what makes redo idempotent in 3.3. Inherit it and you'll cargo-cult it; build it and you'll know exactly which stamp a skipped redo depends on.
- **Why log records carry prevLSN** — undo in 3.3 walks these chains backward. Get the chain semantics from a library and rollback (4.1) becomes a black box.
- **What a torn log tail actually looks like** — you handled torn *pages* in 0.1; the log's append-only variant (valid prefix, CRC-delimited) is the other half of the durability story.

### What carries forward to later tiers

- **Phase 3.2** logs its begin/end checkpoint records through this `WalManager` and truncates these segment files.
- **Phase 3.3** is a pure consumer: analysis scans this log, redo is gated by the pageLSNs you stamp here, undo walks the prevLSN chains you thread here. Every 3.1 invariant becomes a 3.3 correctness dependency.
- **Phase 4.1** wires transaction begin/commit/abort into these records; rollback-without-crash is "walk prevLSN and undo," reusing this chain.
- **Phase 6.2** injects faults at the `io::` seam underneath this log — fsync lies, torn appends — and your prefix-validity property is what its verifier asserts.
- **Phase 6.3** ablates the group-commit knob you ship here; the commits/sec-vs-latency curve you measure now is the baseline.
- **`waldump`** becomes a permanent debugging tool — every Tier III/IV bug hunt starts with it, exactly as `pg_waldump` does in Postgres incident response.

### What good looks like

- You can state the three-clock invariant from memory and point at the line of harness output proving it: for every page on disk, `pageLSN ≤ flushedLSN ≤ end of valid log`.
- `waldump` builds with no engine objects on its link line — provably, from the build graph — and still parses every byte the engine writes. If they ever disagree, you treat it as a format-spec bug, not a "fix the tool" bug.
- Appends from N threads never block on fsync; only committers wait, and `strace` shows one fsync serving a batch of them.
- The crash-tail torture has run hundreds of SIGKILL iterations with zero verifier reds, and you can explain what the verifier checks without looking.
- You can answer cold: *"a thread reserves log space, gets descheduled before copying, and the flusher wakes up — what stops the flusher from fsyncing a hole?"* That question is the heart of this phase.

### Why this is the shape of the deliverable

A library plus an **adversarially independent dump tool** plus a torture rig, because the failure mode of a WAL is silent: a log that round-trips through its own writer/reader code can be self-consistently wrong (writer and reader share the same off-by-one) and you'd never know until recovery eats it in 3.3. The only honest oracle for a format is a second implementation built from the written spec — hence `waldump`, hence zero shared code, hence the rule that the format-spec document, not the engine, is the source of truth. The performance curve ships because group commit is a *trade* (latency for throughput), and trades get measured, not asserted.

## Exam questions this phase targets (build-proven)

1. Implement a physiological WAL with LSN assignment, per-txn prevLSN chains, and pageLSN stamping such that the harness's post-crash invariant row (`pageLSN ≤ durable-log-end` for every page on disk) PASSes across the SIGKILL torture loop.
2. Implement group commit such that the harness's `strace`-based row proves one fsync retires N concurrent committers, and report the measured commits/sec-vs-latency curve at three knob settings.
3. Write a format spec precise enough that an independent tool (`waldump`, zero shared code) parses every record the engine emits, including the torn tail after a crash.
4. Defend at a whiteboard: why physiological and not physical or logical logging? What breaks under each alternative? (ARIES §10.1 territory — answer from your build, not the paper.)

## Prerequisites — concepts this phase uses

These appear in the spec, the contract, and the harness. Recognize each term and the role it plays. Do **not** look up implementations — implementing them is the build. Primary source: Mohan et al., *ARIES: A Transaction Recovery Method...*, ACM TODS 17(1), 1992 — read §1–3 for vocabulary now; the algorithm sections wait for 3.3. Practitioner-level orientation: PostgreSQL docs *WAL — Introduction* (https://www.postgresql.org/docs/current/wal-intro.html) and `pg_waldump` (https://www.postgresql.org/docs/current/pgwaldump.html); MySQL *InnoDB Redo Log* (https://dev.mysql.com/doc/refman/8.4/en/innodb-redo-log.html).

### The WAL rule and logging styles

- **write-ahead rule** — the protocol invariant that gives the log its name: a log record describing a page change must be durable *before* the changed page may be written to disk. Enforced mechanically (the 1.3 eviction hook), never by convention.
- **physiological logging** — Gray & Reuter's term, adopted by ARIES: a record is *physical to the page* (names one specific page) and *logical within the page* (describes the change in terms of slots/fields, not raw byte images). Contrast: pure physical (byte diffs — bloated, but trivially idempotent) and pure logical ("insert into T" — compact, but redo must re-run engine logic against a possibly inconsistent page). Physiological is the ARIES sweet spot; chronos uses it for both redo and (in 3.3) undo.
- **commit record** — the record whose durability *is* the transaction's commit. A transaction is committed when its commit record is on stable storage — not when the client hears back. This definition becomes load-bearing in 3.3's oracle.

### The LSN protocol

- **LSN (log sequence number)** — the address of a log record; monotonically increasing, totally ordered. In Postgres and in ARIES it doubles as a byte position in the log address space, which makes "how far is durable" a single comparison. The role: a global logical clock for "what happened before what."
- **prevLSN** — each record carries the LSN of the *same transaction's* previous record, forming a per-txn backward chain through the interleaved log. The role: 3.3's undo and 4.1's rollback walk this chain; it's how one transaction's history is extracted from everyone's log.
- **pageLSN** — a field in every page header (the slot Phase 1.1 reserved) holding the LSN of the last record that modified the page, stamped at mutation time. The role: 3.3's redo compares record-LSN against pageLSN to decide "already applied?" — the entire idempotence story of ARIES rests on this stamp.
- **flushedLSN** — the highest LSN known durable (fsync returned). The role: the gate in the WAL rule — a dirty page with `pageLSN > flushedLSN` must not be written.

### Log storage machinery

- **log buffer** — an in-memory staging area where appended records accumulate before a flush; circular, so writers wrap around and must not overwrite unflushed bytes (backpressure). Every serious engine has one (InnoDB `innodb_log_buffer_size`, Postgres WAL buffers).
- **segment file** — the log is a sequence of fixed-size files, not one endless file, so old history can be deleted/recycled by the segment (3.2's truncation). Postgres: 16 MB WAL segments.
- **segment epoch / generation** — a header field distinguishing a segment's current incarnation from a recycled past life. The role: a recycled file is full of *valid old records*; without an epoch check, a scanner happily reads the past as the present.
- **torn tail / prefix validity** — the append-only analogue of 0.1's torn page: a crash mid-append leaves a partial record at the end. The contract is that per-record CRCs (your 0.1 slice-by-8) delimit history exactly: everything before the first CRC failure is real, nothing after it may be trusted.

### Group commit

- **group commit** — batching the commit-fsync: when many transactions commit concurrently, one fsync covering the latest commit record durably commits *all of them*. The role: fsync costs ~milliseconds (you measured it in 0.1); without batching, commit throughput is capped at 1/fsync-latency. Postgres exposes this as `commit_delay`/`commit_siblings`.

## How you know you're aligned (the cross-check)

You are never building blind. The harness in `test/wal/` is the **continuous alignment oracle**: build a part, run it, watch rows flip SKIP → PASS. A green row is proof the observable shape is right regardless of how you laid out the buffer or scheduled the flusher — the harness sees only the public surface below, never your internals.

Two genuinely independent oracles keep it non-circular. First, **`waldump` itself**: because it shares zero code with the engine and is written against the format-spec *document*, agreement between engine and tool is evidence about the format, not a tautology — the harness round-trips every workload through `waldump`'s output, not through engine readers. Second, the **crash-tail verifier**: the harness forks a child that appends in a loop and reports each acknowledged flush to the parent over a pipe; the parent SIGKILLs at a random instant, then asserts the prefix-validity property on the dead child's log directory (every record up to the first CRC failure parses; every flush-acknowledged LSN is present; nothing valid appears after the tear) using `waldump --verify` as the reader. The group-commit row wraps the engine in `strace -f -e fsync,fdatasync` and counts syscalls against committed transactions — the kernel's own ledger, which your code cannot fool.

## The build, in parts (each gated independently by the harness)

### Part 1 — Record format, LSN protocol, single-threaded append 🔨
The format-spec document (`docs/wal_format.md` — yours to write, and from this moment the source of truth), plus a `WalManager` that appends physiological records to segment files with correct LSN assignment, per-txn prevLSN threading, per-record CRCs, and segment headers carrying epochs. Page mutations in 1.1/1.2 code paths now stamp pageLSN.
**Harness rows:** append/reopen round-trip via the harness's spec-derived scanner; LSN monotonicity; prevLSN chain extraction per txn; segment-header/epoch sanity.

### Part 2 — `waldump`, the independent oracle 🔨
A standalone binary built **only** from `docs/wal_format.md` — no engine headers, no shared objects. One machine-parseable line per record (LSN, type, txn, prevLSN, page, length, CRC status); `--verify` mode that walks a log directory and reports the exact durable frontier or the first violation, with a meaningful exit code. Resist with your life the urge to `#include` anything from the engine; the moment you do, the oracle dies.
**Harness rows:** `waldump` output vs the harness scanner on seeded workloads; link-line independence check; torn-tail and epoch-violation fixtures correctly flagged.

### Part 3 — Log buffer, wraparound, the flush frontier 🔨
The in-memory buffer with multi-threaded append, wraparound with backpressure, and a flush path that advances flushedLSN. The hardest idea in the phase lives here: concurrent reservers create holes, and **the flusher must never fsync past an unfinished copy**. `FlushTo(lsn)` blocks until the requested LSN is durable.
**Harness rows:** N-thread append fuzz (TSAN build) with full waldump verification afterward; wraparound stress (buffer ≪ workload); crash-tail torture (the SIGKILL loop) goes green here and stays green for the rest of the phase.

### Part 4 — Group commit + the eviction hook 🔨
`Commit()` batches concurrent committers onto shared fsyncs, tunable via the group-commit knob. The 1.3 buffer pool's eviction hook is wired: no dirty frame is written until `FlushTo(pageLSN)` has completed — WAL-before-data enforced by the pool, not by good intentions. Measure the commits/sec-vs-latency curve across knob settings.
**Harness rows:** strace fsync-count row (one fsync, N committers); the post-crash three-clock invariant row (no on-disk page's pageLSN exceeds the durable log end) under eviction-heavy torture; knob monotonicity sanity.

### Part 5 — The harness as a published artifact 🔨
Every row green, including the TSAN build and the full torture count. Write `RESULTS.md` from `templates/RESULTS.md`: the row table, the torture iteration count, the strace evidence, the group-commit curve, and a paragraph on the reserve-then-copy hole problem — how you detected it and what your design guarantees. Publish; a stranger reruns it.

## API contract (what the harness links against)

Namespace `chronos::wal`. Everything below is the **public surface** — the only things the harness touches. **PRIVATE INTERNALS, deliberately unspecified:** the record payload encoding (frozen by *your* `docs/wal_format.md`, not by this spec), the log-buffer layout and reservation mechanism, flusher threading, how prevLSN chains are tracked, segment naming. If you look for them here and don't find them — that's the design work, and it's yours.

### `Lsn`

```cpp
using Lsn = uint64_t;          // totally ordered; kInvalidLsn = 0
```

- **What it does:** addresses one log record; compares with `<` to mean "logged earlier."
- **Why it exists:** the global logical clock every durability decision is phrased in.
- **Critical contract details:** strictly monotonic across all threads; never reused, even across restarts and segment recycling (the epoch design must make this hold).

### `WalManager`

```cpp
static auto Open(const WalOptions& opts) -> Result<WalManager>;
auto Append(const LogRecord& rec) -> Result<Lsn>;          // rec carries txn_id, page_id, type, payload
auto Commit(TxnId txn) -> Result<Lsn>;                     // append commit record + block until durable
auto FlushTo(Lsn lsn) -> Result<void>;
auto flushed_lsn() const -> Lsn;                           // non-blocking
```

- **What it does:** `Append` assigns the next LSN, threads the record onto its transaction's prevLSN chain, copies it into the log buffer, and returns the LSN — with **no durability implied**. `Commit` appends the commit record and returns only after it is durable (this return is the commit point and the only ack the 3.3 oracle will honor). `FlushTo` blocks until `flushed_lsn() ≥ lsn`. `WalOptions` carries directory, segment size, buffer size, and the group-commit knob (an explicit, documented parameter — the 6.3 ablation turns it).
- **Why it exists:** the exact split every engine makes — cheap concurrent append for mutations, expensive durability only at commit and at eviction.
- **Side-effect / durability requirement:** after `Commit`/`FlushTo` returns, the covered records survive SIGKILL and power loss; `waldump --verify` finds them. After a crash, the log is a valid prefix: CRC-delimited, no acknowledged record missing, no phantom record present.
- **Critical contract details:** all methods thread-safe; `Append` must not block on fsync (backpressure on a full buffer is the only permitted wait); concurrent `Commit`s may share one fsync; errors via `Result<T>` — an fsync failure poisons the manager (no silent retry; 0.1's fsync-gate lesson).

### pageLSN stamping (contract on Phase 1.1's `Page`)

- **What it does:** every page-mutating operation, before releasing the page, writes the LSN of its log record into the page-header slot reserved in 1.1.
- **Why it exists:** the idempotence anchor for 3.3's redo; the input to the eviction gate.
- **Side-effect / durability requirement:** no code path mutates page bytes without a same-or-newer pageLSN stamp. The harness checks the consequence, not the call sites: post-crash, every on-disk page satisfies `pageLSN ≤` durable-log-end.

### The eviction hook (contract on Phase 1.3's `BufferPool`)

- **What it does:** before the pool writes any dirty frame (eviction or background write-back), it completes `FlushTo(frame.pageLSN)`. The 1.3 seam is now load-bearing.
- **Why it exists:** this *is* the write-ahead rule, enforced at the only choke point pages pass through on their way to disk.
- **Side-effect / durability requirement:** at no instant does the data directory contain a page whose pageLSN exceeds the durable log frontier — the torture verifier's central invariant.

### `waldump` (standalone binary, `tools/waldump`)

```
waldump [--verify] <wal-dir>     → one record per line on stdout; exit 0 iff prefix-valid
```

- **What it does:** parses a log directory using *only* `docs/wal_format.md`; prints LSN, type, txn, prevLSN, page, length, CRC status per record in a stable machine-parseable format; `--verify` reports the durable frontier or the first violation (bad CRC, bad epoch, LSN discontinuity) with its byte offset.
- **Why it exists:** the independent oracle — and your permanent Tier III/IV debugging instrument, as `pg_waldump` is for Postgres.
- **Critical contract details:** zero shared code with the engine (the harness checks the link line); must enforce your documented segment-boundary rule (see traps) and reject stale-epoch records.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS in `test/wal/`, including the TSAN build of the multi-threaded append/commit rows. ASan/UBSan builds green.
2. Crash-tail torture: ≥ 500 seeded SIGKILL iterations, zero verifier violations; the three-clock post-crash invariant green under eviction-heavy load.
3. `waldump` parses every harness workload byte-for-byte and provably links no engine code; `docs/wal_format.md` is checked in and current.
4. strace row proves one fsync serving N committers; commits/sec-vs-latency curve measured at ≥ 3 knob settings and published.
5. `RESULTS.md` published; a stranger can rerun everything.

## Principal-engineer traps (no solutions)

- **The reservation hole.** LSN assignment and the buffer copy want to be one atomic act but can't be — between "reserve space" and "finish copying," there is a hole, and the flusher must not fsync past it. Decide how the flusher knows the frontier of fully-copied bytes, and write the rule down before TSAN and the torture loop make you. This is the hardest concurrency bug in this tier; the crash-tail row exists for it.
- **Records that span a segment boundary.** Decide explicitly: forbid-and-pad, or support continuation. Either is defensible; an *undecided* policy is how `waldump` and the engine drift. Whatever you choose, `waldump` must enforce it — the format doc is where the decision lives.
- **Recycled segments are full of valid history.** Every record in a recycled file has a correct CRC — it's just from the past. If the epoch check isn't airtight, 3.3's recovery will replay it as the present, and the bug will look like anything except what it is.
- **The eviction hook is now load-bearing.** It was a seam in 1.3; it's the WAL rule now. The failure is silent until a crash lands in exactly the wrong window — which is why the invariant is checked post-crash by the torture verifier, not by a unit test on the hook.
- **`Commit` returning early.** Any path where `Commit` returns before fsync — an "optimization," a missed error branch, a lost wakeup in the group-commit batching — is an acked-but-lost transaction, the one unforgivable sin. The 3.3 oracle is built to catch precisely this; cheaper to catch it now.
- **Don't let `waldump` learn from the engine.** Copy-pasting one struct "to save time" quietly kills the oracle. If parsing feels hard without engine code, the format doc is incomplete — fix the doc.

## What you hand back for review

1. `WalManager` + `waldump` + `docs/wal_format.md` + the harness table + torture counts + the strace evidence + the group-commit curve.
2. One sentence per trap above: did it bite, and how was it resolved.

Review will be principal-engineer style: the interview attack on your reservation design ("flusher wakes mid-copy — walk me through every byte it may fsync"), the segment-boundary policy defense, and whether the durability claims are torture-proven or merely clean-exit-proven. Then we advance to Phase 3.2.

*Start Part 1 when ready — `docs/wal_format.md` is yours to write from a blank page, and it leads the code, not the other way around.*

# Tier III · Phase 3.2 — Fuzzy checkpoints (scaffold)

**Deliverable of this phase:** fuzzy checkpointing on top of the 3.1 log — begin/end checkpoint records carrying the Active Transaction Table and Dirty Page Table (with recLSNs), a master record updated atomically via the 0.1 primitive, log truncation — plus a measured recovery-work-vs-checkpoint-interval mini-study.
**What you'll own afterward:** the ability to bound recovery time without ever stopping the engine — and the understanding of why every serious database checkpoints *fuzzily*, which is the difference between "checkpoint" as a marketing word and as a protocol.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**A fuzzy checkpoint is a bookmark, not a consistency point.** It records *where to start looking* — a snapshot of which transactions are active (ATT) and which pages are dirty and since-when (DPT, with recLSNs) — taken **while the system runs, flushing nothing**. At the instant the checkpoint completes, the database on disk is still inconsistent: dirty pages are still dirty, active transactions are still mid-flight, and that is the entire point. The checkpoint doesn't make anything consistent; it tells 3.3's analysis pass where history's relevant portion begins, so recovery scans bounded log instead of all log since genesis.

It is **not a flush-everything snapshot**, and this is the confusion that ruins the phase if you carry it in. The intuitive design — quiesce writers, flush every dirty page, write "CHECKPOINT: all clean" — is a *sharp* (stop-the-world) checkpoint. It works, it's what people build by instinct, and it teaches nothing: it stalls the engine for the duration of the flush storm, it's what ARIES was specifically designed to obsolete, and it makes the DPT pointless (everything's clean, so why record it?). If your checkpoint blocks transactions or issues page writes, you have built the wrong artifact. The ARIES insight is that recording *the names and recLSNs* of dirty pages is exactly as useful to recovery as flushing them — at zero I/O and zero stall.

The right artifact test: **"a running engine emits checkpoints under full write load with zero page flushes and no observable stall, and recovery's starting point is provably correct anyway."**

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

Reasoning about *bounded* recovery: given a log, a DPT of recLSNs, and an ATT, you can state exactly which LSN recovery must scan from, which log can be deleted, and why — and you can defend the truncation point as a theorem about the protocol, not a heuristic. This is the skill behind every production conversation about checkpoint tuning, WAL retention, and "why is crash recovery taking forty minutes."

### Why "from scratch" is the right call here

- **Postgres's `CHECKPOINT` and InnoDB's adaptive flushing look like flush machinery** — both flush pages as a *side activity* of advancing the checkpoint. Use them as your mental model without building the ARIES core first and you'll conflate "checkpoint" with "flush," which is exactly the misconception this phase exists to break.
- **The truncation formula is where inherited code hides the proof.** `min(min recLSN, firstLSN of oldest active txn)` is two distinct obligations — redo's and undo's — and getting *either* wrong means recovery dereferences deleted log. Build it and you own the proof; inherit it and you own a config knob.
- **The master record is a torn-write hazard in miniature** — the exact failure 0.1's atomic-write primitive was built for. Wiring your own primitive into its first real consumer closes a loop the curriculum opened three tiers ago.
- **Checkpoint frequency is a measured trade.** Without building both ends — the logging cost of frequent checkpoints, the recovery cost of rare ones — "how often should we checkpoint?" stays a vibe. The mini-study makes it a curve you produced.

### What carries forward to later tiers

- **Phase 3.3** starts every recovery at your master record and seeds its analysis pass with your checkpoint's ATT/DPT; the rebuild function you expose this phase *becomes* the analysis pass there. 3.3's torture also reruns against truncated logs — your truncation correctness is on its critical path.
- **Phase 4.1**'s transaction manager takes over feeding the ATT (txn begin/commit/abort become first-class lifecycle events); the table you define here is the interface it fills.
- **Phase 6.2**'s fault injector tears the master-record write on purpose; surviving it is your 0.1-primitive wiring, proven again under hostile I/O.
- **Phase 6.3/6.4** time real recovery mid-benchmark; the interval study you do now is the rehearsal, redone there with full ARIES wall-clock numbers.

### What good looks like

- Checkpoints complete under a full-throttle write workload with no transaction observing a stall and `waldump` showing zero page-write pressure attributable to the checkpointer.
- You can state from memory what lives between BEGIN_CKPT and END_CKPT in the log (arbitrary other records — that's what *fuzzy* means) and why the captured tables are still safe to seed analysis with despite racing with mutations.
- The master record always names a *complete* checkpoint: a crash at any instant during checkpointing leaves the previous master intact and usable.
- Truncation never deletes a segment any reachable LSN points into — recLSNs, prevLSN chains of active transactions, the checkpoint's own records — and you can prove which term of the min() protects which pointer.
- The interval study has real numbers and a sentence of interpretation, not just a table.

### Why this is the shape of the deliverable

A protocol extension plus a measurement, because this phase's two failure modes are opposite in kind. Correctness failures are silent and crash-timed — a wrong recLSN or a torn master record costs nothing until recovery needs it, which is why the harness reconstructs truth independently from the full log and crashes you mid-checkpoint on purpose. The tuning question, meanwhile, has no correct answer at all, only a trade — which is why the deliverable includes a curve instead of a constant.

## Exam questions this phase targets (build-proven)

1. Implement fuzzy checkpointing such that the harness's ATT/DPT-equivalence row PASSes: tables rebuilt from your latest checkpoint must match what an independent scanner reconstructs from the entire log — under a write workload running *during* the checkpoint.
2. Implement log truncation such that the harness's pointer-reachability row PASSes (no recLSN, no active txn's chain, no master pointer dangles below the cut), and state the truncation formula and what each term protects.
3. Survive SIGKILL at randomized instants *during* checkpointing — including mid-master-record-update — with the master always naming a complete, usable checkpoint.
4. Produce the recovery-work-vs-checkpoint-interval curve and defend an interval choice from it.

## Prerequisites — concepts this phase uses

Vocabulary only — the role each term plays, never the implementation. Primary source: Mohan et al., *ARIES*, ACM TODS 17(1), 1992, §4.3 (checkpoints) and §5.4 (restart analysis) — read for the *shape* of what's recorded, not for code. Practitioner orientation: PostgreSQL *WAL Configuration* (https://www.postgresql.org/docs/current/wal-configuration.html — `checkpoint_timeout`, `max_wal_size` are this phase's knob in production trim); InnoDB *Checkpoints* (https://dev.mysql.com/doc/refman/8.4/en/innodb-checkpoints.html).

### Checkpoint state

- **ATT (Active Transaction Table)** — the set of in-flight transactions at a given instant, each with its txn id, state, and **lastLSN** (most recent log record — the entry point into its prevLSN chain). Role: tells recovery who was mid-flight and therefore who may need undoing. In this tier the WAL layer maintains it from begin/commit/abort records; 4.1's transaction manager takes over later.
- **DPT (Dirty Page Table)** — the set of pages that are dirty in the buffer pool, each with its **recLSN**. Role: tells recovery which pages *might* be stale on disk, so redo knows where it must look — without anyone flushing anything.
- **recLSN (recovery LSN)** — per dirty page, the LSN of the *first* record that dirtied it since it was last clean — recorded by the buffer pool at the clean→dirty transition (the seam 1.3 left open, now load-bearing). Role: the earliest log record that could possibly need re-applying to that page; the minimum over the DPT is where 3.3's redo starts and one arm of the truncation formula.
- **fuzzy checkpoint** — a checkpoint taken while the system runs: a BEGIN_CKPT record, a capture of ATT + DPT, an END_CKPT record carrying them — with ordinary log records freely interleaved between the two. Role: bounds recovery's scan without stalls or flushes. Contrast with a **sharp checkpoint** (quiesce + flush everything), which buys the same bound by stopping the world.

### Anchoring and retention

- **master record** — a tiny, fixed-location, atomically-updated value pointing at the most recent *complete* checkpoint's BEGIN_CKPT LSN. Role: recovery's bootstrap — the one thing it reads before the log. Postgres's analogue is `pg_control`. It is written with the 0.1 atomic-write primitive because a torn master is a recovery that can't start.
- **log truncation** — deleting (or recycling — 3.1's epochs exist for this) log segments wholly below the point recovery could ever need. Role: keeps the log finite. The safe point is `min(min recLSN over the DPT, firstLSN of the oldest active transaction)` — redo's needs and undo's needs respectively; the formula is given here because it's a *contract*, and the harness checks both arms independently.
- **firstLSN of a transaction** — the LSN of its begin record; the floor its prevLSN chain bottoms out at. Role: undo of a loser may walk all the way back here, so no log below the oldest active firstLSN may be discarded.

## How you know you're aligned (the cross-check)

The harness in `test/checkpoints/` is the continuous oracle: build a part, run it, rows flip SKIP → PASS. It touches only the public surface below — never your checkpointer's internals or your table representations beyond the snapshot structs in the contract.

The independent oracle is the harness's **own log scanner** (spec-derived, built from `docs/wal_format.md` like `waldump`, sharing no engine code): for any workload it reconstructs the true ATT and DPT by scanning the **entire log from the beginning** — the brute-force ground truth a checkpoint exists to make unnecessary — and demands that `RebuildTables()` seeded from only your latest checkpoint produce equivalent tables. Your shortcut is checked against the no-shortcut answer; agreement can't be circular. Truncation is checked the same way: the harness keeps a pre-truncation copy of the log, reconstructs tables from both full and truncated versions, asserts equivalence, and walks every reachable pointer (recLSNs, ATT chains, the master) to prove none resolves below the cut. Crash-timing rows reuse 3.1's SIGKILL machinery, now aimed mid-checkpoint and mid-master-update. (Full state-equivalence of truncated-vs-full *recovery* is re-proven end-to-end by 3.3's torture, which reruns over logs you truncate here.)

## The build, in parts (each gated independently by the harness)

### Part 1 — ATT and DPT as live, capturable state 🔨
The two tables exist and are queryable while the engine runs: the WAL layer tracks active transactions (begin/commit/abort) with lastLSNs; the buffer pool records recLSN at every clean→dirty transition (the 1.3 seam, activated) and can snapshot its dirty set. No checkpoint records yet — just truthful tables.
**Harness rows:** snapshot-vs-full-log-scan equivalence at quiesce points; recLSN-is-first-dirtier checks under a seeded workload.

### Part 2 — Begin/end checkpoint records + the master record 🔨
The checkpoint protocol proper: BEGIN_CKPT, fuzzy capture, END_CKPT carrying the tables (through 3.1's `Append`; `waldump`'s format doc grows the new record types), then the master record updated via the 0.1 atomic-write primitive — only after END_CKPT is durable. `RebuildTables()` reads master → checkpoint → log tail and reconstructs the tables.
**Harness rows:** rebuild-equals-full-scan under concurrent write load (the central row of the phase); no-stall/no-flush row (workload latency and page-write counts indistinguishable with checkpointing on vs off); SIGKILL mid-checkpoint and mid-master-write torture — master always names a complete checkpoint.

### Part 3 — Log truncation 🔨
Compute the safe truncation point from the live tables; release whole segments below it through 3.1's segment machinery (epochs make recycling safe). A long-running transaction must visibly pin the log — that's the formula's second arm doing its job, and the harness checks it deliberately.
**Harness rows:** pointer-reachability after truncation; truncated-vs-full table reconstruction equivalence; the long-running-txn pin row; truncation never cuts above the master's checkpoint.

### Part 4 — The interval mini-study + the harness as a published artifact 🔨
Run a fixed seeded workload at several checkpoint intervals; for each, measure recovery *work* — bytes of log the harness scanner must scan from the master to the tail, and DPT size at crash — plus the logging overhead checkpoints add. (Wall-clock recovery time is remeasured with real ARIES in 3.3; this curve is its faithful proxy and you'll say so in the writeup.) Then: every row green, `RESULTS.md` from `templates/RESULTS.md` with the table, torture counts, the curve, and your interval recommendation. Publish; a stranger reruns it.

## API contract (what the harness links against)

Namespace `chronos::wal`. **PRIVATE INTERNALS, deliberately unspecified:** how the live tables are stored, how the fuzzy capture races with concurrent mutators, the checkpoint-record payload encoding (frozen by your `docs/wal_format.md`, which `waldump` must keep parsing), segment-release mechanics. The snapshot structs below are observable *contents*, not storage prescriptions.

### Table snapshots

```cpp
struct AttEntry { TxnId txn; Lsn last_lsn; Lsn first_lsn; };
struct DptEntry { PageId page; Lsn rec_lsn; };
struct Tables   { std::vector<AttEntry> att; std::vector<DptEntry> dpt; };
```

- **What it does:** a point-in-time capture of the two recovery tables, in the form the harness compares against its full-log reconstruction. Order is not part of the contract; contents are.
- **Why it exists:** the lingua franca between checkpointer, recovery (3.3 consumes exactly this shape), and harness.
- **Critical contract details:** `rec_lsn` is the *first* dirtier since last clean — not the latest (the classic inversion; the harness row exists for it). `first_lsn` must be present: truncation's second arm depends on it.

### `WalManager::TakeCheckpoint`

```cpp
auto TakeCheckpoint() -> Result<Lsn>;    // returns the BEGIN_CKPT LSN now named by the master
```

- **What it does:** runs one complete fuzzy checkpoint — BEGIN_CKPT, capture, END_CKPT, durable, then master update — concurrently with normal traffic, and returns the LSN the master now points at.
- **Why it exists:** the bound on recovery; the thing Postgres's checkpointer process does on `checkpoint_timeout`.
- **Side-effect / durability requirement:** flushes **no data pages** and blocks **no transactions** (brief latches on the tables excepted). On return, the checkpoint is durable and the master names it. A crash at any interior instant leaves the *previous* master intact and valid — torn-master is impossible by construction (the 0.1 primitive).
- **Critical contract details:** thread-safe against all engine activity; concurrent calls may serialize; `Result<T>` on I/O failure with the previous master untouched.

### `ReadMaster` / `RebuildTables`

```cpp
static auto ReadMaster(const std::filesystem::path& dir) -> Result<Lsn>;
auto RebuildTables() -> Result<Tables>;   // master → checkpoint → scan to the log tail
```

- **What it does:** `ReadMaster` bootstraps from the fixed-location master record. `RebuildTables` seeds the tables from the named checkpoint and rolls them forward through the log tail to the crash frontier — the read side of checkpointing, and the function 3.3 promotes into its analysis pass.
- **Why it exists:** if this is right, analysis in 3.3 is a renaming; if it's wrong, the harness's full-scan oracle says so now, three weeks before recovery would.
- **Side-effect / durability requirement:** read-only; safe on a freshly-crashed directory; tolerates the torn tail (stops at the 3.1 durable frontier).
- **Critical contract details:** must equal the full-log reconstruction (the harness's definition of correct); a missing/never-written master degrades to scan-from-genesis, not an error.

### `TruncateLog`

```cpp
auto ComputeTruncationPoint() -> Lsn;
auto TruncateLog() -> Result<Lsn>;        // returns the new lowest retained LSN
```

- **What it does:** computes `min(min recLSN over DPT, min firstLSN over ATT)` — also floored by the master's checkpoint — and releases only whole segments strictly below it.
- **Why it exists:** finite disk; the WAL retention policy every DBA eventually fights with (`max_wal_size`).
- **Side-effect / durability requirement:** after return, every LSN reachable from the master, the tables, or any active txn's chain still resolves; released segments re-enter 3.1's recycling with fresh epochs.
- **Critical contract details:** conservative under concurrency — when racing with new activity, it may keep too much, never too little.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS in `test/checkpoints/`; ASan/UBSan green; the concurrent-capture rows green under TSAN.
2. ≥ 200 seeded SIGKILL iterations across checkpoint-interior crash points (including mid-master-update), zero violations; the no-stall/no-flush row green under full write load.
3. Truncation rows green, including the long-running-transaction pin case; `waldump` parses the new record types (format doc updated).
4. The interval study published: curve, method, and a defended interval choice. `RESULTS.md` complete; a stranger can rerun.

## Principal-engineer traps (no solutions)

- **The truncation min() has two arms and people drop one.** min recLSN protects *redo*; oldest-active firstLSN protects *undo*. Drop the first and a stale on-disk page can't be repaired; drop the second and a loser can't be rolled back. Both failures are invisible until 3.3 dereferences missing log — the reachability row is your early warning, but know *which* arm each pointer class tests.
- **recLSN inversion.** Recording the *latest* dirtier instead of the *first* makes redo start too late and skip needed history — and almost every workload still recovers fine, until one doesn't. The clean→dirty transition is the only correct capture point; re-dirtying an already-dirty page must not move it.
- **The master record is the torn-write hazard, in miniature.** Two checkpoints' pointers, one fixed location, crash mid-overwrite: exactly the 0.1 demonstrator. Reaching for a bare `pwrite` here means Phase 0.1 didn't take; the mid-master SIGKILL row is aimed at this.
- **"Fuzzy" tempts you to over- or under-lock.** Quiesce the engine to capture the tables and you've built a sharp checkpoint with extra steps (the no-stall row fails); capture with no discipline at all and the tables can be inconsistent in ways analysis can't repair. The window between BEGIN_CKPT's LSN and the capture is where the subtlety lives — think it through and write your reasoning down for review.
- **Checkpoint frequency is a measured trade, not a vibe.** Too often: log overhead and table-capture churn for nothing. Too rare: unbounded recovery and unbounded WAL disk. If your study's curve is flat, your workload isn't dirtying enough pages to test anything — fix the workload, not the conclusion.

## What you hand back for review

1. The checkpoint + truncation implementation, the harness table, torture counts, and the interval-study curve with your recommendation.
2. One sentence per trap: did it bite, how resolved — plus your written reasoning for the fuzzy-capture window (the review will attack it).

Review will be principal-engineer style: the interview attack ("SIGKILL lands between END_CKPT's fsync and the master update — walk me through the next recovery"; "a transaction stays open for an hour — show me exactly what stops truncation, and what a DBA would see"). Then we advance to Phase 3.3, where everything this phase bookmarked gets cashed in.

*Start Part 1 when ready — the 1.3 recLSN seam has been waiting two tiers for this.*

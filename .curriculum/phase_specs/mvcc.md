# Tier IV · Phase 4.2 — MVCC: version store, snapshot isolation, vacuum (scaffold)

**Deliverable of this phase:** a per-tuple multi-version store with snapshot-isolation visibility, first-committer-wins write-conflict detection, and a horizon-driven vacuum — proven by an offline visibility oracle, a **mandatory write-skew reproduction**, and a vacuum check that reclaims every dead version under concurrent writers.
**What you'll own afterward:** the versioned-storage machine underneath every modern OLTP engine — deep enough that "readers don't block writers," "serialization failure," and "table bloat from an idle connection" stop being folklore and become mechanisms you can point at.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is the versioned-storage machine, not an isolation policy.** Phase 4.1 built the pessimistic answer to concurrency: conflicting transactions queue. This phase builds the optimistic one: writers create *new versions* instead of overwriting, readers see a frozen *snapshot* instead of waiting, and conflict is detected rather than prevented. The deliverable is the machine itself — version chains, snapshots, visibility, write-conflict detection, reclamation — operating correctly under concurrent load. What to *do* with the machine (which statements see which snapshot, when to escalate to serializable behavior) is Phase 4.3's policy layer; if you find yourself writing per-isolation-level branches here, you've drifted a phase ahead.

It is also **not** quackdb's L27. That was coarse table-level snapshot versioning over immutable columnar blocks — fine for analytics, useless for OLTP. chronos does the real thing: **per-tuple** version chains, per-snapshot visibility decisions on every read, first-committer-wins conflict detection on every overlapping write, and a vacuum that must reclaim under writers that never stop.

You're done with the framing when you can say: the deliverable is **versioned storage where readers never block writers and vice versa, with provably correct visibility and reclamation** — not an isolation-level switch, and not a lock manager with extra steps.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

You'll be the kind of engineer who can debug "my read returned stale data" / "my UPDATE failed with a serialization error" / "this table is 40 GB of dead rows" by reasoning about snapshots, commit timestamps, and the GC horizon — the exact triad behind the most common production Postgres incidents — instead of cargo-culting `VACUUM FULL` at 3 a.m.

### Why "from scratch" is the right call here

Treat MVCC as a black box and you will never be able to reason about:

- Why a transaction that read a value can fail at *commit*, minutes later, having done nothing else wrong (first-committer-wins surfacing).
- Why one idle `psql` session left open over a weekend pins gigabytes of dead versions across every table in the database (the horizon is a *minimum*).
- Why two transactions can each read a constraint, each write a disjoint row, both commit — and the constraint is now violated (write skew: SI's signature anomaly, which 4.3 exists to kill).
- Why "readers don't block writers" is compatible with writers aborting each other.
- Why vacuum under load is a memory-reclamation race — the same problem class as 2.2's epoch-based page reclamation, now at tuple granularity.

### What carries forward to later tiers

- **Phase 4.3** is policy *over this machine*: RC and RR are snapshot-timing and lock choices; SSI is SIREAD tracking layered on these exact snapshots and version chains. Nothing in 4.3 touches storage.
- **Phase 5.3** — every SeqScan and IndexScan in the Volcano executor makes a visibility call per tuple against the snapshot in the txn context. The visibility API you freeze here is the one the scan path hammers.
- **Phase 6.3** — the isolation-level ablation and the long-reader bloat numbers in the benchmark suite come straight from this phase's machinery and its vacuum counters.
- The **2.4 skiplist** earns its keep here: it is the in-memory index of version chains keyed by RID, sized in that phase for exactly this load.
- The **2.2 epoch-based reclamation** discipline returns: unlinking versions while readers traverse is the second time chronos faces "free memory someone may still be reading."

### What good looks like

- The visibility decision is a pure function of (version metadata, snapshot) — no locks taken, no global state consulted beyond the snapshot itself. You can state the rule in two sentences at a whiteboard.
- You can answer without running code: *"T1 begins, T2 begins, T2 updates row R and commits, T1 reads R — what does T1 see and why?"* and *"Both update R — who fails, and at what moment do they learn?"*
- Readers never block writers and writers never block readers — the harness measures this (a long reader running while a writer commits thousands of updates; neither stalls).
- Write skew is **reproducible on demand**. If your SI prevents it, you built something stronger and mislabeled it.
- After quiescing readers and running vacuum, version count equals live-tuple count, every time, under ASan.
- One documented choice: first-committer-wins vs first-updater-wins, with the test that distinguishes them.

### Why this is the shape of the deliverable

Visibility bugs are the silent kind: a read that returns the *wrong version* still returns a plausible tuple, and almost every test passes. So the artifact is a library plus an **offline replay oracle**: every read logs `(RID, snapshot, version-id returned)`, every commit logs its write set and commit timestamp, and a standalone checker replays the commits in timestamp order into a single-threaded model, computing independently which version *should* have been visible at each logged snapshot. The anomaly canary (write skew must occur) guards the other direction — against accidentally building 2PL in MVCC clothing. The vacuum oracle closes the loop on reclamation, the bug class no in-process assert can see.

## Exam questions this phase targets (build-proven)

1. Implement SI visibility such that the offline replay oracle agrees with every one of millions of logged reads across ≥ 10 randomized concurrent seeds.
2. Implement first-committer-wins such that exactly one of two overlapping writers of the same RID commits, the loser receives `ErrorCode::WriteConflict`, and the harness's signature test distinguishes your choice from first-updater-wins.
3. Reproduce write skew under your SI on demand (the harness FAILS you if it cannot), and demonstrate vacuum reclaiming to exactly the live-tuple count after readers quiesce — including after the deliberate long-running-reader bloat scenario.

## Prerequisites — concepts this phase uses

Audience: a strong C/C++ systems programmer who has never built a database. Recognize each term and the role it plays. Do NOT look up implementations — implementing them is the build.

### Versions and chains

- **tuple version** — one historical state of a logical row: the data plus metadata identifying which transaction created it and (if superseded or deleted) which ended it. Postgres's `xmin`/`xmax` columns are the famous instance of this metadata.
- **version chain** — the linked sequence of versions of one logical row, newest-first in chronos, reachable from the RID. The durable heap tuple (Phase 1.1/1.2) is the chain head; older versions live in the in-memory version store (the 2.4 skiplist keyed by RID). A reader walks the chain until it finds the version its snapshot may see.
- **dead version** — a version no current or future snapshot can ever see. Dead ≠ old: a version is dead only relative to the set of snapshots still alive.

### Snapshots and visibility

- **snapshot** — a frozen description of "which transactions had committed as of an instant," taken at transaction BEGIN in SI and consulted for every read. Its role: it makes a transaction's entire view of the database a pure function of one moment, immune to concurrent commits.
- **visibility rule** — the predicate deciding, for (version, snapshot), whether this snapshot may see this version: created by a transaction the snapshot considers committed, and not superseded/deleted by one it considers committed. Stating it precisely is easy; getting in-progress, aborted, and own-writes cases right is the build.
- **commit timestamp / txn-id allocation** — the monotonic identities that order transactions. The role: snapshots and conflict detection are both defined in terms of this order; a non-monotonic or racy allocator silently breaks both.
- **own-writes visibility** — a transaction must see its *own* uncommitted writes, before anyone else does. The standard place visibility rules quietly go wrong.

### Conflict and anomaly vocabulary

- **write-write conflict** — two overlapping transactions (neither's commit precedes the other's snapshot) writing the same row. SI forbids both committing.
- **first-committer-wins (FCW)** — the conflict rule: whichever conflicting writer commits first survives; the other learns it lost when *it* tries to commit. The alternative, **first-updater-wins**, blocks the second writer at write time and aborts it when the first commits — the difference is *when the loser learns*, and it has an observable signature.
- **write skew** — SI's defining anomaly: two txns each read the *other's* row, each write their own, both commit; no write-write conflict exists, yet the result is non-serializable (the on-call doctors example). Named and dissected in Berenson et al., *A Critique of ANSI SQL Isolation Levels* (1995) — the paper that established SI is *not* serializable. Read at vocabulary level; this phase requires you to *exhibit* the anomaly, 4.3 to prevent it.

### Reclamation

- **GC horizon / oldest-active-snapshot** — the minimum over all live snapshots; any version invisible at and after the horizon is dead and reclaimable. The role: it is the single number that decides everything vacuum may touch.
- **vacuum** — the background reclamation pass that unlinks and frees dead versions. The Postgres name; the Postgres failure story (one long transaction pinning the horizon → unbounded bloat) is reproduced *deliberately* in this phase.
- **epoch-based reclamation** — the 2.2 scheme: defer freeing until no thread can still hold a reference from before the unlink. Returns here because readers traverse chains latch-free while vacuum unlinks.

## How you know you're aligned (the cross-check)

The harness in `test/mvcc/` is the continuous oracle, and its core is independent of your engine by construction: the **offline replay checker** shares only a log format with your code. During a run, every read appends `(RID, snapshot-id, version-id returned)` and every commit appends `(txn, write set, commit timestamp)`. Afterward, the checker — single-threaded, no chronos code linked — replays the committed write sets in commit-timestamp order into its own naive model and computes, for each logged read, the version that snapshot *must* have seen. One disagreement is a FAIL with the full history attached.

It checks **both directions**. The anomaly canary scripts a classic write-skew interleaving and requires it to **succeed in occurring** — if both txns can't commit, your "SI" is something stronger (probably accidental locking) and the row FAILS. This is the discipline that catches mislabeled implementations, which prose review never does.

The vacuum oracle: under concurrent writers, run the workload, quiesce readers, run vacuum to fixpoint, and assert version count == live-tuple count (counted by an independent heap walk). ASan covers the other half — nothing freed while reachable.

The harness sees only the public surface below. Chain layout, snapshot representation, horizon bookkeeping: yours. Build a part, run the suite, watch rows flip SKIP→PASS.

## The build, in parts (each gated independently by the harness)

### Part 1 — Identity: txn-ids, commit timestamps, snapshots 🔨
Monotonic txn-id/timestamp allocation extending the 4.1 `TransactionManager`; snapshot capture at BEGIN; the snapshot is **per-transaction, taken once** and used for every subsequent read. **Harness rows:** monotonicity under hammer threads; snapshot stability (a txn's reads don't change when others commit mid-txn — this row also catches accidental per-statement re-snapshotting).

### Part 2 — Version chains and SI reads 🔨
Per-tuple newest-first chains: durable heap tuple as head (1.1/1.2), older versions in the 2.4 skiplist keyed by RID; the visibility predicate; own-writes visibility; reads that never take logical locks. **Harness rows:** the replay-oracle run at small scale; own-writes; in-progress and aborted-version invisibility; the no-blocking measurement (long reader vs committing writer, neither stalls).

### Part 3 — Write-conflict detection and finalization 🔨
First-committer-wins (or first-updater-wins — your documented choice) for overlapping writers of the same RID; commit finalizes the txn's versions as committed, abort makes them permanently invisible and unlinkable. **Harness rows:** exactly-one-survivor under N concurrent updaters of one row; the FCW-vs-FUW signature test (the harness probes *when* the loser learns and matches it against your documented choice); the **write-skew canary** (must occur); the full-scale replay-oracle soak.

### Part 4 — Vacuum and the horizon 🔨
The oldest-active-snapshot horizon; a vacuum pass that unlinks and reclaims dead versions while readers traverse (the 2.2 epoch discipline, second appearance); the **deliberate bloat demonstration**: one long-running reader pins the horizon while a writer churns — version count grows without bound, measured and graphed; reader exits, vacuum reclaims, count returns to live. **Harness rows:** reclaim-to-exact-count after quiesce; no-reclaim-past-horizon (a version visible to any live snapshot survives vacuum); traversal-during-vacuum soak under ASan + TSAN; the bloat curve emitted for RESULTS.md.

### Part 5 — The harness as a published artifact 🔨
Every row green across ASan/UBSan and TSAN builds. Write `RESULTS.md` from `templates/RESULTS.md`: the table, the bloat curve with the Postgres-story paragraph, your FCW/FUW choice and its observable signature, and the memory-overhead-per-version number. Publish so a stranger can rerun.

## API contract (what the harness links against)

All fallible calls return `Result<T>`. Everything below is the **public surface**. PRIVATE INTERNALS — version record layout, chain link representation, snapshot encoding, horizon bookkeeping, epoch machinery — are deliberately unspecified; designing them is the work.

### Snapshot acquisition (extends 4.1 `TransactionManager`)

```cpp
auto begin() -> Result<TxnId>;          // now also captures the txn's snapshot
auto snapshot_of(TxnId txn) -> Result<SnapshotId>;   // stable for the txn's lifetime
auto oldest_active_snapshot() -> SnapshotId;          // the GC horizon
```

- **What it does:** `begin` captures the snapshot at BEGIN; `snapshot_of` exposes it (the harness logs it per read); `oldest_active_snapshot` is the min over all live txns' snapshots.
- **Why it exists:** the snapshot is the transaction's entire worldview under SI; the horizon is the single input vacuum is allowed to act on.
- **Side-effect requirement:** the snapshot returned for a txn never changes across its lifetime. When a txn ends (commit or abort), it stops contributing to the horizon — promptly, or the bloat row will show your forgotten-session bug.
- **Critical contract details:** thread-safe; `oldest_active_snapshot` with no active txns returns a value that classifies every committed version as potentially reclaimable.

### `VersionStore`

```cpp
auto read(TxnId txn, Rid rid) -> Result<std::optional<TupleView>>;
auto insert(TxnId txn, Rid rid, TupleData tuple) -> Result<void>;
auto update(TxnId txn, Rid rid, TupleData tuple) -> Result<void>;
auto remove(TxnId txn, Rid rid) -> Result<void>;
```

- **What it does:** `read` walks the version chain at `rid` and returns the version visible to `txn`'s snapshot (its own uncommitted write counts), or `nullopt` if no visible version exists. Writes create new uncommitted versions; they never destroy old ones in place.
- **Why it exists:** this is the storage call surface 5.3's executor sits on — every scan is `read` in a loop, every UPDATE is `read` + `update`.
- **Side-effect requirement:** after a write, the txn's own subsequent `read` sees it; **no other txn's read does** until commit. `read` acquires no logical locks and blocks on no writer.
- **Critical contract details:** an overlapping write-write conflict surfaces as `ErrorCode::WriteConflict` — at write time (FUW) or at commit (FCW), per your documented choice, consistently. `TupleView` is valid only until the calling txn ends (the epoch contract). Operations on an ended txn → error, never UB.

### Commit/abort finalization & vacuum

```cpp
// commit(txn) / abort(txn) — 4.1 signatures, now also finalize the txn's versions.
auto vacuum() -> Result<uint64_t>;        // versions reclaimed this pass
auto version_count() -> uint64_t;         // total versions currently held (live + dead)
```

- **What it does:** commit stamps the txn's versions committed at its commit timestamp (atomically with respect to visibility — no snapshot may observe a half-committed write set); abort makes them permanently invisible. `vacuum` reclaims versions dead relative to the current horizon; `version_count` is the harness's bloat probe.
- **Why it exists:** finalization is where SI's "commit is atomic and instantaneous" illusion is manufactured; vacuum is why the illusion doesn't eat all memory.
- **Side-effect requirement:** under FCW, commit performs conflict detection *before* the WAL commit record is written — a loser must not be durable. Vacuum never frees a version visible to any live snapshot, and never frees memory a concurrent traversal can still reach (epoch discipline).
- **Critical contract details:** `vacuum` is safe to run concurrently with full read/write load; repeated calls converge (idempotent at fixpoint). Commit of a conflict loser returns `ErrorCode::WriteConflict` and the txn is aborted.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS — replay oracle (≥ 10 seeds, millions of reads), own-writes, no-blocking measurement, exactly-one-survivor, FCW/FUW signature, **write-skew canary occurs**, vacuum reclaim-to-exact-count, no-reclaim-past-horizon — under ASan/UBSan **and** TSAN.
2. The bloat demonstration produced, graphed, and reversed (reader exits → reclaim to live count).
3. Memory overhead per version reported; FCW vs FUW choice documented with its signature test.
4. `RESULTS.md` published; a stranger can rerun.

## Principal-engineer traps (no solutions)

- **The snapshot is taken at BEGIN and used for every statement.** Re-snapshotting per statement quietly gives you Read Committed, and most tests won't notice — the snapshot-stability row exists because this is the most common mislabeled-isolation bug in homegrown MVCC.
- **FCW vs FUW differ in *when the loser learns*** — block-then-abort versus sail-along-then-abort-at-commit. Pick one, document it, and test for the *other's* signature; an implementation that exhibits both depending on timing has a race, not a policy.
- **The horizon is a min over ALL active snapshots.** One idle session you forgot to end — a harness thread, a debugging REPL, 6.1's future leaked connection — pins every dead version in the database forever. The bloat demo is this trap made visible on purpose.
- **Traversing a chain while vacuum unlinks it is a reclamation race**, and it will pass every single-threaded test. This is 2.2's epoch problem again at tuple granularity; if your TSAN soak is clean but ASan occasionally screams use-after-free, you skipped the discipline.
- **Commit finalization must be atomic with respect to visibility.** If a snapshot can observe half of a txn's write set as committed, the replay oracle will eventually catch it — at the worst possible seed.
- **Aborted versions are not "dead later," they are invisible *now*** — and they still occupy chain links until vacuum. Conflating "aborted" with "reclaimable immediately" frees memory a concurrent reader is walking.

## What you hand back for review

1. Implementation + the full harness table (all sanitizer builds) + the bloat curve + the per-version overhead number + your FCW/FUW writeup from RESULTS.md.
2. One sentence per trap above: did it bite you, and how did you resolve it?

Review is principal-engineer style: the interview attack on your visibility rule (the T1/T2 whiteboard scenarios from "What good looks like"), the both-directions check (did write skew really occur?), and the seam check — is the read path actually lock-free enough for 5.3's scan loop? Then we advance to Phase 4.3.

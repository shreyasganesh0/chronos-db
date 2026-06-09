# Tier III · Phase 3.3 — ARIES recovery (scaffold)

**Deliverable of this phase:** full three-pass ARIES restart recovery — analysis, redo (repeating history), undo with CLRs — restartable at any instant, proven by a torture loop that SIGKILLs the engine at hundreds of randomized crash points (including *during recovery itself*) and verifies the surviving database against an independent shadow model; plus a recovery-internals writeup including the documented limits of physiological undo.
**What you'll own afterward:** the sentence "kill -9 at any instant is safe" stops being a claim and becomes a theorem you proved by torture — and ARIES, the most-cited recovery algorithm in database history, becomes something you can re-derive at a whiteboard from its three invariants rather than recite as a checklist.
**Calibration:** 🔨 (with one 📖 component: logical undo / nested top actions)
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is the algorithm that turns the 3.1 log into a durability guarantee.** Until now the WAL is a meticulously ordered, checksummed byte stream that *nobody reads*. This phase is the reader: at startup, walk the log and bring the database to the exact state where every transaction whose commit record is durable is fully present, and every transaction without one has vanished without trace — no matter when the previous process died, and no matter how many times recovery itself is interrupted and restarted.

It is **not a backup/restore system** — there is no second copy of the data, no archive, no point-in-time anything; recovery repairs the *one* database in place from its own log. It is **not replication** — no second node, no shipping; those are explicitly future-work notes in 6.4. And it is not a "recovery feature checklist" to be ticked through: an implementation that handles every case you thought of is worth nothing here, because the cases that matter are the ones a random SIGKILL finds and you didn't think of. The artifact is not the code; the artifact is the *evidence* — hundreds of randomized crash points, every one recovered, verified against an oracle your engine has never seen.

The right artifact test: **"kill -9 at any instant is safe — proven by torture, not by a feature list."**

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

Adversarial reasoning about crash timing: the ability to take any instant in any interleaving — mid-append, mid-eviction, mid-commit-fsync, mid-*undo* — and trace what's on disk, what recovery will conclude, and why the conclusion is right. Engineers with this skill read a "database corrupt after power loss" postmortem and name the violated invariant in one pass; engineers without it add fsyncs until the bug hides.

### Why "from scratch" is the right call here

ARIES is the single highest-value from-scratch build in this curriculum. Treat it as a black box and:

- **Repeating history will never feel necessary.** Every first-time implementer "optimizes" redo to skip losers — it's *obviously* wasted work — and gets a database where undo operates on pages that don't reflect the history being undone. You have to watch your own intuition be wrong here; reading about it doesn't inoculate.
- **CLRs will look like bookkeeping** instead of what they are: the mechanism that makes undo itself crash-safe and bounds rollback work even under repeated crashes. You only respect undoNextLSN after you've understood the double-crash it prevents.
- **You'll never trust your own torture rig.** 6.2 generalizes this phase's crash orchestration to full-SQL fault injection; the person who built the original oracle knows exactly what it does and doesn't prove.
- **Every later phase leans on this.** 4.1's abort path *is* this phase's undo machinery running without a crash. 5.1's "catalog survives kill -9 mid-CREATE-TABLE" is this phase applied to system tables. 6.4's capstone demo is literally this phase performed live.
- **The interview canon runs through here.** "Walk me through ARIES" is the systems-interview question for database roles; "why repeat history?" and "what's in a CLR?" separate people who built it from people who read it.

### What carries forward to later tiers

- **Phase 4.1** — transaction rollback reuses the prevLSN walk + CLR machinery verbatim; abort is "recovery for one transaction, no crash required."
- **Phase 4.2** — MVCC version chains must themselves recover; the WAL/recovery discipline from this phase is what makes the version store durable rather than rebuilt-on-start.
- **Phase 5.1** — the catalog's crash-safety claim is this phase's guarantee, inherited by storing system tables as ordinary logged heap pages.
- **Phase 6.2** — the torture rig generalizes this phase's child/parent SIGKILL orchestration and shadow-model verification to full-SQL workloads with injected I/O faults.
- **Phase 6.4** — the capstone's centerpiece (kill -9 mid-TPC-C, timed recovery, consistency green) is this phase performed under benchmark load.
- **The 📖 component** — logical undo / nested top actions — is the bridge to how real engines recover B+Tree structure modifications; the writeup you produce here is the design note you'd hand a team adding index logging to chronos later.

### What good looks like

- Recovery is the *only* startup path — there is no "clean shutdown" fast path that skips it, because an empty-work recovery on a clean log is cheap and a separate path is an untested path.
- Recovery is idempotent and re-entrant by construction, and you can say *why* in one sentence per pass (analysis writes nothing; redo is pageLSN-gated; undo writes CLRs that record their own progress).
- The torture loop runs unattended for hundreds of seeded crash points — including crashes *during* recovery — with zero shadow-model divergences, and a failure reproduces from its seed.
- You can answer cold, from your build: *"why must redo reapply a loser's update that undo is about to reverse?"*, *"a CLR is encountered during redo — what happens?"*, *"the client never got its commit ack but the commit record is in the log — committed or not, and whose definition wins?"*
- The writeup names the exact operation where chronos's physiological undo would corrupt a B+Tree, and how a nested top action fixes it — precisely enough that a reader could implement NTAs from it.

### Why this is the shape of the deliverable

A torture rig with an independent shadow model, because correctness here is a property of *all possible crash instants*, and no enumeration of hand-written test cases covers them — only randomized adversarial sampling at scale does. The oracle must live outside the engine (a parent process holding its own model of acknowledged commits) because recovery verified by the engine's own read path proves only self-consistency, and a self-consistently wrong database is the failure mode that ends careers. Hence: child process, ack-after-fsync protocol, SIGKILL, recover, compare against the shadow — hundreds of times, seeded, replayable.

## Exam questions this phase targets (build-proven)

1. Implement three-pass ARIES such that the torture row PASSes: across ≥ 300 randomized SIGKILL points, every acknowledged-committed transaction is fully present after recovery and every unacknowledged one is absent, verified against the harness's shadow model.
2. Implement redo as *repeat history* — losers included, pageLSN-gated — such that the harness's loser-redo probe row PASSes (a workload constructed so that skipping loser redo leaves undo facing a page state it cannot correctly reverse).
3. Implement CLRs with undoNextLSN such that the double-crash row PASSes: SIGKILL delivered *during undo*, recovery rerun, final state identical and no record undone twice (waldump shows the CLR chain proving it).
4. Prove idempotence: run recovery, snapshot the files, run recovery again, byte-identical.
5. 📖 Write the logical-undo note: the precise B+Tree scenario where physiological undo corrupts, and how a nested top action prevents it.

## Prerequisites — concepts this phase uses

Vocabulary and roles only; the algorithm is the build. **Primary source — read it for real this time:** Mohan, Haderle, Lindsay, Pirahesh, Schwarz, *ARIES: A Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging*, ACM TODS 17(1), 1992 — §3 (goals), §6 (restart processing), and the §10 rationale for repeating history. Read it for *invariants and why*, not as a recipe; your structures are your own. Practitioner orientation: PostgreSQL *Reliability and the Write-Ahead Log* (https://www.postgresql.org/docs/current/wal.html — note Postgres redoes-only because it undoes via MVCC, a contrast worth being able to articulate); InnoDB recovery (https://dev.mysql.com/doc/refman/8.4/en/innodb-recovery.html — a true ARIES-family redo+undo).

### The three passes

- **analysis pass** — first pass: start from the master record (3.2), seed ATT/DPT from the checkpoint, scan forward to the log's durable end, ending with the truth at crash time: who was active (losers), which pages might be stale, and where redo must begin (min recLSN). Role: figure out what happened; touches no data pages. You built its core as 3.2's `RebuildTables`.
- **redo pass** — second pass: from min recLSN forward, reapply logged changes to bring pages to their exact state at the crash — **including changes made by transactions that will be undone moments later**. Role: reconstruct history exactly, so that undo operates on the same page states the original execution produced. Gated per-record by pageLSN for idempotence.
- **undo pass** — third pass: walk each loser's update chain backward via prevLSN (3.1's chains), reversing each update and logging a CLR for it, until every loser's effects are gone. Role: erase the uncommitted.
- **repeating history** — ARIES's signature idea and the one your intuition will fight: redo restores *everything that happened*, not just what deserves to survive. Why it's load-bearing for fine-granularity locking: with multiple transactions touching one page, the page's crash-time state is a weave of winner and loser updates, and undoing a loser's slot-level change is only well-defined against the page state that change actually produced. ARIES §10.1 is the canonical defense.

### Winners, losers, and the commit point

- **loser transaction** — active (no commit record in the durable log) at crash time; analysis identifies them; undo erases them. **Winner** — commit record durable; survives untouched.
- **the commit point** — a transaction *is committed* iff its commit record is in the durable log. Not "iff the client heard back." A crash between commit-fsync and the ack creates a transaction that is committed by the log's definition but unacknowledged by the client's — and the log's definition wins. Role: this asymmetry is exactly what the harness's shadow-model protocol must (and does) handle; see the cross-check section.

### Crash-safe undo

- **CLR (compensation log record)** — a log record written *for each undo step*, describing the reversal that undo just performed. CLRs are **redo-only**: a CLR is never itself undone (undoing undo would un-erase a loser). Role: makes undo's progress durable, so a crash during undo doesn't lose the work.
- **undoNextLSN** — the field that makes CLRs more than bookkeeping: each CLR records *which record to undo next* (the prevLSN of the record just compensated). After a crash mid-undo, recovery resumes each loser at its latest CLR's undoNextLSN — skipping everything already compensated. Role: no record is ever undone twice, and total undo work is bounded no matter how many crashes pile up.
- **re-entrancy / restartability** — the property that recovery itself can be killed at any instant and rerun from scratch, converging to the same final state. Not a separate mechanism — the *consequence* of the three passes' individual properties, which is why the harness attacks it directly rather than trusting the argument.

### The 📖 boundary: logical undo and nested top actions

- **physiological undo** — reversing an update as "the inverse change, on the *same page*." Chronos's scope. Sufficient while an update's undo target is wherever the update happened.
- **logical undo** — reversing an update by *re-executing its inverse through the engine* ("delete key k from the index"), wherever the data now lives. Required when concurrent structure changes move data between log time and undo time.
- **SMO (structure modification operation)** — a multi-page physical reorganization, e.g. a B+Tree leaf split: correct as a unit, meaningless half-done.
- **nested top action (NTA)** — ARIES's device (§10.8) for making an SMO permanent-once-complete regardless of its enclosing transaction's fate: a sub-action closed off (via a dummy CLR bridging undoNextLSN over it) so that undo of the enclosing transaction skips the SMO entirely and compensates logically around it. Role in this phase: understand it, **don't build it** — chronos Tier II indexes are rebuilt/not-logged at this tier's scope, and your writeup documents exactly where physiological undo would corrupt and how NTAs fix it. That document is the deliverable for this concept.

## How you know you're aligned (the cross-check)

The harness in `test/aries_recovery/` is the continuous oracle: build a pass, run it, rows flip SKIP → PASS. It sees only the public surface below — `Recover()`, the report counters, the data via ordinary reads, the log via `waldump` — never your pass internals.

The canonical oracle — the one this whole tier has been building toward — works like this. The harness runs a **deterministic seeded workload in a child process**: a stream of small transactions mutating a known keyspace. The child reports each transaction to the parent over a pipe **only after its commit-fsync has returned** — the ack is downstream of durability, never upstream. The parent maintains a **shadow model**: an in-memory map holding the effects of exactly the acknowledged transactions. At a random instant the parent SIGKILLs the child mid-flight, runs recovery on the carcass, reads back the full database state, and compares: every acknowledged transaction's effects present and exact, no unacknowledged transaction's effects present — with one carefully-handled exception. A transaction killed *between* commit-fsync and ack is legitimately committed (the log's definition) yet absent from the shadow; the harness treats these in-flight-at-kill transactions as *may-be-present*: allowed fully present or fully absent, never partial. Everything older is strict. The shadow model is independent by construction — a hash map in another process that has never seen a page, a latch, or an LSN; agreement with it cannot be self-consistency.

Three more oracles attack recovery itself. **Idempotence:** recover, snapshot every file byte-for-byte, recover again, byte-compare — any divergence means some pass writes when it shouldn't. **Double-crash:** SIGKILL delivered at a random instant *during* recovery (undo especially), recover again, compare against the same shadow — undoNextLSN's reason to exist, tested directly. **Loser-redo probe:** a workload engineered so that a redo pass which skips losers leaves undo facing page states it cannot correctly reverse — the row that catches the most common ARIES misimplementation by its observable consequence. All randomized rows are seeded and replayable; a red row prints its seed.

## The build, in parts (each gated independently by the harness)

### Part 1 — Analysis 🔨
From the master record: seed ATT/DPT from the checkpoint (3.2's `RebuildTables`, promoted), scan to the durable frontier, emit losers and the redo start point. Touches no data pages; safe to run on any crashed directory.
**Harness rows:** analysis-vs-full-scan equivalence on crashed directories (the 3.2 oracle, now post-crash); loser identification across commit-boundary crash timings; correct redo-start derivation.

### Part 2 — Redo: repeat history 🔨
From min recLSN forward, reapply every relevant update — winners *and losers* — to pages whose pageLSN says the change is missing; stamp as you apply. DPT consulted to skip irrelevant pages; pageLSN consulted to skip applied records.
**Harness rows:** post-redo pageLSN exactness; the idempotence row (recover-snapshot-recover-compare) starts passing for redo-only workloads; the loser-redo probe row.

### Part 3 — Undo: losers, prevLSN, CLRs 🔨
Walk each loser backward via prevLSN, reverse each update physiologically, log a CLR with undoNextLSN per step, write an end record per finished loser. CLRs go through the 3.1 `Append` path like any record; `docs/wal_format.md` grows the CLR type and `waldump` parses it — the chain must be *visible* in waldump output.
**Harness rows:** the full canonical torture row goes green here (shadow-model comparison across randomized crash points); waldump-visible CLR chains with correct undoNextLSN linkage; no-partial-loser row.

### Part 4 — Re-entrancy: crash-during-recovery 🔨
No new machinery if Parts 1–3 kept their invariants — this part *proves* it. Recovery becomes the unconditional startup path; the harness now SIGKILLs during recovery itself, repeatedly, then lets it finish.
**Harness rows:** double-crash row (kill during undo, recover, same final state, no double-undo — verified via waldump's CLR chains); kill-during-redo row; N-fold repeated-crash row (crash recovery k times in a row, converge identically); full idempotence row.

### Part 5 — The harness as a published artifact + the recovery writeup 🔨/📖
Every row green at full torture counts. Write `RESULTS.md` from `templates/RESULTS.md`: the table, total crash points survived, seeds, and the recovery-internals writeup — your three passes in your own words, the invariant each rests on, the re-run of 3.2's interval study with real wall-clock recovery times, and the 📖 logical-undo note: the precise B+Tree-split scenario where physiological undo corrupts, and how an NTA closes it. Publish; a stranger reruns the torture from seeds.

## API contract (what the harness links against)

Namespace `chronos::recovery`. **PRIVATE INTERNALS, deliberately unspecified:** pass structure and data structures, how redo batches page fetches through the 1.3 pool, undo scheduling across losers, CLR payload encoding beyond what `docs/wal_format.md` freezes for waldump. The surface is deliberately tiny — recovery is a thing that *happens*, observable almost entirely through its effects.

### `Recover`

```cpp
struct RecoveryReport {
  Lsn   master_lsn;            // checkpoint recovery started from (kInvalidLsn if none)
  Lsn   durable_end_lsn;       // the valid-prefix frontier analysis stopped at
  Lsn   redo_start_lsn;        // min recLSN derived by analysis
  uint64_t records_scanned;
  uint64_t redo_applied;
  uint64_t redo_skipped_by_pagelsn;
  uint64_t loser_txns;
  uint64_t clrs_written;
};

auto Recover(const std::filesystem::path& db_dir) -> Result<RecoveryReport>;
```

- **What it does:** runs complete three-pass ARIES restart recovery on a database directory, in place, and returns an accounting of what each pass did. The **only** way the engine opens a database — clean shutdown included (where it degenerates to a cheap scan with zero redo/undo, and the harness checks exactly that).
- **Why it exists:** the durability guarantee, cashed in. Every engine has this exact moment — InnoDB's startup recovery, Postgres's REDO — between "process started" and "ready for connections."
- **Side-effect / durability requirement:** on return, the database state on disk reflects exactly the committed-by-log-definition transactions; all recovery writes (redone pages, CLRs, end records) are durable; the engine is ready for new transactions with LSN assignment continuing above `durable_end_lsn` (3.1's no-reuse contract — segment epochs and the post-recovery log position must cooperate). Running `Recover` on its own output changes **nothing** — the byte-compare row holds it to that literally.
- **Critical contract details:** single-threaded entry (no concurrent engine activity during recovery); must tolerate being SIGKILLed at any interior instant and rerun; `Result<T>` on unreadable log/master with the directory left no worse than found; the report counters are contract — the harness asserts on them (e.g. `redo_skipped_by_pagelsn` > 0 on a re-run proves the gate is live, `clrs_written` cross-checks waldump's CLR count).

### Commit acknowledgment (re-affirmed contract on `wal::WalManager::Commit`)

- **What it does:** returns only after the commit record is durable — restated here because it is the *load-bearing half of the oracle protocol*: the harness child acks the parent strictly after `Commit` returns.
- **Why it exists:** the entire meaning of "acknowledged-committed" in the torture verdict rests on this ordering. If 3.1 ever lets `Commit` return early, this phase's torture is the place it gets caught — as a shadow-model divergence, not a hang.
- **Critical contract details:** the killed-between-fsync-and-ack window is *expected* and handled by the harness's may-be-present rule; what is never tolerated is the inverse (acked but absent after recovery) or a partial transaction in either direction.

### CLR visibility (contract on `waldump` / `docs/wal_format.md`)

- **What it does:** the format doc gains the CLR record type (with its undoNextLSN field) and per-loser end records; `waldump` — still zero shared code — renders them, and the harness reads undo's progress *through waldump alone*.
- **Why it exists:** the double-crash row needs to *see* that resumed undo continued from undoNextLSN rather than re-compensating — and the only honest window into the log is the independent tool.
- **Critical contract details:** undoNextLSN must be printed; chain linkage (CLR → compensated record → next-to-undo) must be reconstructible from waldump output alone.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS in `test/aries_recovery/`; ASan/UBSan builds green.
2. Canonical torture: ≥ 300 seeded randomized crash points, zero shadow-model divergences; double-crash and repeated-crash rows green at ≥ 100 crash-during-recovery points; loser-redo probe green.
3. Idempotence: recover → snapshot → recover → byte-identical, on every torture carcass class.
4. `waldump` parses CLRs and end records; the format doc is current; recovery is the unconditional startup path (no clean-shutdown bypass exists).
5. `RESULTS.md` published with torture counts, seeds, wall-clock recovery-time study (3.2's curve, re-measured for real), and the logical-undo/NTA writeup; a stranger can rerun from seeds.

## Principal-engineer traps (no solutions)

- **Skipping loser redo is THE classic ARIES misimplementation.** It feels like an obvious optimization — why reapply what undo will erase? — and it survives every simple test, because single-transaction-per-page workloads never expose it. It breaks only when winner and loser updates interleave on one page. The probe row is engineered for exactly that interleaving; when your intuition argues with the spec here, the spec is right, and ARIES §10.1 explains why. Do the argument with yourself *before* the row fails.
- **CLRs without undoNextLSN are decoration.** Writing CLRs but resuming undo from the loser's lastLSN (or re-deriving "what's left" any other way) double-undoes after a crash mid-undo — physiologically, that's applying an inverse twice, i.e. corruption. The double-crash row exists for this one field. Forget it and the row will fail in a way that looks like a redo bug; it isn't.
- **CLRs must never be undone.** If your undo pass can encounter a CLR and try to reverse it, repeated crashes oscillate instead of converging. The repeated-crash row (k crashes in a row) is the detector.
- **The commit point is the log's, not the client's.** A commit record in the durable log is a committed transaction even if the ack never left the building. If your shadow-model reasoning — or worse, your recovery — defines committed as "the client heard back," the may-be-present window becomes a divergence factory. Get the definition straight in your head before reading torture failures, or you'll "fix" the engine to match the wrong oracle.
- **Recovery that writes without re-stamping breaks its own idempotence.** Every redo application and every undo compensation moves the affected page's pageLSN forward — miss one path and the recover-twice byte-compare catches a page that changes on the second run. The byte-compare is merciless on purpose; it also catches sloppy nondeterminism you didn't know you had.
- **Physiological undo has a cliff — know exactly where.** Undoing a B+Tree insert *after the leaf has split* means the inverse-on-the-same-page targets a page the key no longer lives on. Chronos scopes this out (Tier II indexes aren't WAL-logged at this tier), but the writeup must pin the failure precisely — the page, the moment, the corruption — and show how an NTA plus logical undo closes it. Vagueness here means the 📖 component didn't land.
- **Fresh LSNs after recovery.** If post-recovery LSN assignment can collide with pre-crash history (or with stale recycled segments), the *next* crash recovers the wrong timeline. 3.1's epoch design and this phase's restart position must agree; the torture loop's multi-crash lineages will find it if they don't.

## What you hand back for review

1. The recovery implementation, the full harness table, torture counts + seeds, the wall-clock recovery-time study, and the recovery-internals writeup with the logical-undo/NTA note.
2. One sentence per trap: did it bite, how resolved.

Review will be principal-engineer style: the interview attack runs straight down the traps ("show me, in your waldump output, the CLR chain from the double-crash run — prove nothing was compensated twice"; "delete your clean-shutdown assumption: where exactly does your code rely on the log ending with end records?"), plus an audit of whether every durability claim traces to a torture row rather than a clean-exit test. Then Tier III is closed — and 4.1 gets to reuse your undo machinery as its abort path.

*Start Part 1 when ready — analysis is 3.2's `RebuildTables` finally meeting a real corpse.*

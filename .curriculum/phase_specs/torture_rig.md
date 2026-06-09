# Tier VI · Phase 6.2 — Torture rig: crash injection & deterministic replay (scaffold)

**Deliverable of this phase:** `chronos-torture` — one command that runs N seeded fault schedules against full-SQL workloads through a fault-injecting shim at the `chronos::io` seam (torn writes, dropped writes / fsync lies, short reads, EIO) plus SIGKILL orchestration, verifies every run against the composed oracles, and reports any violation with a seed that replays it exactly.
**What you'll own afterward:** an adversarial physics simulator for your storage stack — the ability to ask "what if the disk lied right *here*" about any write chronos ever issues, and to hand yourself (or a reviewer) a one-seed reproducer instead of a war story.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is an adversarial physics simulator for the storage stack — not a test suite, and not a fuzzer for parse errors.** Phase 3.3's rig threw SIGKILL at a synthetic WAL workload; this phase generalizes it in two directions at once. First, the *workload* becomes full SQL through the 5.3 executor (and optionally through the 6.1 server), so the faults land under realistic page/WAL traffic. Second — and this is the new substance — the *fault model* moves below the syscall: a shim implementing the `chronos::io` interface (the seam Phase 0.3 deliberately left open) injects the failures physical crash testing rarely produces on demand: a write torn at a sector boundary, an fsync that returned success for data that never persisted, a read that comes back short, a spurious `EIO`.

It is not a fuzzer for inputs — malformed SQL and hostile bytes were Tier V's problem. The SQL here is *valid*; what's hostile is the storage underneath it. And it is not a grab-bag of test cases: the rig's value is the **seed**. Every fault schedule is a deterministic function of a seed; every red run ends in `--replay <seed>` reproducing the same faults at the same injection points. Your fuzzing-research background is exactly the right instinct here — this is coverage-guided thinking applied to crash states, with determinism as the non-negotiable invariant. Lean into it.

You're done with the framing when you can say: the deliverable is *a seeded fault simulator whose every verdict is replayable*, not *more tests*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
You'll be the kind of engineer who treats "the disk did something weird" as a programmable input rather than an act of God — who can take a durability claim ("acked commits survive any crash") and convert it into a fault schedule that attacks the exact write ordering the claim depends on, then minimize a red run to a three-operation reproducer. This is the discipline behind every serious storage system's CI (SQLite's test harness, FoundationDB's simulation): the bugs that matter live in states you must *construct*, because nature won't construct them for you on schedule.

### Why "from scratch" is the right call here
Use someone else's fault injector (dm-flakey, CharybdeFS, a libc interposer) and the following stay opaque:

- Why injecting at *your own* `io::` interface beats injecting at the kernel boundary: the shim sees chronos's intent (which page, which WAL segment, which fsync barrier) and can target the schedule at semantically interesting points — the write *after* the commit record, the fsync *between* WAL and master record.
- Why determinism is an architecture, not a flag — what in your engine (thread scheduling, io_uring completion order, group-commit batching) breaks replay, and which oracle designs survive that.
- Why "fsync lied" is a distinct bug class from "crash before fsync": the engine *proceeded* on a durability promise that was false, so its in-memory state and the disk have already diverged before the crash. ARIES-style torture that only kills processes never reaches these states.
- Why a torn write that your CRC layer *silently accepts* is a worse finding than a crash — and how you'd ever notice, if the rig didn't insist on detection.
- Why a flaky red without a seed is a project-killing event in integration week, and what rig design choices prevent it.

### What carries forward to later tiers
- **Phase 6.3** reuses the rig's workload drivers and verdict machinery: TPC-C consistency checks after benchmark runs are the same "run, then interrogate the corpse" pattern.
- **Phase 6.4**'s capstone gate is this rig running 6.3's workload through 6.1's server — the composed pipeline. The demo's kill -9 step is one schedule from this phase, promoted to theater.
- The build plan's definition of done requires an overnight rig run with zero *unexplained* reds — the triage discipline (every red ends in a minimized reproducer or a documented expected-abort) is established here and enforced through the end.
- The acked-committed shadow model (3.3) and the SQLite differential (5.3) get their final, hardest workout here; any laxity they tolerated in their home phases surfaces now.

### What good looks like
- `chronos-torture --seed 42` run twice produces byte-identical fault schedules and identical verdicts. Run on another machine: same schedules, same verdicts.
- The fault shim is a drop-in implementation of `chronos::io`'s interface — the engine cannot tell (and must not be able to ask) whether it's running on the shim. Zero `#ifdef TORTURE` in engine code.
- Every injected torn or dropped write is either *detected* by the CRC/recovery path or *masked* by a documented mechanism (e.g., the page was rewritten later in the schedule and the verifier proves it); a silent read of injected corruption is reported as a violation in its own right.
- You can answer cold: *"Your rig found a red at seed 7781 last night. Walk me through the next hour."* — and the answer is a procedure (replay, bisect the schedule, minimize, classify), not improvisation.
- The expected-abort ledger is short and every entry has a one-line justification; "flaky, reran green" appears nowhere.

### Why this is the shape of the deliverable
Crash-safety bugs are reachable-state bugs: the code is correct for every state you thought of, and the bug lives in a state you didn't. A *tool* that mass-produces hostile states — deterministically, so each one is a permanent regression test — is the only artifact shape that matches the problem. Hence one command, seeded schedules, replayability as a hard contract, and oracles that judge consequences (which committed txns survived) rather than internals.

## Exam questions this phase targets (build-proven)

1. Implement a fault-injecting `chronos::io` shim such that the harness's determinism row passes: identical seed ⇒ identical injection trace ⇒ identical verdict, across runs and machines.
2. Implement the fsync-lie fault (fsync acks; the writes it claimed to persist vanish at the next crash) and demonstrate via the rig that chronos either survives it correctly or detects the loss — never silently serves data inconsistent with the acked-committed set.
3. Run N ≥ 500 seeded schedules of mixed torn/dropped/short/EIO faults plus SIGKILL over full-SQL workloads with the composed oracles green — and produce, for one deliberately-introduced engine bug, the minimized seed that catches it.

## Prerequisites — concepts this phase uses

Vocabulary, not the build. Recognize the role each plays; designing the rig is the phase.

### the fault model
- **torn write** — a multi-sector write of which only a prefix (or arbitrary sector subset) persisted; the unit of atomicity is the sector, not your 4 KB page. Phase 0.1 demonstrated it physically; here you *manufacture* it on demand. The classic treatment is in the ARIES paper's media-failure discussion and PostgreSQL's full-page-writes rationale.
- **dropped write / fsync lie** — `fsync` returns success but the data never reaches stable storage, discovered only after a crash. Real-world precedents: consumer drives with volatile write caches, and the 2018 "fsyncgate" PostgreSQL incident (errors reported once, then forgotten). Role here: the fault that distinguishes "crash-safe" from "crash-safe assuming the disk is honest."
- **short read** — a read completing with fewer bytes than requested without error. Phase 0.3's engine already resubmits; the rig verifies that promise under adversarial scheduling.
- **EIO** — the I/O error errno. The interesting question is never the errno itself but the engine's *state machine afterward*: retry, abort the txn, or refuse further writes.
- **fault schedule** — the deterministic, seed-derived plan of which I/O operations get which faults (e.g., "the 3rd fsync on the WAL fd lies; the 41st page write tears after sector 2"). The rig's unit of reproducibility.

### determinism & replay
- **seeded determinism** — all randomness (workload generation, fault placement, kill timing) flows from one PRNG seeded by the run's seed, so the entire run is a pure function of (seed, binary, initial state).
- **invariant-based oracle vs. state-equality oracle** — two ways to judge a run: assert properties that must hold under *any* legal schedule ("every acked-committed txn's effects present; no uncommitted txn's effects present") vs. assert byte/state equality with a reference execution. Under multi-threaded nondeterminism only the former is sound; equality oracles are reserved for runs a seed pins to a single-threaded schedule.
- **minimization** — shrinking a failing schedule (fewer statements, fewer faults) while preserving the failure — delta-debugging, applied to fault schedules. Your fuzzing background owns this term already.
- **expected-abort ledger** — the documented list of red-looking outcomes that are actually correct behavior (e.g., EIO during commit ⇒ txn reported aborted ⇒ its absence after recovery is *required*). Triage discipline made into an artifact.

### the oracles (all pre-existing — this phase composes them)
- **acked-committed shadow model (from Phase 3.3)** — the rig records, outside the engine, every txn whose commit was acknowledged *to the client/driver* before the fault; after recovery, all of those must be fully present and all others fully absent. The ack boundary is what makes it independent.
- **SQLite differential (from Phase 5.3)** — the same statement stream applied to SQLite; after recovery, surviving data must match SQLite's result for the acked-committed prefix. Used where the schedule pins determinism tightly enough for equality to be meaningful.
- **recovery idempotence (from Phase 3.3)** — crashing *during* recovery and re-running it must converge to the same correct state; the rig re-checks this under every fault schedule, not just clean SIGKILL.
- **CRC detection obligation (from Phases 0.1/1.2)** — every page read passes the per-page CRC; injected corruption that reaches a caller without a CRC failure is itself a violation, regardless of downstream behavior.

## How you know you're aligned (the cross-check)

The harness in `test/torture_rig/` validates the *rig* — it is the meta-layer, and it is not circular because every judgment it makes uses an oracle the engine (and the rig's injection code) cannot see:

1. **Determinism rows:** the harness runs your rig twice at fixed seeds and diffs the injection traces and verdicts; any divergence fails. This is checked *before* any correctness row is trusted — a nondeterministic rig's greens are worthless.
2. **Detection rows (the rig must catch a guilty engine):** the harness ships fault schedules targeting specific durability promises and asserts the *shape* of the outcome — e.g., under an fsync-lie schedule on the WAL, either recovery is correct or the rig reports a violation; a green verdict with silently-missing acked txns fails the harness, because the harness independently maintains its own acked-set from the driver's view.
3. **Oracle-composition rows:** N seeded schedules with the shadow model + SQLite differential + CRC-detection + recovery-idempotence checks all green, run under the harness's supervision with the harness re-deriving the acked set itself.

The harness drives only the `chronos-torture` CLI and reads its machine-parseable report; the shim's internals, schedule encoding, and orchestration are private. (Per the five-gate protocol, the harness was validated against a deliberately-lying throwaway rig before handoff — your rig inherits a meta-oracle that has already caught a fraud once.)

## The build, in parts (each gated independently by the harness)

### Part 1 — The fault shim at the `io::` seam 🔨
An implementation of the `chronos::io` interface wrapping the real 0.3 engine, consuming a seed-derived fault schedule: torn write, dropped write/fsync lie, short read, EIO, each targetable by (fd-role, operation index). Engine code unchanged and unaware. **Harness rows:** each fault type demonstrably fires (injection trace shows it; observable effect matches the fault's definition), pass-through mode is byte-transparent, determinism of the injection trace.

### Part 2 — Seeded schedules & SIGKILL orchestration 🔨
The orchestrator: generate workload + fault schedule from a seed, run chronos (library-level executor workload now; the 6.1 server is plugged in once available), kill -9 at scheduled points — including during recovery — restart, recover. **Harness rows:** seed → identical run twice, kill-during-recovery re-entrancy, schedule covering all fault types in one run.

### Part 3 — The composed verdict 🔨
Wire in the oracles: acked-committed shadow model, SQLite differential where the schedule permits equality, CRC-detection obligation, recovery idempotence. Violations reported with seed + schedule position + violated invariant, machine-parseable. **Harness rows:** the detection rows (rig catches the harness's targeted schedules), oracle-composition soak, violation-report format.

### Part 4 — Triage discipline as a feature 🔨
`--replay <seed>`, schedule minimization support (rerun with a schedule prefix/subset), and the expected-abort ledger consulted by the verdict (documented expected outcomes are reported as such, not as violations or silent greens). **Harness rows:** replay fidelity, ledger entries surfaced distinctly in the report, a minimized schedule still reproducing a harness-planted detection case.

### Part 5 — The harness as a published artifact 🔨
Every row green; then the soak: ≥ 500 seeded schedules plus one overnight run, zero unexplained reds. Write `RESULTS.md` from `templates/RESULTS.md`: the table, soak parameters, the expected-abort ledger, and one paragraph on the most interesting state the rig reached. Publish.

## API contract (what the harness drives)

The harness drives the `chronos-torture` binary and parses its report; it never links rig internals. **PUBLIC SURFACE** below; **PRIVATE INTERNALS — deliberately unspecified:** schedule encoding, shim implementation, orchestration mechanics, workload generator design (its *distribution* is yours; its *determinism* is contracted).

### `chronos-torture` (CLI)

```
chronos-torture --schedules <N> [--seed <base>] [--data-dir <dir>] [--workload sql|server]
chronos-torture --replay <seed> [--minimize]
   exit 0  ⇔ zero unexpected violations
   report: one machine-parseable line per schedule:
   <seed> <verdict: PASS|VIOLATION|EXPECTED-ABORT> <faults-injected> <invariant-or-ledger-ref>
```

- **What it does:** runs N independent seeded fault schedules (seed derived deterministically from `--seed` + index), each: fresh-or-continued database → workload under the fault shim → crash/kill per schedule → recovery → composed oracle verdict. `--replay` reruns one schedule exactly; `--minimize` searches for a smaller schedule preserving the violation.
- **Why it exists:** the one-command durability proof the rest of Tier VI (and the overnight definition-of-done run) invokes.
- **Side-effect requirement:** after any schedule — violation or not — the data directory is either recovered-consistent or quarantined with its seed for post-mortem; the next schedule never inherits undiagnosed corruption silently.
- **Critical contract details:** identical (binary, seed) ⇒ identical injection trace and verdict — the load-bearing contract. Verdict lines are append-written so a crashed *rig* loses at most the in-flight schedule. `VIOLATION` exit is nonzero even if later schedules pass.

### `chronos::io::FaultShim`

```cpp
// Implements the chronos::io engine interface (Phase 0.3's seam), wrapping a real engine.
class FaultShim /* : the io interface */ {
  FaultShim(/* real engine */, const FaultSchedule&);   // schedule is seed-derived, immutable
  // ... the full io:: interface, faithfully or faultily per schedule ...
  const InjectionTrace& trace() const;   // ordered record of every fault actually injected
};
```

- **What it does:** transparently proxies I/O except where the schedule says otherwise: tears writes at sector granularity, acks fsyncs whose writes evaporate at the next crash point, truncates reads, returns EIO.
- **Why it exists:** the seam-based injector — faults expressed in terms chronos's own I/O layer understands, which is what makes schedules targetable at semantically interesting writes.
- **Side-effect requirement:** with an empty schedule, observationally identical to the real engine (the pass-through row). Every injected fault appears in `trace()` — the rig's report and the harness's determinism diff are built from it.
- **Critical contract details:** the engine must have no way to detect it is shimmed. The fsync-lie fault must preserve the *illusion* until a crash point — reads served from the engine's own cache may legitimately see the lied-about data before the crash; the divergence is a disk-truth property, and modeling that honestly is part of the design work.

## Acceptance criteria (phase-level "done")

1. `test/torture_rig/` — all rows PASS: determinism, per-fault-type detection, oracle composition, replay fidelity. Shim pass-through row green under ASan+UBSan.
2. Soak: ≥ 500 seeded schedules across all fault types + SIGKILL, including kill-during-recovery, zero unexplained reds; the expected-abort ledger published with justifications.
3. One overnight run completed and recorded (the build plan's definition-of-done item starts counting here).
4. `RESULTS.md` published; a stranger can rerun `chronos-torture --schedules 100 --seed <s>` and get your verdicts.

## Principal-engineer traps (no solutions)

- **Random kill -9 alone explores a thin slice of the state space.** Process death only ever loses *unflushed* state; syscall-level injection reaches states where the engine proceeded on a false durability promise. The fsync-lied case finds real bugs that pure ARIES torture structurally cannot — if your schedules under-weight it, the rig is theater.
- **Nondeterminism is the rig's death.** Thread schedules and io_uring completion order vary run to run; an oracle that assumes otherwise will flake, and a flaking rig gets ignored. Invariant-based verdicts for concurrent runs; state-equality only where a seed pins a single-threaded schedule. Decide which rows are which *before* they disagree.
- **Triage discipline is a deliverable, not a virtue.** Every red run ends in a minimized reproducer or a documented expected-abort entry — an unfiled flake today is a lost week in 6.4, when the same seed resurfaces inside the composed demo pipeline.
- **A silent read of injected corruption is a finding, not a pass.** If a torn page flows through without a CRC failure and the run still "looks right," the rig must say so — masking by luck is the bug class that ships.
- **The shim must lie consistently.** An fsync-lie whose data survives the crash anyway (because the shim wrote through) tests nothing; one whose data vanishes *before* the crash point tests the wrong thing. The fault's timeline is part of its definition.
- **Quarantine beats cleanup.** Auto-deleting a violated data directory destroys the evidence the seed was supposed to preserve; the next schedule starting from undiagnosed corruption destroys the rig's credibility.

## What you hand back for review

1. Implementation + the harness table + the soak summary (schedule count, fault mix, ledger) + one replayed violation walkthrough (seed → trace → invariant → minimized form, using a deliberately-planted engine bug if the soak was clean).
2. One sentence per trap above: did it bite you, and how did you resolve it?

Review is principal-engineer style: the interview attack ("your rig is green for 500 seeds — name three states it still can't reach"), any overstated claims, the next upgrade. Then Phase 6.3.

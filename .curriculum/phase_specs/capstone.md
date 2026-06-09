# Tier VI · Phase 6.4 — Capstone: the crash-survivable demo & architecture writeup (scaffold)

**Deliverable of this phase:** `./demo.sh` — one scripted, end-to-end run (server up → psql session → TPC-C-lite load from N clients → `kill -9` mid-benchmark → restart → timed ARIES recovery → consistency checks green → benchmark resumes) that runs green on a Linux box that isn't yours — plus `ARCHITECTURE.md`, the design-decision log, and the exam-question self-test answered from memory.

**What you'll own afterward:** the whole machine, defensible at a whiteboard. Not twenty-two components that each passed a harness, but one database whose every layer you can narrate from a SQL byte arriving on a socket down to the sector the WAL record lands on — and back up through recovery when the process dies mid-write.

**Calibration:** 🔨

**Ground rule:** spec + validated harness only. No solution code. You design and write every line — including `demo.sh` itself. The harness judges only the observable pipeline; how you orchestrate it is yours.

---

## What this is — and is not

**This is an integration phase, not a feature phase.** Nothing new gets built here: no new subsystem, no new index, no protocol extension, no "while I'm in here" improvement. The inputs are the 6.1 server, the 6.2 torture rig, the 6.3 benchmark suite, and every tier below them — already shipped, already green against their own oracles. The work of this phase is making them run as *one process under one adversarial script*, and then writing the document that proves you understand what you built.

It is emphatically **not a victory lap**. Integration is where the bugs that every per-phase harness *structurally could not see* finally collide: the pin leak that only manifests when the executor aborts mid-scan because a socket died (6.1's race meeting 1.3's quiesce invariant); the latch acquired in 2.2's order but released in 4.1's abort path; the session teardown that fires concurrently with group commit. Each tier's harness tolerated these in isolation because each harness drove its layer through a controlled front door. The demo drives everything through the real front door at once, with a `kill -9` in the middle. Schedule real time for this phase — historically, integration of independently-green components is where database projects discover what "green" was actually worth.

It is also not just the script. **The writeup is half the deliverable.** A working database you cannot defend at a whiteboard fails this phase. The principal-architect claim this curriculum builds toward is earned by `ARCHITECTURE.md` and the self-test, not by the demo alone.

You're done with the framing when you can say: the deliverable is *one rerunnable proof plus the document that defends it — not new code*.

---

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

System-level debugging and system-level narrative. You'll be the kind of engineer who, handed a stack of independently-tested components, can predict where their *composition* breaks — and who can stand at a whiteboard and trace a transaction from `Query` message to fsynced commit record to post-crash visibility, naming every latch, lock, LSN, and version on the path. That narration skill — holding the whole machine in your head at once — is the difference between someone who built database components and someone who built a database.

### Why "from scratch" is the right call here

There is no library version of integration. But there is a counterfeit version: declaring victory because all twenty-two harnesses are green. The from-scratch discipline here is refusing that counterfeit. Concretely, what stays opaque if you skip this phase:

- Whether your session-death abort path (6.1) actually composes with lock release (4.1), vacuum horizons (4.2), and pin discipline (1.3) under load — three pairwise-green seams that have never all fired in one event.
- Whether recovery time under a *real* TPC-C dirty-page footprint matches what the 3.2 mini-study predicted from synthetic workloads.
- Whether your "stranger can rerun" claims survive an actual stranger's machine — different kernel, different fs, different `psql` version.
- Whether you can answer your own exam questions a year of phases later — the self-test is where banked knowledge is proven to still be owned, not just to have once passed review.

### What carries forward to later tiers

This is the terminal phase, so what carries forward is what carries *out* of the project:

- **`ARCHITECTURE.md`** — the portfolio artifact. It's what you hand someone who asks "what did you build?", and the basis for the Postgres/InnoDB/SQLite comparison conversations this curriculum exists to let you hold.
- **The design-decision log** — every trap that bit across all 23 phases and the trade you made. This is the raw material for the "tell me about a hard bug" question for the rest of your career.
- **The 📖 future-work notes** (replication, 2PC) — the honest statement of where chronos stops, which is itself an architect-level skill: knowing and naming the boundary of what you built.

### What good looks like

- `demo.sh` is boring to read: it starts things, kills things, asserts things, and prints a timed recovery line. All intelligence lives in the components it drives.
- The `kill -9` lands at a *randomized* point in the benchmark window, not a hand-picked safe instant — and the demo is green across repeated runs anyway.
- Recovery time is printed, explained, and consistent with the 3.2 checkpoint-interval study; you can say *why* it took the time it took (how many pages in the DPT, how long the undo chains were).
- The TPC-C consistency conditions pass *after* recovery without any "repair" step — durability did the work, not a fixup script.
- You can answer every spec's exam questions from memory. Where you can't, you reopen that phase before calling this one done.
- A stranger ran it green. Not "could in principle" — did.

### Why this is the shape of the deliverable

A single script plus a writeup, because the correctness problem at this altitude is *composition under crash timing*, and the only honest evidence for that is one continuous adversarial run — not a checklist of subsystem results. The script makes the claim mechanical and rerunnable; the writeup makes it *defended*. Both are required because they fail differently: a demo without a writeup is a magic trick; a writeup without a demo is marketing.

---

## Exam questions this phase targets (build-proven)

1. Run `./demo.sh` end-to-end green on a machine you've never used, with the `kill -9` instant randomized — and report the timed recovery line across 5 runs.
2. Trace, at a whiteboard and from memory, one NewOrder transaction from the `Query` bytes on the socket to its post-crash visibility: every latch, lock, log record, LSN stamp, and version created on the path. (This is the integration exam — the per-phase exam questions are its sub-questions.)
3. Measure recovery time at two checkpoint intervals and reconcile the numbers with your Phase 3.2 study — explain any divergence in terms of DPT size and loser-undo length.
4. Defend three deliberate divergences from PostgreSQL/InnoDB/SQLite designs in `ARCHITECTURE.md` ("they do X, chronos does Y, because Z") — each grounded in a phase you built, not in folklore.

---

## Prerequisites — concepts this phase uses

Almost everything here was built in Tiers 0–VI; this section defines only the vocabulary new at this altitude.

### integration vocabulary

- **end-to-end (E2E) demo** — a scripted run exercising the full stack through its real entry point (the network socket), as opposed to the per-phase harnesses that drove each layer through its API. The demo's value is exactly the seams the harnesses couldn't cross.
- **seam** — a boundary between independently built components where each side's assumptions about the other have never been jointly tested (e.g., session-death abort vs lock release). Integration bugs live in seams, not components.
- **design-decision log** — an append-style record of choices made under uncertainty: the trap, the options, the trade taken, what it cost later. Distinct from `ARCHITECTURE.md` (which describes the result); the log describes the path.

### the comparison targets (📖 level — for the writeup, not for building)

- **PostgreSQL** — process-per-connection, heap tuples with in-page version chains, SSI as built in Ports & Grittner. The closest published design to chronos's Tier IV; your comparison section leans on it.
- **InnoDB (MySQL)** — index-organized tables (the heap *is* a B+Tree on the primary key), undo logs in rollback segments instead of in-heap versions, next-key locking for phantoms. The main counter-design worth defending against.
- **SQLite (WAL mode)** — single-writer, no server, page-level WAL with checkpointing into the main file. Your 6.3 baseline; the comparison explains *which* of chronos's costs buy *which* capabilities SQLite deliberately doesn't have.
- **replication / two-phase commit (2PC)** — making committed state survive *machine* loss (not just process loss) and coordinating commits across nodes. Out of scope for chronos by design; the future-work section names what would have to change (log shipping hooks in 3.1, prepared-txn states in 4.1) without building any of it.

---

## How you know you're aligned (the cross-check)

The continuous oracle for this phase is the composed pipeline you already own: **the 6.2 torture rig running the 6.3 workload through the 6.1 server**, judged by the two independent oracles that have anchored the whole curriculum — the **TPC-C consistency conditions** (the spec's cross-table assertions, e.g. W_YTD = Σ D_YTD, checked by the harness's own scanner after recovery) and the **acked-committed shadow model** (a client-side ledger of transactions acknowledged before the kill; every one must be visible after recovery, every unacked one must be atomic — present entirely or not at all). Neither oracle shares code with the engine; both predate this phase; nothing about them is circular.

The loop while you integrate: run the demo, watch it die at a seam, minimize with the 6.2 rig's replayable seeds, fix in the *component that owns the bug* (re-running that component's own Tier harness to prove the fix didn't regress the layer), re-run the demo. A green demo run is proof the composition holds at that seed; the acceptance bar is green across many seeds and a foreign machine. The harness drives only the public entry points — the socket, the process lifecycle, the on-disk files; everything between is your design, unjudged except through its observable consequences.

For the writeup there is no mechanical oracle — the cross-check is the self-test (answer every phase's exam questions from memory, score yourself honestly) and review, where the interview attack is the whole-system trace.

---

## The build, in parts (each gated independently by the harness)

### Part 1 — The composed pipeline, no crash 🔨
Wire 6.1 + 6.3 into one scripted run: server starts from empty, schema loads, TPC-C-lite runs from N concurrent clients, consistency conditions checked at the end. No kill yet. This flushes the first wave of seam bugs (session churn under load, catalog contention at setup) while failures are still cheap to read.
**Harness rows:** clean E2E run green; consistency conditions green; orphan-txn/lock invariant scan green at quiesce.

### Part 2 — The crash in the middle 🔨
Add the randomized mid-benchmark `kill -9`, the restart, the timed recovery, the post-recovery consistency check, and the benchmark resume. Then fold in 6.2's fault schedules (fsync lies, torn writes) at the `io::` seam under the same script.
**Harness rows:** acked-committed shadow model green across repeated randomized kill points; recovery idempotence (double-restart) green; injected-fault runs either detected-and-refused or recovered — never silent.

### Part 3 — The stranger gate 🔨
`demo.sh` plus a README-level run document, executed on a machine that isn't yours (different kernel/fs at minimum). Everything the script assumes (paths, ports, `psql` availability, build steps) is either checked with a clear error or documented.
**Harness rows:** the demo's own exit status *on the foreign box* is the row.

### Part 4 — The writeup 🔨
`ARCHITECTURE.md` (the system, layer by layer, with the seams called out), the design-decision log (every trap that bit across 23 phases: manifestation → diagnosis → trade), the Postgres/InnoDB/SQLite comparison, the 📖 replication/2PC future-work notes, and the exam-question self-test with honest scoring.

### Part 5 — The harness as a published artifact
Demo green across seeds and machines; RESULTS.md (from `templates/RESULTS.md`) with the timed recovery lines, run counts, and the foreign-box attestation. Update `STATUS.md`: the curriculum is complete.

---

## API contract (what the harness drives)

The harness for this phase drives *processes and files*, not linked symbols. The contracts below are CLI/observable contracts. PRIVATE INTERNALS: everything — this phase adds no new library surface; any helper code inside `demo.sh` is unspecified by design.

### `./demo.sh`

```
./demo.sh [--seed N] [--clients N] [--warehouses N] [--kill-window-ms A:B]
exit 0 iff every stage passed; non-zero with a named failing stage otherwise
```

- **What it does:** the full scripted run — build check, server start from an empty data directory, schema + initial TPC-C population, concurrent benchmark load, randomized `kill -9` inside the kill window, restart, timed recovery, consistency verification, benchmark resume, final report to stdout.
- **Why it exists:** it is the phase's deliverable — the mechanical, rerunnable form of the claim "this database survives crashes under load."
- **Side-effect / durability requirement:** leaves the data directory and all logs from the run on disk for post-mortem; a failed run must say *which stage* failed and leave enough state to debug it (the 6.2 seed line included).
- **Critical contract details:** `--seed` makes the kill instant and workload deterministic where the components allow; the recovery stage must print a single machine-greppable timed line (the harness and RESULTS.md both consume it); the script must refuse to run (clear error, exit ≠ 0) rather than degrade silently if a dependency (`psql`, free port, fs support) is missing.

### `ARCHITECTURE.md` + the design-decision log + the self-test

```
ARCHITECTURE.md · DECISIONS.md · SELFTEST.md   (repo root or RESULTS/ — your call, stated in README)
```

- **What they are:** the system description with seams called out; the per-trap decision record; the all-phases exam-question answers with self-scoring.
- **Why they exist:** the principal-architect half of the deliverable — the demo proves the machine works, these prove *you* hold the machine.
- **Side-effect requirement:** the comparison section contains at least three defended divergences from Postgres/InnoDB/SQLite; the future-work section names the replication/2PC boundary without code.
- **Critical contract details:** review treats unanswered or hand-waved self-test questions as phase-blocking — reopen the owning phase rather than annotating around it.

---

## Acceptance criteria (phase-level "done")

1. `./demo.sh` green across ≥10 seeded runs locally (randomized kill points) **and** ≥1 run on a machine that isn't yours, with the foreign run attested in RESULTS.md.
2. At least one demo configuration runs under 6.2 fault schedules with zero silent corruptions (every injected fault detected or recovered).
3. The 6.2 rig has run overnight at least once against the composed pipeline with zero unexplained reds (per `build_plan.md`'s definition of done).
4. `ARCHITECTURE.md`, the decision log, and the completed self-test are published; RESULTS.md carries the timed recovery lines reconciled against the 3.2 study.
5. `STATUS.md` updated: curriculum complete.

---

## Principal-engineer traps (no solutions)

- **Integration is not a victory lap.** The seams listed in "What this is" *will* fire. Budget the time; resist fixing seam bugs with demo-script workarounds — the fix belongs in the component that owns the invariant, gated by that component's own harness re-run.
- **Bisect with the tiers, not with printf.** When the demo dies, the first move is re-running the owning tiers' harnesses — a seam bug usually means one component's *contract* was narrower than its consumer assumed. Find which spec was violated before touching code.
- **A hand-picked kill instant is a demo of nothing.** If the kill window quietly shrinks to "after the load phase settles," the artifact stops proving what it claims. Keep the window wide and the instant random; let the reds teach you.
- **Recovery time is a result, not a footnote.** An unexplained recovery duration — too fast as much as too slow — is a finding to chase (empty DPT? checkpoint raced the kill? undo chains shorter than the workload implies?), not a number to report and move past.
- **The writeup slips last and matters most.** Every project pressure says ship the demo and stub the documents. The self-test especially — it's the only gate that catches knowledge that quietly evaporated five tiers ago, and it's the half of the deliverable that makes the curriculum's claim true.

---

## What you hand back for review

1. The demo output (local seeds + the foreign-box run), the timed recovery lines, and RESULTS.md.
2. `ARCHITECTURE.md`, the decision log, and the scored self-test.
3. One sentence per trap above: did it bite, and how was it resolved.

Review is principal-engineer style and it is the final one: the whole-system whiteboard trace (exam question 2, live), the three defended divergences, and an audit that every claim in `ARCHITECTURE.md` traces to a shipped, torture-proven artifact rather than an intention. Then the curriculum is closed — and what you do with a from-scratch, crash-survivable OLTP engine and the OLAP sibling beside it is the next project's question.

*Start Part 1 when ready — everything `demo.sh` calls already exists; the blank file is the script itself.*

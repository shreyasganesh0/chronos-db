# Tier IV · Phase 4.3 — Isolation levels & serializable SI (scaffold)

**Deliverable of this phase:** Read Committed / Repeatable Read / SI selectable per transaction, plus SSI layered on SI — proven by an anomaly battery in which every scripted anomaly **occurs** at the levels that permit it and is **prevented** at the levels that forbid it, and by an offline DSG cycle checker over SSI soak runs.
**What you'll own afterward:** the ability to choose, defend, and debug an isolation level — to read `could not serialize access due to read/write dependencies among transactions` and know exactly which two edges fired, and to answer "is Repeatable Read enough for this workload?" with a mechanism instead of a shrug.
**Calibration:** 🔨 (RC/RR/SI selection) / 🧩 (SSI) / 📖 (Ports & Grittner as reference design)
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is a policy layer composed from machinery you already built — not new storage and not new locks.** Phase 4.1 gave you transaction-scoped locks with a hierarchy; Phase 4.2 gave you snapshots, version chains, and write-conflict detection. This phase wires *combinations* of those into named isolation levels, then adds the one genuinely new mechanism — SSI's read-dependency tracking — on top of SI. If you find yourself adding a new lock table or a second version store, you've misread the phase: every level here is a *configuration* of Tier IV parts plus bookkeeping.

The deliverable is **not a feature flag**. `SET TRANSACTION ISOLATION LEVEL` is trivial to plumb; what you ship is a **proven anomaly matrix** — evidence, both directions, that each level admits exactly the anomalies its definition permits and no others. An isolation level that's accidentally too strong is as wrong, for this phase, as one that's too weak: it means you don't know what you built, and the harness is designed to refuse you the comfortable ambiguity.

You're done with the framing when you can say: the deliverable is **a per-level required-outcome matrix with every cell proven by a scripted anomaly**, not a working `SET TRANSACTION` statement.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

You'll be the kind of engineer who reasons about application correctness in terms of *which anomalies the chosen level admits* — who can look at a balance-check-then-withdraw pattern and say "that's write skew, SI won't save you, you need SSI or an explicit lock," and who treats serialization-failure retry loops as a designed-in cost, not a bug report.

### Why "from scratch" is the right call here

Use the database's isolation levels as labels on a config knob and these stay folklore:

- Why Postgres's `REPEATABLE READ` is actually SI, and why the ANSI level names underdetermine behavior at all (the entire point of Berenson et al.).
- Why your SERIALIZABLE transaction aborted when it conflicted with *nothing it wrote* — what an rw-antidependency is and why two of them in a row are the danger sign.
- Why phantoms survive every tuple-level mechanism you own, and what an index range has to do with locking rows that don't exist yet.
- Why a read-only transaction can be the one that makes a history non-serializable (Fekete et al.'s result — the anomaly almost nobody can explain at a whiteboard).
- Why SSI aborts more than strictly necessary and why that's the correct engineering trade.

### What carries forward to later tiers

- **Phase 5.3** — the binder/executor honors the session's isolation level: the scan path's choice of "take S locks vs. read the snapshot" and "register SIREAD/range tracking" is decided by what you build here.
- **Phase 5.2 / 6.1** — `SET TRANSACTION ISOLATION LEVEL` in the SQL grammar and per-session defaults in the server both terminate in this phase's `begin(level)`.
- **Phase 6.3** — the isolation-level ablation (throughput and abort rate per level under YCSB/TPC-C-lite) is the benchmark face of this phase's matrix; the SSI abort-rate number you start measuring now becomes a headline result there.
- The **2.1 iterator-validity contract** pays its final dividend: phantom tracking hangs range registration on the B+Tree iterator — the seam the build plan said was load-bearing three tiers ago.
- The **anomaly-battery / both-directions discipline** generalizes: it's the same test philosophy as the elle-style checkers used against real distributed databases.

### What good looks like

- Each level is expressible in one sentence as a composition: which snapshot timing, which lock policy, which tracking. If a level needs a paragraph, the design is tangled.
- You can answer without running code: *"Walk me through write skew under SI, then show me the two rw-edges SSI sees and which txn it aborts."* and *"Why must SIREAD locks survive commit?"*
- The anomaly matrix has no blank cells: every (anomaly × level) pair is either OCCURS-and-the-harness-saw-it or PREVENTED-and-the-harness-saw-the-abort.
- SSI's false-positive rate is *measured* (abort rate on conflict-free and low-conflict workloads), reported, and not "fixed."
- SSI soaks pass the offline DSG cycle check across every seed — zero committed cycles, ever.
- Read-only transactions are handled deliberately: they participate in SSI tracking (the read-only anomaly row proves it) and you can say why.
- Victims surface `SerializationFailure` cleanly through `Result<T>` — no level leaks its mechanism as a hang, a wrong answer, or UB.

### Why this is the shape of the deliverable

Isolation bugs are workload-dependent and bidirectional: a too-weak level corrupts data under exactly the interleaving your tests didn't write, and a too-strong one passes every safety test while silently being a different product. The only honest artifact is therefore the battery itself — scripted interleavings, each with a *required outcome per level*, run both directions — plus, for SSI's randomized soaks where scripts can't enumerate the space, an offline checker (elle-style) that reconstructs ww/wr/rw edges from operation logs and asserts the committed history's serialization graph is acyclic. The checker shares a log format with the engine and nothing else.

## Exam questions this phase targets (build-proven)

1. Implement per-txn RC, RR, and SI over the 4.1/4.2 machinery such that every cell of the harness's anomaly matrix reports its required outcome — including the cells that require the anomaly to **occur**.
2. Implement SSI (SIREAD tracking + rw-antidependency detection + the dangerous-structure rule) such that write skew and the read-only anomaly are prevented at SERIALIZABLE — one txn aborts with `SerializationFailure` — and ≥ 10 randomized soak seeds produce zero committed DSG cycles.
3. Implement phantom handling via next-key/range tracking through the 2.1 iterator contract such that the phantom row is prevented where required, and report SSI's measured abort rate on a low-conflict workload.

## Prerequisites — concepts this phase uses

Audience: a strong C/C++ systems programmer who has never built a database. Recognize each term and the role it plays. Do NOT look up implementations — implementing them is the build.

### Isolation levels and their literature

- **isolation level** — a named contract about which interleaving effects a transaction may observe. ANSI SQL defined four by *prohibited anomalies*; the definitions were shown ambiguous and incomplete by Berenson et al., *A Critique of ANSI SQL Isolation Levels* (1995) — the paper that also defined Snapshot Isolation. Read at vocabulary level: you need its anomaly names, not its proofs.
- **Read Committed (RC)** — the level where each statement sees only committed data, but two statements in one txn may see different committed states. Its role here: the weakest level chronos ships, and the behavioral signature of a *fresh-snapshot-per-statement* policy.
- **Repeatable Read (RR)** — reads, once made, are stable for the txn's lifetime; phantoms (new rows matching a predicate) remain possible. In chronos: a policy over 4.1 locks and/or the 4.2 snapshot — your composition, documented.
- **Snapshot Isolation (SI)** — the 4.2 machine used directly: one snapshot at BEGIN, first-committer-wins. Forbids the classic anomalies but admits write skew — the gap SSI closes.
- **SERIALIZABLE / SSI** — Serializable Snapshot Isolation: SI plus runtime detection of the dependency structures that make SI histories non-serializable, resolved by aborting. Origin: Cahill, Röhm & Fekete (SIGMOD 2008). The production reference design — and this phase's 📖 — is Ports & Grittner, *Serializable Snapshot Isolation in PostgreSQL* (VLDB 2012).

### The anomaly bestiary (the matrix's rows)

- **lost update** — two txns read-modify-write the same row; one update overwrites the other. Forbidden by SI's write-write conflict rule; observable under RC.
- **non-repeatable read** — a row re-read within one txn has changed. RC's signature.
- **phantom** — a re-run *predicate* read (range scan) returns new rows another txn inserted. Tuple-level mechanisms can't touch it: the row that conflicts didn't exist when you read.
- **write skew** — two txns read overlapping data, write disjoint rows, both commit; the conjunction violates an invariant neither saw broken. SI's signature anomaly (4.2 made you reproduce it; this phase makes SERIALIZABLE kill it).
- **read-only anomaly** — Fekete, O'Neil & O'Neil (2004): a *read-only* txn observes a state inconsistent with any serial order of the other two committed txns. Its role: it proves read-only transactions cannot be exempted from SSI tracking, and it is the test almost nobody writes.

### SSI mechanics

- **concurrent transactions** — in SI/SSI vocabulary: two txns neither of which committed before the other's snapshot was taken. Concurrency in this sense — overlap in snapshot time, not in wall-clock execution — is what every SSI rule below quantifies over.
- **predicate read** — a read defined by a condition ("all rows with `k` in [10, 20)") rather than an identity. The read whose conflict set includes rows that *don't exist yet* — which is why it needs range tracking, not tuple marking.
- **SIREAD lock** — not a lock that blocks anyone: a *marker* recording "this txn read this object under SSI," kept so later writers can be detected as creating rw-antidependencies. Crucially, it must outlive the reader's commit while concurrent txns remain.
- **rw-antidependency** — the edge Ti —rw→ Tj meaning Ti read a version Tj later overwrote (Ti "read under" Tj's write). The dependency type SI fails to serialize; SSI exists to watch it.
- **dangerous structure** — the Ports & Grittner abort rule: a txn with an inbound *and* an outbound rw-antidependency edge among concurrent txns (two consecutive rw-edges) sits in every SI cycle; detect that pattern and abort one participant. Conservative on purpose — it fires on some safe histories.
- **next-key / gap (range) tracking** — recording a *key range* in an index, not a tuple, so an insert *into* the range is detectable as a conflict with a prior predicate read. The phantom answer, hung on the 2.1 B+Tree iterator.
- **DSG (direct serialization graph)** — Adya's formalism: nodes are committed txns; edges are ww/wr/rw dependencies; the history is serializable iff the graph is acyclic. The offline soak checker's data structure (the same idea behind the elle checker used by Jepsen).
- **serialization failure** — the error a victim receives; the application-visible contract of SSI is "retry on this error."

## How you know you're aligned (the cross-check)

**The anomaly battery IS the oracle.** Each anomaly in the bestiary is a scripted multi-txn interleaving with a *required outcome per isolation level*: at levels whose definition permits the anomaly, the harness asserts the anomalous result **actually materialized**; at levels that forbid it, the harness asserts exactly one txn aborted with `SerializationFailure` (or `WriteConflict`, where SI's FCW is the preventing mechanism) and the invariant held. This both-directions discipline is what makes the oracle non-circular: an implementation that's secretly 2PL-strength everywhere fails the OCCURS cells; one that's secretly RC everywhere fails the PREVENTED cells. You cannot pass by being vaguely strong or vaguely fast — only by being *exactly* each level.

Scripts can't cover SSI's randomized space, so soaks get a second, independent oracle: every operation is logged `(seq, txn, op, object, version)`, and an offline DSG checker — elle-style, zero engine code linked — reconstructs ww/wr/rw edges over committed txns and asserts acyclicity. Any committed cycle is a FAIL with the cycle printed.

The harness in `test/isolation_ssi/` touches only the public surface below — `begin(level)`, the 4.2 read/write calls, the range-read registration, commit/abort results. SIREAD storage, edge bookkeeping, abort-victim choice: invisible, yours. Build a part, run the battery, watch matrix cells flip SKIP→PASS.

## The build, in parts (each gated independently by the harness)

### Part 1 — Levels as compositions: RC, RR, SI selectable per txn 🔨
`begin(IsolationLevel)`; each level mapped onto 4.1 locks and 4.2 snapshots per your documented composition (RC's per-statement visibility refresh, RR's stability guarantee, SI as 4.2 native). **Harness rows:** the non-repeatable-read and lost-update matrix cells for all three levels — both directions; level isolation (concurrent txns at different levels each get their own contract).

### Part 2 — Phantoms: range tracking through the iterator contract 🔨
Predicate reads register the key ranges they scanned, via the 2.1 B+Tree iterator seam; inserts into a registered range become detectable conflicts at the levels that forbid phantoms. **Harness rows:** the phantom cells — OCCURS at RC (and wherever your documented RR composition permits it), PREVENTED at SERIALIZABLE; insert-adjacent-to-range probe (tracking must not overreach: an insert *outside* every registered range conflicts with nothing).

### Part 3 — SSI: SIREAD tracking, rw-antidependencies, dangerous structures 🧩
SERIALIZABLE = SI + tracking: SIREAD markers on reads (tuple and range), rw-edge detection when writes land on marked objects, the two-consecutive-rw-edges abort rule, victims surfacing `SerializationFailure`. SIREAD lifetime extends past commit until all concurrent txns finish. 📖 Ports & Grittner is the reference design — read it for shape and vocabulary; the mechanism you build is yours. **Harness rows:** write-skew PREVENTED at SERIALIZABLE (still OCCURS at SI — both cells); the **read-only anomaly** cell; SIREAD-lifetime probe (a commit-then-conflict timing window the harness aims at directly); abort-rate measurement on conflict-free and low-conflict workloads.

### Part 4 — The harness as a published artifact 🔨
The full matrix green both directions; ≥ 10 SSI soak seeds through the offline DSG checker with zero committed cycles; TSAN and ASan/UBSan builds green. Write `RESULTS.md` from `templates/RESULTS.md`: the complete anomaly matrix as the centerpiece, the measured SSI abort rates with a paragraph defending them as a feature, your RR composition documented, and the DSG soak summary. Publish so a stranger can rerun.

## API contract (what the harness links against)

All fallible calls return `Result<T>`. Everything below is the **public surface**. PRIVATE INTERNALS — SIREAD storage, edge representation, range-tracking granularity, victim choice within a dangerous structure — are deliberately unspecified; designing them is the work.

### Level selection (extends 4.1/4.2 `TransactionManager`)

```cpp
enum class IsolationLevel { ReadCommitted, RepeatableRead, SnapshotIsolation, Serializable };
auto begin(IsolationLevel level) -> Result<TxnId>;
auto level_of(TxnId txn) -> Result<IsolationLevel>;
```

- **What it does:** starts a transaction whose every subsequent read, write, and commit obeys the named level's contract. The level is fixed at begin for the txn's lifetime.
- **Why it exists:** the per-txn knob 5.2's `SET TRANSACTION` and 6.1's session defaults terminate in; the dimension 6.3 ablates.
- **Side-effect requirement:** the level governs observable behavior from the first operation — there is no "warm-up statement" at a different level.
- **Critical contract details:** concurrent txns at different levels must each receive their own contract (the harness mixes levels in one run). Changing level mid-txn is an error.

### Reads, writes, and range registration

```cpp
// Tuple reads/writes: the 4.2 VersionStore surface, now level-aware via the txn.
auto read(TxnId txn, Rid rid) -> Result<std::optional<TupleView>>;
// Predicate reads: the executor (and the harness, directly) registers scanned ranges.
auto register_range_read(TxnId txn, IndexId index, KeyRef low, KeyRef high) -> Result<void>;
```

- **What it does:** `read` behaves per the txn's level (RC's statement-fresh visibility vs. SI's begin-snapshot; SIREAD marking under SERIALIZABLE). `register_range_read` records that the txn predicate-read `[low, high]` on an index, so subsequent inserts into the range by concurrent txns are conflict-detectable at levels that forbid phantoms.
- **Why it exists:** range registration is the *only* mechanism that can see a phantom — the conflicting row doesn't exist at read time, so no tuple-granularity bookkeeping can ever catch it. In 5.3 the IndexScan operator calls this through the iterator seam; here the harness calls it directly.
- **Side-effect requirement:** under SERIALIZABLE, both tuple reads and registered ranges leave SIREAD markers that persist beyond the txn's commit until no concurrent txn remains.
- **Critical contract details:** registration is cheap enough to call per scan (5.3 will); over-broad ranges are a correctness-preserving but abort-rate-inflating choice — the granularity is yours, the no-overreach probe bounds it from below.

### Commit under SSI

```cpp
// commit(txn): 4.1/4.2 semantics, plus under Serializable —
//   returns ErrorCode::SerializationFailure if the txn is aborted as a
//   dangerous-structure victim (at commit or earlier, surfaced on any call).
```

- **What it does:** finalizes the txn per its level; under SERIALIZABLE, a txn implicated in two consecutive rw-antidependency edges may be aborted instead — at commit time or eagerly on the operation that completed the structure.
- **Why it exists:** `SerializationFailure` is the entire application-visible contract of SSI: the engine promises serializability and the application promises to retry.
- **Side-effect requirement:** a victim's effects are fully undone (the 4.2 abort path) and its WAL commit record is never written. A *committed* SERIALIZABLE txn is guaranteed to appear in no DSG cycle — the soak checker holds you to exactly this.
- **Critical contract details:** which participant of a dangerous structure dies is your documented policy; the harness requires only that *someone* dies, the invariant holds, and survivors commit. False-positive aborts are legal and measured, never an error.

## Acceptance criteria (phase-level "done")

1. Harness: every anomaly-matrix cell PASS in its required direction — lost update, non-repeatable read, phantom, write skew, read-only anomaly × RC / RR / SI / SERIALIZABLE — under ASan/UBSan **and** TSAN builds.
2. SSI soaks: ≥ 10 randomized seeds, offline DSG checker reports zero committed cycles; SIREAD-lifetime and no-overreach probes green.
3. SSI abort rate measured and reported for conflict-free and low-conflict workloads (the conflict-free number is your false-positive floor).
4. `RESULTS.md` published with the full matrix; a stranger can rerun.

## Principal-engineer traps (no solutions)

- **SSI false positives are correct behavior; false negatives are corruption.** The dangerous-structure rule is conservative by design — when in doubt, abort. The failure mode to fear is "fixing" an annoying abort with a cleverer condition and quietly admitting a cycle. Measure the abort rate; do not tune it down with correctness.
- **SIREAD locks must outlive commit** until every concurrent transaction has finished. Free them at commit and write skew reenters through a timing window — a committed reader's overwritten read becomes invisible to detection. The harness aims a probe at exactly this window; production Postgres carries this state in shared memory for the same reason.
- **No tuple-level mechanism stops phantoms.** If your phantom cell passes without range tracking, your test is wrong, not your engine clever — the conflicting row did not exist to be marked. This is the payoff of 2.1's iterator-validity contract; hang the tracking on that seam, not on the heap.
- **The read-only anomaly is the test almost nobody writes.** Write it. If your SSI exempts read-only txns from tracking as an "optimization," this cell is the one that catches you (the safe read-only optimizations in Ports & Grittner are precise, not a blanket exemption).
- **Both directions, always.** A level that prevents an anomaly it should permit is a mislabeled product — it will also show up as an inexplicable throughput cliff in 6.3. The OCCURS cells are not optional decorations on the matrix.
- **Mixed-level runs are where compositions leak.** An RC txn and a SERIALIZABLE txn touching the same rows must *each* get their contract; sharing the wrong bookkeeping between them breaks one silently.

## What you hand back for review

1. Implementation + the full anomaly matrix (all sanitizer builds) + DSG soak summary + measured SSI abort rates + your documented RR composition and victim policy from RESULTS.md.
2. One sentence per trap above: did it bite you, and how did you resolve it?

Review is principal-engineer style: the whiteboard attack ("draw write skew's two rw-edges and tell me who dies"), the both-directions audit of the matrix, and the seam check — can 5.3's IndexScan actually drive `register_range_read` through the iterator contract as-is? Then Tier IV is closed and we advance to Phase 5.1.

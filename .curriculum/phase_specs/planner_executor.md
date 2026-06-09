# Tier V · Phase 5.3 — Binder, planner & Volcano executor (scaffold)

**Deliverable of this phase:** end-to-end SQL — bind against the 5.1 catalog, plan heuristically, execute through pull-based Volcano iterators wired to MVCC snapshots (4.2) and lock acquisition (4.1) — proven green on a seeded random-statement differential suite against SQLite.
**What you'll own afterward:** the layer where SQL meets transactions: you'll know exactly where visibility checks and lock acquisitions live in a scan path, why OLTP engines pull rows while your quackdb OLAP engine pushed vectors, and what that choice costs and buys.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is the execution layer of an OLTP engine: binder + heuristic planner + row-at-a-time pull iterators, with transaction semantics threaded through the scan path.** A bound, planned query becomes a tree of operators — SeqScan, IndexScan, Filter, Project, NestedLoopJoin, Sort+Limit, Insert/Update/Delete — where each parent calls `next()` on its child and rows flow upward one at a time. Inside the scans, every tuple is checked against the transaction's snapshot and locked per the isolation level before it is allowed up the tree. That interleaving — visibility and locking *inside* the iterator, per row — is the substance of the phase.

It is **not** an optimizer. The planner here is a handful of fixed heuristics: is the predicate index-eligible? then point-lookup or index-range scan; otherwise seq scan. No cost model, no cardinality estimation, no join reordering — 📖 that is quackdb L25–26 territory, explicitly out of scope, and any hour spent on costing here is an hour stolen from the actual lesson.

It is also **not** vectorized, and that is deliberate, not a simplification. quackdb pushed column vectors down a pipeline because analytics amortizes per-batch overhead over thousands of values and needs no per-row transaction checks. chronos pulls single rows because OLTP queries touch few rows and *every row needs a visibility verdict and possibly a lock* before the next operator may see it. Holding both architectures in your head — push/vectors/columns vs pull/rows/visibility+locks — IS the lesson; build this one clean enough to articulate the contrast.

You're done with the framing when you can say: the deliverable is *SQL executing transactionally through pull iterators*, not *a query optimizer* and not *quackdb again*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

You'll be the kind of engineer who can debug a transactional query at the operator level: "the wrong row came back" decomposes into binder, plan choice, iterator contract, visibility check, or lock-wait recheck — five distinct suspects you can interrogate separately. And you'll have designed an error-carrying iterator protocol under `-fno-exceptions` once, on paper, and lived with the consequences through seven operators.

### Why "from scratch" is the right call here

- **Where visibility lives goes opaque otherwise.** Reading "MVCC scan" in a paper tells you nothing about the moment a SeqScan holds a page latch, finds a version chain, and must decide visibility before unlatching. You built the rules in 4.2; this is where they meet a query, and only the wiring teaches the order of operations.
- **The lock-wait recheck is invisible from outside.** Postgres's EvalPlanQual exists because a tuple can change while you wait for its lock. Users never see this machinery; engineers who haven't built it routinely design isolation bugs. The differential suite will catch you if you skip it.
- **The Halloween problem is only real once you cause it.** An UPDATE driven by an index scan over the column it updates is a famous self-feeding loop. You can recite it from a textbook; you can only *defend a design against it* after your own executor has eaten its own writes.
- **The no-exceptions iterator tax must be paid in person.** Threading a three-state `next()` through a join's nested loops and a sort's buffer teaches exactly what exceptions were hiding. This is the deepest `Result`-propagation exercise in the codebase.

### What carries forward to later tiers

- **Phase 6.1:** the server's session loop is `Parse → Bind → Plan → Execute` per message; this phase ships everything after the arrow. Session = txn context only works because executors take an explicit transaction.
- **Phase 6.2:** the torture rig runs full-SQL workloads through this executor; its invariant scans assume the executor never leaks a pin, latch, or lock on any error path — the discipline you establish here.
- **Phase 6.3:** YCSB and TPC-C-lite execute through these operators; every benchmark number measures this scan path. The ablation curves (isolation level vs throughput) are legible only because you know where the locks are taken.
- **Phase 6.4:** the capstone demo is this phase's pipeline under fire — psql in, ARIES recovery mid-benchmark, consistency checks green.

### What good looks like

- The three-state `next()` contract was written down *before* the first operator and survived all seven without amendment — or the amendments are documented with reasons.
- Operators are uniform: a NestedLoopJoin doesn't know whether its child is a scan or another join. Adding a hypothetical eighth operator would touch no existing one.
- Visibility and locking appear in exactly one layer (the scans / write operators), not smeared across the tree. You can point at the single place a tuple gets its snapshot verdict.
- Every error path unwinds cleanly: a mid-query abort (deadlock victim, write-write conflict) releases pins and latches and leaves the transaction abortable — provable because the differential suite keeps running afterward.
- You can answer cold: "walk a row from heap page to client through `SELECT … WHERE k > 5 ORDER BY v LIMIT 3`," "where exactly does Read Committed differ from Snapshot Isolation in your scan code," and "why doesn't your UPDATE loop forever?"

### Why this is the shape of the deliverable

The output of a SQL engine is a result set, and the space of (schema × data × statement × isolation) is far too large for hand-written expectations — but it has a perfect independent referee: another SQL engine. Hence differential testing against SQLite on seeded random statement streams: thousands of statements where any disagreement is a bug in one of us, and the seed replays it. Concurrency correctness already has its own oracles (Tier IV's serializability checker and anomaly battery), so this suite runs visibility-sensitive cases single-session to keep the SQLite comparison fair — this harness judges *query semantics on top of* the transaction layer, not the transaction layer again.

## Exam questions this phase targets (build-proven)

1. Implement Bind→Plan→Execute such that a 10,000-statement seeded random stream produces result sets identical to SQLite's (order-insensitive, order-sensitive under ORDER BY) across CREATE/INSERT/SELECT/UPDATE/DELETE.
2. Make `UPDATE t SET k = k + 10 WHERE k < 100`, planned over an index on `k`, terminate with each qualifying row updated exactly once — the Halloween row PASSes.
3. Demonstrate the Read Committed lock-wait recheck: in the harness's scripted two-session scenario, a tuple modified while session B waits for its lock is re-evaluated against B's predicate, and B updates only rows that still qualify.
4. Compile `WHERE k >= a AND k < b` onto the 2.1 range iterator with bounds correct at every inclusive/exclusive corner — fuzz-proven.

## Prerequisites — concepts this phase uses

Role-level vocabulary for a systems programmer who has never built a database engine. Recognize the term and its job; the build is yours.

### Frontend-to-execution vocabulary

- **binder (semantic analysis)** — the pass between parse and plan that resolves every name in the AST against the catalog: table name → TableId + schema, column name → column index + type, with type checking of expressions. Turns "text that parsed" into "a statement that is meaningful against this database." Errors here ("no such column") are user errors, distinct from syntax errors. You built a sibling in quackdb L23 against an in-memory map; this one reads 5.1's transactional catalog under the caller's snapshot.
- **query plan** — the executable tree of operators the planner emits from a bound statement. Its in-memory representation is private to you; the harness sees only `Explain`'s rendering of it.
- **heuristic planning** — choosing plan shape by fixed rules rather than estimated cost: equality on a uniquely-indexed column → point lookup; range predicate on an indexed column → index-range scan; otherwise → seq scan; inner join input → prefer index lookup when one exists. Real engines layer cost models on top (📖 quackdb L25–26); the rules are enough for OLTP point queries and are this phase's ceiling.
- **sargable predicate** — industry shorthand for a predicate that can be compiled into index search bounds (`k = 5`, `k >= a AND k < b`), as opposed to one that can't (`k + 1 = 6`). The planner's index-eligibility test is exactly "is this sargable against an existing index?"

### The Volcano / iterator model

- **Volcano model** — the classic execution architecture (Graefe, *Volcano — An Extensible and Parallel Query Evaluation System*, IEEE TKDE 1994; read at vocabulary level) in which every operator implements one uniform iterator interface — open / next / close — and a plan executes by the root pulling rows from its children, which pull from theirs. Uniformity is the point: operators compose without knowing each other.
- **pull vs push execution** — Volcano pulls a row at a time from the top; quackdb (L13–24) pushed column-vector batches from the bottom. Pull-per-row pays a per-tuple call overhead OLAP can't tolerate, and in exchange gets a natural place to interleave per-row visibility checks, lock waits, and early termination (LIMIT) — the OLTP trade.
- **pipeline breaker** — an operator that must consume its entire input before emitting anything (Sort here; hash-build in other engines). Matters because rows downstream of a breaker are no longer "live" tuples from the scan — a property one of the traps turns on.
- **the three-state `next()`** — each pull yields exactly one of: a row, exhaustion, or an error. With exceptions, the third state rides the unwinder; under `-fno-exceptions` it must be explicit in the return type and *checked by every caller at every level*. Designing this contract once, on paper, before any operator exists, is a named requirement of this phase.

### Transactional execution vocabulary

- **visibility check in the scan path** — applying 4.2's snapshot rules to each version chain *inside* the scan operator, so upper operators only ever see tuples that exist for this transaction. The executor's defining difference from quackdb's scans.
- **Halloween problem** — the classic failure where an UPDATE whose driving scan is an index on the updated column re-encounters rows it already moved (named at IBM circa 1976 when a raise-all-salaries update ran until everyone earned the maximum). The standard defenses are breaking the pipeline or remembering touched RIDs; choosing one is your design call.
- **EvalPlanQual-style recheck** — PostgreSQL's name (see its docs/README on EvalPlanQual) for the Read Committed obligation: after blocking on a row lock, the row may have been changed by the committer you waited for — so you must re-fetch the current version and re-evaluate your predicate before acting. The role is what matters; your mechanism is yours.
- **differential testing** — running identical inputs through two independent implementations and diffing outputs; any divergence convicts one of them. **SQLite** is the reference here — mature, embeddable, trivially driven from a harness, and importantly *not your code*. The harness restricts generated SQL to semantics where chronos and SQLite are defined to agree.

## How you know you're aligned (the cross-check)

The harness in `test/planner_executor/` is the continuous oracle, and it is layered so you light it up part by part: binder rows first (bind errors and resolved-shape checks against a catalog fixture), then plan-shape rows (`Explain` output vs required plan class), then per-operator execution rows on tiny fixtures, then the full differential gauntlet.

The independent oracle is **SQLite**: the harness builds the same schema and data in both engines, runs the same seeded random statement stream through both, and compares result sets — order-insensitively as multisets, except order-sensitive where the statement carries ORDER BY (with a deterministic tiebreak required of the generator, so comparison is well-defined). DML statements are compared by their *after* state (full-table diffs), not just return counts. Visibility-sensitive scenarios (the recheck row, lock-interaction rows) run single-session or with scripted session interleavings the harness controls — Tier IV already owns concurrency correctness; this suite owns query semantics. Every red row prints its seed; replay is one command.

The harness sees only the public surface below: `Bind`/`Plan` results as observable errors-or-success, `Explain` text, and rows pulled from `ResultSet`. Your plan-tree representation, operator class layout, and expression evaluator are invisible to it.

## The build, in parts (each gated independently by the harness)

### Part 1 — The binder 🔨
AST + catalog + transaction → bound statement: names resolved to ids/indexes, types checked, `*` expanded, errors positioned and user-grade. Reads the catalog under the caller's snapshot via 5.1's lookup surface. **Harness rows:** bind-resolve fixtures, bind-error battery (unknown table/column, type mismatch, ambiguous column in the join), bind-respects-snapshot.

### Part 2 — The `next()` contract + the read pipeline 🔨
First, on paper: the three-state contract — who allocates the row, what error propagation looks like through N levels, what `close()` must guarantee after an error, rebind/rerun rules. Check that half-page of design in *before* operator code; it is reviewable. Then SeqScan, IndexScan (point + range, compiled bounds on the 2.1 iterator), Filter, Project, NestedLoopJoin, Sort+Limit — scans doing visibility (4.2) and lock acquisition (4.1) per the transaction's isolation level. **Harness rows:** per-operator fixtures, the inclusive/exclusive bounds fuzz, join correctness vs SQLite on small relations, ORDER BY/LIMIT semantics, error-unwind row (an injected mid-scan abort leaks no pins or locks — checked via the 1.3 pin-leak detector and the lock table at quiesce).

### Part 3 — The write path 🔨
Insert, Update, Delete operators driven by the read pipeline, taking X locks, writing through 4.2's version machinery, maintaining every index registered in `chronos_indexes`. The two marquee correctness rows live here: Halloween (the self-feeding UPDATE must terminate with exactly-once updates) and the Read Committed lock-wait recheck (scripted two-session interleaving). **Harness rows:** DML state-diffs vs SQLite, index-maintenance check (post-DML index scan ≡ seq scan results), halloween-update, rc-recheck, write-conflict surfaces as a clean `Result` error.

### Part 4 — The planner 🔨
The heuristic rules, made visible: `Explain` renders the chosen plan shape, and the harness asserts plan class for a fixture battery (point predicate on indexed column → point lookup; range → index-range with correct bound openness; non-sargable → seq scan; planner survives index absence). Wired last deliberately — every operator is already proven, so plan bugs can't hide behind operator bugs. **Harness rows:** plan-shape battery, plan-vs-result consistency (forcing seq scan vs index scan yields identical result sets — the planner may only change speed, never answers).

### Part 5 — The harness as a published artifact 🔨
The full differential gauntlet at scale — thousands of seeded statements across multiple schemas and isolation levels, zero divergences — plus all earlier rows green under ASan/UBSan and the TSAN scripted-interleaving rows. `RESULTS.md` from `templates/RESULTS.md`: the table, stream sizes and seeds, and a paragraph you'll want at interviews — the pull-rows-vs-push-vectors contrast with quackdb, written from having built both. Publish.

## API contract (what the harness links against)

**PRIVATE INTERNALS — deliberately unspecified:** the bound-statement and plan-tree representations, operator class layout, expression evaluation, the Halloween defense mechanism, and the recheck mechanism. The three-state `next()` shape is mandated at the public `ResultSet` boundary below; its internal operator-to-operator form is your design (it will likely mirror the boundary — that's fine, but it's your call).

### `Bind`

```
Result<BoundStatement> Bind(const Statement&, Catalog&, Transaction&);
```

- **What it does:** resolves and type-checks one parsed statement against the catalog as visible to this transaction; fails with user-grade, positioned errors.
- **Why it exists:** the seam between text and meaning — the binder is the only component that touches both the AST and the catalog.
- **Side-effect requirement:** none beyond catalog reads under the transaction's snapshot (no locks taken on user data at bind time).
- **Critical contract details:** bind errors are ordinary `Result` errors distinguishable from syntax errors and from execution errors — 6.1 maps the three classes to different client messages. `BoundStatement`'s internals are private.

### `Plan` / `Explain`

```
Result<Plan> PlanStatement(const BoundStatement&, Catalog&, Transaction&);
std::string  Explain(const Plan&);
```

- **What it does:** applies the heuristic rules and emits an executable plan; `Explain` renders the operator tree as deterministic text — one node per line, indented children, scans naming table/index and bound openness.
- **Why it exists:** `Explain` is the only window the harness (and you, debugging) has into plan choice; plan-shape rows parse it.
- **Side-effect requirement:** pure aside from catalog reads.
- **Critical contract details:** identical inputs produce identical `Explain` output (deterministic planning — no hidden randomness). Exact format is yours; it freezes when the plan-shape fixtures are checked in.

### `Execute` / `ResultSet`

```
Result<ResultSet> Execute(Plan&, Transaction&);

class ResultSet {
  Result<std::optional<Row>> Next();   // row / done(nullopt) / error — the three states
  const Schema& OutputSchema() const;
  // Row: typed values per OutputSchema; exact representation is yours, readable by the harness shim
};
```

- **What it does:** runs the plan within the given transaction. Each `Next()` yields exactly one of: a row, exhaustion, or an error. For DML, the result set yields the affected-row count by a convention you fix and document.
- **Why it exists:** this is the engine's entire query surface — 6.1's server and 6.3's load generators sit directly on it. The signature *is* the three-state contract, public.
- **Side-effect requirement:** all reads honor the transaction's snapshot and isolation level; all locks acquired during execution follow 4.1 discipline and are released by commit/abort, never by the executor unilaterally (strict 2PL). After exhaustion, error, or early destruction of the `ResultSet` (LIMIT abandons the pipe), no buffer-pool pins or page latches remain held — the leak-detector row enforces this.
- **Critical contract details:** after `Next()` returns an error, subsequent calls keep returning error (sticky), and the transaction is left in a state where `Abort` is legal and clean. Deadlock-victim and write-write-conflict outcomes surface here as distinguishable error values. Execution is single-threaded per statement; concurrency is across sessions (Tier IV's domain).

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS — binder battery, operator fixtures, bounds fuzz, halloween-update, rc-recheck, plan-shape battery, leak rows — ASan/UBSan green; TSAN green on the scripted-interleaving rows.
2. The differential gauntlet: thousands of seeded statements vs SQLite, zero divergences, seeds recorded in RESULTS.md.
3. The `next()` contract document checked in before Part 2's operators, with any later amendments annotated.
4. `RESULTS.md` published, including the pull-vs-push paragraph; a stranger can rerun everything.

## Principal-engineer traps (no solutions)

- **The Halloween problem.** `UPDATE t SET k = k + 10` driven by an index scan on `k` revisits its own writes — non-termination or multi-updates. The known defenses are breaking the pipeline or tracking touched RIDs; what bites people is choosing per-statement *which plans even have the hazard* and proving the defense costs nothing on plans that don't.
- **Skipping the recheck.** Under Read Committed, the row you waited on is not the row you saw. Re-fetch the current version and re-evaluate the predicate after every lock wait on the write path — skip it and you silently update rows that no longer qualify. The harness's scripted row exists because this bug produces *plausible* wrong answers that random single-session testing never catches.
- **Bound openness on the range scan.** Compiling `k >= a AND k < b` (and `>`, `<=`, and the equality-as-degenerate-range case) onto the 2.1 iterator is four corner cases per predicate shape. Unit tests check the corners you remembered; the bounds fuzz checks the ones you didn't. Budget for it to find something.
- **The three-state `next()` is the no-exceptions tax — design it once.** Retrofit it after three operators exist and you'll rewrite all three; let one operator swallow a child's error state and the suite goes red somewhere far away. The paper design in Part 2 is the cheapest hour of the phase.
- **Resource leaks on the early exits.** LIMIT abandons a running join; a deadlock victim dies mid-scan with pins held three operators deep. Every operator's close-after-error path is load-bearing, and only the leak-detector rows make these bugs visible before 6.2 makes them fatal.
- **Smearing transaction logic upward.** The moment a Filter or Join "just checks visibility once more to be safe," you have two sources of truth and an unfalsifiable scan layer. Visibility and locking live in the scans and write operators — one layer, auditable.

## What you hand back for review

1. Implementation + the `next()` contract doc + the full harness table with gauntlet sizes and seeds.
2. One sentence per trap: did it bite, how resolved.

Review attack: "your UPDATE's index scan returns a row; narrate everything until the new version exists — locks, recheck, version write, index maintenance, in order," and "defend pulling rows here after pushing vectors in quackdb — when is each wrong?" Then Tier VI: the server.

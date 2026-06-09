# Tier IV · Phase 4.1 — Transaction manager & 2PL lock manager (scaffold)

**Deliverable of this phase:** a `TransactionManager` + `LockManager` whose committed histories pass an offline conflict-serializability check over thousands of randomized concurrent transactions, plus scripted deadlock scenarios detected within a bound and resolved by a deterministic victim policy.
**What you'll own afterward:** the ability to reason about transaction-lifetime concurrency — who waits on whom, why a system hangs versus deadlocks versus livelocks, and what "the commit returned" is actually allowed to mean — at the level where you can debug a production lock pileup from a waits-for dump instead of restarting the server.
**Calibration:** 🔨 (lock escalation: 🧩)
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is a logical concurrency-control layer, not a data-structure protection layer.** Phase 2.2 settled the latch: a microsecond-scale mutex protecting the physical integrity of a page or node, held for the duration of one structure operation, invisible to the user. What you build now is the other thing that word "lock" means in a database: a **logical lock** on a *named object* (a table, a row), held for the **lifetime of a transaction** — milliseconds to minutes — with a request *queue*, fairness, upgrade semantics, and deadlock as first-class design concerns. A latch never deadlocks if your code is correct; logical locks deadlock when the *workload* is adversarial, no matter how correct your code is, and the system must detect and break that itself.

This is also **not MVCC**. There are no versions here, no snapshots, no "readers don't block writers" — under pure strict 2PL, readers absolutely block writers and that is the point. Phase 4.2 builds the versioned alternative; 4.3 composes the two. If you catch yourself storing old tuple values or timestamps, you've drifted a phase ahead.

You're done with the framing when you can say: the deliverable is a **transaction-scoped lock table with queueing and a deadlock detector**, not a faster latch and not a version store.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

You'll be the kind of engineer who can look at a stalled system and distinguish *blocked* (waiting in a lock queue, will resolve), *deadlocked* (a cycle, needs a victim), and *starved* (a fairness bug in grant ordering) — and who knows that "commit" is a durability event, not a function return, so lock release is sequenced against the WAL, not against the caller.

### Why "from scratch" is the right call here

Use a DBMS's lock manager as a black box and these stay opaque forever:

- Why Postgres sometimes aborts your transaction with `deadlock detected` and sometimes just makes you wait 40 seconds — and why both are correct.
- Why `SELECT ... FOR UPDATE` of two readers planning to write deadlocks them symmetrically (the upgrade trap, which you will build and then script as a test).
- Why intention locks exist at all — why "lock the table" and "lock a row in the table" must be made *comparable* in one compatibility matrix or table scans race row updates.
- Why a connection dying mid-transaction can wedge an entire application (Phase 6.1 will make you clean this up; here you build the `unlock_all` path it depends on).
- Why releasing locks "at commit" is ambiguous until you've wired commit to a group-commit fsync and felt the recoverability hole.

### What carries forward to later tiers

- **Phase 4.3** implements Read Committed and Repeatable Read *as policies over this lock manager* — RC is "release S-locks early," RR is "hold them"; the lock manager itself doesn't change.
- **Phase 5.3** — the Volcano executor acquires these locks in the scan path: IS/IX at the table before S/X at rows. The hierarchy you define here is the one the planner targets.
- **Phase 6.1** — session death must drive `abort`, which must drive `unlock_all`; the orphan-lock invariant scan in the chaos soak is checking *this phase's* cleanup path.
- **Phase 6.3** — the isolation-level ablation benchmarks run directly on the RC/RR/SI machinery this phase and 4.2 provide.
- The **waits-for graph + cycle detection** pattern recurs anywhere resources queue: buffer-pin ordering, distributed lock services, even build systems. You'll recognize the shape.

### What good looks like

- The lock table's public surface is small: acquire (by mode, by object), release-all, and a detector hook. If callers can observe queue internals, the design leaks.
- You can answer without running code: *"Two transactions both hold S on the same row and both request X — what happens, and why does FIFO grant ordering not save you?"* and *"Why can't I release this txn's locks the moment `commit()` is called?"*
- At quiesce (no active txns), the lock table is empty and the waits-for graph has no edges — the harness checks this after every scenario.
- Deadlocks are detected within the configured detection interval plus one pass, never "eventually."
- Victim selection is deterministic given the same history — the scripted scenarios assert *which* txn dies.
- No thread ever sleeps while holding the lock-table latch; TSAN and the throughput floor will both tell on you.

### Why this is the shape of the deliverable

Concurrency bugs here are silent in exactly the worst way: a wrong grant produces a non-serializable history that *looks* fine — every individual read and write succeeded. So the artifact is a library plus an **offline checker that the engine never sees**: every transaction logs its read/write sets with global sequence numbers, and after the run a separate program builds the precedence graph over committed transactions and asserts acyclicity. Deadlock correctness is the dual problem — the bug is a hang, not a wrong answer — so it gets scripted scenarios with known cycles and time bounds instead.

## Exam questions this phase targets (build-proven)

1. Implement strict 2PL with S/X/IS/IX/SIX such that the harness's 10k-randomized-txn run produces an acyclic precedence graph (the `conflict-serializability` rows PASS).
2. Implement waits-for deadlock detection such that the scripted 2-txn, 3-txn-ring, and S→X-upgrade deadlocks are each detected within the bound and the named victim's abort demonstrably unblocks the survivors.
3. Demonstrate that commit-time lock release is sequenced after WAL durability: the harness's crash-at-commit row must show no transaction whose locks were released but whose commit record is absent.

## Prerequisites — concepts this phase uses

Audience: a strong C/C++ systems programmer who has never built a database. Recognize each term and the role it plays. Do NOT look up implementations — implementing them is the build.

### Transaction lifecycle

- **transaction (txn)** — a unit of work with all-or-nothing semantics: it `begin`s, does reads/writes, then either `commit`s (all effects survive) or `abort`s (no effects survive). The unit at which locks are scoped in this phase.
- **commit point** — the instant a transaction's effects become guaranteed. In chronos this is defined by Phase 3.1: the moment the txn's commit record is durable on disk (group-commit fsync returned), *not* the moment `commit()` is invoked.
- **strict 2PL (two-phase locking)** — the locking *discipline*: a txn acquires locks as it goes and releases **none of them until commit/abort** ("strict" = even the growing/shrinking distinction collapses to "hold everything until the end"). Its role: it's the classical sufficient condition for serializable, recoverable histories. The discipline is a rule about *when* release happens; enforcing it is the build.

### Lock modes and the granularity hierarchy

- **S / X locks** — shared (many readers) and exclusive (one writer) modes on a logical object. The two-by-two compatibility core every other mode extends.
- **intention locks (IS / IX / SIX)** — modes taken on a *coarser* object (the table) to advertise that finer locks (on rows) exist or will exist below it: IS = "I hold S somewhere below," IX = "I hold X somewhere below," SIX = "S on the whole thing plus IX below." Their role: they make "lock the whole table" and "lock one row" comparable in a single compatibility check, so a table-scanner and a row-updater conflict at the table level without examining every row. Canonical source, worth reading at vocabulary level: Gray et al., *Granularity of Locks and Degrees of Consistency in a Shared Data Base* (1976).
- **compatibility matrix** — the table saying which mode pairs may be held simultaneously on one object. You will define yours in the spec'd order (IS/IX/S/SIX/X) and the harness probes every cell.
- **lock upgrade** — a txn holding S on an object requesting X on the same object. A distinct request class, not "release then reacquire" (which would break 2PL) and not an ordinary X request (which would queue behind other waiters forever).
- **lock escalation** 🧩 — replacing many row locks held by one txn under a table with a single coarser table lock, to bound lock-table memory. A policy decision (when to trigger, what to do when escalation itself conflicts), not a new mechanism.

### Deadlock

- **waits-for graph** — a directed graph with a node per active txn and an edge T1→T2 when T1 is waiting for a lock T2 holds. Its role: a cycle in this graph *is* a deadlock, definitionally.
- **deadlock detection vs. prevention** — detection lets cycles form and breaks them (a background detector + victim abort, what chronos builds); prevention (wound-wait, wait-die timestamp schemes) refuses some waits up front. Know the distinction; build detection.
- **victim selection** — the policy choosing which txn in a cycle aborts. Real engines use cost heuristics (youngest, least-work-done); the contract here is only that yours is documented and deterministic.

### Serializability theory (the oracle's vocabulary)

- **conflict-serializability** — a concurrent history is conflict-serializable iff it's equivalent to *some* serial order under conflicting-operation reordering rules. The correctness bar for this phase.
- **precedence graph** — node per committed txn, edge Ti→Tj when an operation of Ti conflicts with (and precedes) one of Tj on the same object (w-w, w-r, r-w). Acyclic ⟺ conflict-serializable. This is what the offline checker builds — from logs, never from engine state.

## How you know you're aligned (the cross-check)

You never build this blind and find out at the end. The harness in `test/txn_lock_manager/` is the continuous oracle, and its core is **deliberately independent of your engine**: during every run, each transaction's reads and writes are logged with global sequence numbers to a flat file; after the run, a standalone checker — which links against none of your lock-manager code — builds the precedence graph over committed txns and asserts acyclicity. You cannot fool it by being subtly wrong in a self-consistent way, because it doesn't share your definitions; it only shares the log format.

The deadlock rows are scripted, not randomized: the harness constructs known cycles (two-txn, three-txn ring, symmetric S→X upgrade) with deterministic interleavings, then asserts (a) detection within the configured interval plus one detector pass, (b) the documented victim is the one aborted, (c) the surviving txns actually proceed to commit — proving the abort released the cycle, not just reported it.

The harness touches only the public surface below — begin/commit/abort, the lock calls, and the detector hook. Queue layout, grant bookkeeping, graph representation: invisible, yours. Build a part, run the suite, watch rows flip SKIP→PASS.

## The build, in parts (each gated independently by the harness)

### Part 1 — Transaction lifecycle wired to the WAL 🔨
`begin`/`commit`/`abort` against the Phase 3.1 `WalManager`. The load-bearing contract: `commit()` returns only after the commit record is **durable** (the group-commit fsync has covered it), and abort drives the undo path. **Harness rows:** lifecycle state machine; crash-at-commit recoverability probe (kill between "commit called" and "fsync returned" — the txn must be absent after recovery, and its locks must not have been observably released to a concurrent txn before durability).

### Part 2 — The lock table: S/X with strict 2PL queueing 🔨
The hash-to-queue structure (built on the Tier 0 allocators; the 2.4 skiplist is available where ordered iteration over held locks pays). S and X modes, FIFO-with-compatibility grant ordering, blocking waits, release-all at commit/abort. **Harness rows:** compatibility probes for S/X; blocked-writer/blocked-reader sequencing; the randomized-txn conflict-serializability run at row granularity; quiesce-empty check.

### Part 3 — The hierarchy: intention modes and escalation 🔨/🧩
IS/IX/SIX on tables above S/X on rows; the full 5×5 compatibility matrix; upgrades as a distinct priority class. 🧩 Lock escalation: a documented row-count threshold per (txn, table) that swaps fine locks for a coarse one. **Harness rows:** every matrix cell probed; table-scan-vs-row-update conflict detected at the table level; upgrade rows; escalation row (memory bound respected, post-escalation conflicts still correct).

### Part 4 — Deadlock detection and victim selection 🔨
The waits-for graph, maintained on every grant/release/abort path; a background cycle detector with a configurable interval (plus the deterministic single-pass hook the harness uses); a documented, deterministic victim policy. **Harness rows:** the three scripted cycles; phantom-deadlock probe (no abort when no cycle exists); a mixed soak where randomized txns + injected cycles run together and the serializability checker still passes over survivors.

### Part 5 — The harness as a published artifact 🔨
Every row green, including TSAN and ASan/UBSan builds. Write `RESULTS.md` from `templates/RESULTS.md`: the passing table, the deadlock-detection latency numbers, your victim policy and escalation threshold stated as decisions, and a paragraph on the nastiest interleaving you had to debug. Publish so a stranger can rerun.

## API contract (what the harness links against)

All fallible calls return `Result<T>` (no exceptions in this codebase). Everything below is the **public surface** — the only thing the harness touches. PRIVATE INTERNALS — queue node layout, grant-set representation, the waits-for graph's storage, latch placement inside the lock table — are deliberately unspecified; designing them is the work.

### `TransactionManager`

```cpp
auto begin() -> Result<TxnId>;
auto commit(TxnId txn) -> Result<void>;
auto abort(TxnId txn)  -> Result<void>;
```

- **What it does:** allocates monotonically increasing txn identities; `commit` makes the transaction's effects permanent; `abort` erases them.
- **Why it exists:** every engine has exactly one component that owns "what transactions exist and what state is each in" — the lock manager, the WAL, and (in 4.2) the version store all key off it.
- **Side-effect / durability requirement:** when `commit` returns success, the commit record is on durable storage (Phase 3.1 group-commit semantics) **and** all the txn's locks are released — in that order. When `abort` returns, all effects are undone via the 3.3 undo path and all locks are released. Both are terminal: any later operation by that TxnId is an error.
- **Critical contract details:** thread-safe; `commit` blocks for the group-commit window (this is the latency the 3.1 knob trades). Double-commit/double-abort/unknown-TxnId → error result, never UB.

### `LockManager`

```cpp
enum class LockMode { IS, IX, S, SIX, X };
auto lock_table(TxnId txn, TableId table, LockMode mode) -> Result<void>;
auto lock_row(TxnId txn, TableId table, Rid rid, LockMode mode) -> Result<void>;  // S or X only
auto unlock_all(TxnId txn) -> Result<void>;
```

- **What it does:** grants the requested mode when compatible with all current holders (per the matrix in Part 3), otherwise **blocks** the calling thread until granted or until the txn is chosen as a deadlock victim. A request by a txn already holding a weaker mode on the same object is an upgrade.
- **Why it exists:** the single serialization point for logical conflicts; everything 4.3 calls "an isolation level" is a release policy over these calls.
- **Side-effect requirement:** after a successful return the lock is held until `unlock_all` — strict 2PL means *no* public per-object unlock exists for txn-duration locks (this absence is deliberate; the harness checks that early release is impossible to observe). `unlock_all` is called only by commit/abort paths and must wake every waiter whose request becomes grantable.
- **Critical contract details:** if the txn is selected as a deadlock victim while blocked, the call returns `ErrorCode::DeadlockVictim` — the caller (txn manager / harness) then aborts it. Row locks require the appropriate intention mode already held on the table, else `ErrorCode::HierarchyViolation`. Waiting must be FIFO-fair among compatible classes (a documented upgrade-priority exception is allowed and must be stated in RESULTS.md). Never holds the lock-table latch across a wait.

### Deadlock detector hook

```cpp
auto run_detector_once() -> Result<std::optional<TxnId>>;  // victim chosen this pass, if any
```

- **What it does:** one synchronous detection pass over the waits-for graph; returns the victim it selected (and marked), if a cycle was found.
- **Why it exists:** the production detector is a background thread on an interval; the harness needs a deterministic way to say "detect now" so scripted scenarios don't depend on timing.
- **Side-effect requirement:** a returned victim has been signalled (its blocked lock call returns `DeadlockVictim`); the pass itself must not block lock traffic for its full duration.
- **Critical contract details:** returning `nullopt` on an acyclic graph is mandatory — the phantom-deadlock row aborts you for false positives.

### Op-log emission (for the offline checker)

```cpp
// Every read/write the workload performs is logged: (seq, txn, op, table, rid, commit/abort marker).
// Format frozen in test/txn_lock_manager/oplog_format.md; emission is a harness-provided shim
// around your public calls — your engine does not implement or see the checker.
```

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS — compatibility matrix, lifecycle, upgrade, escalation, the three scripted deadlocks, phantom-deadlock probe, crash-at-commit probe — under ASan/UBSan **and** TSAN builds.
2. The randomized run: ≥ 5,000 committed concurrent txns, offline precedence graph acyclic, lock table empty at quiesce; repeated across ≥ 10 seeds.
3. Deadlock detection latency reported (bound: interval + one pass); victim determinism shown across reruns of each scripted scenario.
4. `RESULTS.md` published; a stranger can rerun everything.

## Principal-engineer traps (no solutions)

- **Never sleep holding the lock-table latch.** The latch protects the *structure*; a condition-wait protects the *request*. Conflate them and every blocked txn serializes the entire database — the throughput floor and TSAN will both expose it, but only if you look.
- **"Release at commit" means release after the commit record is durable.** Releasing when `commit()` is *called* — before the group-commit fsync returns — lets another txn read your txn's writes and commit *first*; crash between the two and recovery produces a history that never happened. The crash-at-commit row exists for exactly this.
- **S→X upgrades deadlock two readers symmetrically.** Two txns hold S, both request X, both wait on each other — forever, if upgrades sit in the ordinary queue. Upgrades are a distinct priority class with their own conflict rule; treating them as plain X requests is the most common silent hang in student lock managers.
- **Waits-for edges must be maintained on every path — including abort.** A stale edge (waiter granted or aborted, edge not removed) produces phantom deadlocks and innocent victims; a missing edge (added on block but not on upgrade-block, or dropped on the wrong wake) produces undetectable hangs. The abort path is where people forget.
- **Escalation that conflicts is still a lock request.** Escalating to a table lock while another txn holds incompatible row locks under it must wait or fail by the same rules — escalation is not a superpower.

## What you hand back for review

1. Implementation + the full harness table (all three sanitizer builds) + detection-latency numbers + your written victim/escalation/fairness policy from RESULTS.md.
2. One sentence per trap above: did it bite you, and how did you resolve it?

Review is principal-engineer style: the interview attack on your queue design ("walk me through two upgraders and a waiting X"), any overstated claims, and the seam check — is `unlock_all` actually callable from the 6.1 session-death path? Then we advance to Phase 4.2.

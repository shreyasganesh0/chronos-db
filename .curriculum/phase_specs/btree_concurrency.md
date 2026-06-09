# Tier II · Phase 2.2 — Concurrent B+Tree: crabbing & OLC (scaffold)

**Deliverable of this phase:** the Phase 2.1 `BPlusTree` made safe for N concurrent threads two ways — pessimistic latch crabbing, then optimistic lock coupling with version counters and restart — with epoch-based reclamation for deleted pages, TSAN-clean under an N-thread differential fuzz, plus a shipped throughput table: crabbing vs OLC at 1 and 8 threads.
**What you'll own afterward:** the latch-vs-lock distinction at reflex level, and your first real deferred-memory-reclamation design — the exact problem that returns as vacuum in Phase 4.2 — so that when a concurrent index misbehaves you debug it as "which latch protocol invariant broke," not "threads are scary."
**Calibration:** 🔨 (B-link trees: 📖)
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; latch placement, version-counter layout, and epoch machinery are yours.

## What this is — and is not

**This is physical synchronization, not transaction isolation.** A *latch* protects the bytes of a page from torn concurrent access for the microseconds a thread is restructuring it. A *lock* (Phase 4.1) protects the logical content of data for the lifetime of a transaction — milliseconds to minutes — with deadlock detection, queues, and modes. This phase builds latches only. Two transactions latching the same page in sequence is normal and constant; two transactions *locking* the same row is a conflict the lock manager must arbitrate. If you find yourself adding "transaction ids" or "wait queues with fairness policies" to this phase, you have wandered into Tier IV — stop.

This distinction is THE conceptual point of the phase, and it is the single most reliable interview discriminator between people who have built a database and people who have read about one. After this phase you must be able to give the latch-vs-lock answer cold: duration, what's protected, deadlock discipline (latches: avoid by ordering, never detect; locks: detect and abort), and where each lives in the engine.

It is **not** a rewrite of 2.1. Same structure, same API, same walker. If your concurrency story requires changing what the tree *is*, your 2.1 design had a latent flaw — find it first.

You're done with the framing when you can say: the deliverable is *microsecond-scale structural protection for one index*, not *isolation between transactions*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
Designing and *arguing* a latch protocol: stating its invariant ("no thread ever holds X on a node whose ancestor it has released, unless the child is safe"), proving deadlock-freedom by ordering, and debugging violations from a TSAN report or a torn fuzz log. This is the capability that generalizes — every concurrent structure you ever touch is a latch protocol plus an argument.

### Why "from scratch" is the right call here
- **Phase 4.1's lock manager is built by someone who knows what locks are *not*.** Engineers who skipped latches build lock managers that secretly do latching (and collapse), or latch protocols that secretly do locking (and crawl).
- **Phase 4.2's vacuum is this phase's reclamation problem wearing MVCC clothes** — "when is it safe to free something a concurrent reader might still see?" is answered with epochs/horizons both times. Earn the pattern here, on pages, where the failure is an ASan report instead of a corrupted snapshot.
- **OLC failures are invisible without having built the version protocol.** A missing re-validation reads a torn node and *usually* gets away with it. You can only place the validation points correctly if the protocol is yours.
- **Performance intuition.** "Latch coupling is fine, root contention is the killer, optimism wins on reads" is folklore until you've measured your own crabbing-vs-OLC table.

### What carries forward to later tiers
- **Phase 4.1:** the latch-vs-lock line you draw here *is* the design boundary of the lock manager — and the lock table's internal buckets are protected by exactly these latches.
- **Phase 4.2:** epoch-based reclamation returns as vacuum's oldest-active-snapshot horizon; same shape, bigger stakes.
- **Phase 4.3:** phantom tracking rides the 2.1 iterator contract *as amended this phase* — what a scan may assume under concurrent splits is decided here, permanently.
- **Phase 6.3:** the crabbing-vs-OLC throughput method (fixed workload, thread sweep, report the table) is the benchmark discipline Tier VI industrializes.
- **📖 B-link trees** (Lehman & Yao) stay whiteboard-level: you must be able to explain how a right-link plus a high key lets readers tolerate concurrent splits with almost no latching, and why you chose not to build it.

### What good looks like
- A written protocol note per mode: the held-latch invariant, the deadlock-freedom argument (top-down ordering, no upgrades), and the OLC validation points. Half a page each.
- "Safe node" is defined for *both* directions — won't split on insert paths **and** won't merge/redistribute on delete paths — and the definition appears verbatim in your protocol note.
- Zero TSAN findings at full fuzz strength, not "only benign ones."
- OLC restarts are counted, exposed in `Stats`, and bounded under the hot-key seed — restart *storms* are a failed design, not bad luck.
- Retired pages are provably reclaimed: at quiesce, `pages_retired == pages_reclaimed`, and ASan never sees a reader dereference a freed frame.

### Why this is the shape of the deliverable
Concurrency bugs are probabilistic: a protocol that is wrong once per million interleavings passes every demo and dies in production. So the artifact is volume + independent judgment: N threads, per-thread op logs replayed against an oracle at quiesce, the 2.1 walker re-asserting structure, TSAN as a non-negotiable gate, and a throughput table because a "correct" concurrent tree that scales worse than a global mutex is a failed phase. No single one of these catches everything; together they corner the bug classes this phase actually has.

## Exam questions this phase targets (build-proven)
1. Implement latch crabbing whose "safe" test covers both split and merge, such that the delete-heavy N-thread fuzz never deadlocks (harness watchdog row).
2. Implement OLC — version validation on every optimistic read, restart on conflict, backoff under contention — such that the hot-key seed completes with a bounded restart count.
3. Implement epoch-based reclamation such that the ASan build of the concurrent delete fuzz is green and `pages_retired == pages_reclaimed` at quiesce.
4. Produce the crabbing-vs-OLC table at 1 and 8 threads and explain every entry — including any cell where crabbing wins.
5. 📖 Whiteboard B-link trees: the right-link/high-key mechanism, and the trade you made by not building it.

## Prerequisites — concepts this phase uses

### Synchronization vocabulary
- **latch vs lock** — the database-specific terminology split: latch = short-duration physical mutual exclusion on a memory/page object, no deadlock detection, acquired/released within one operation; lock = long-duration logical protection of data content with modes, queues, and deadlock handling, held to transaction end. The canonical treatment is Goetz Graefe, "A Survey of B-Tree Locking Techniques" (ACM TODS, 2010) — read §1–2 for vocabulary; the protocols are the build.
- **reader-writer latch** — shared mode for readers, exclusive for writers. You'll build or wrap one per page frame. Know the standard hazards by name: writer starvation, upgrade deadlock.
- **latch coupling / crabbing** — the descent protocol: hold the parent's latch until the child's is acquired, releasing ancestors once the child is *safe*. The crab walks claw-over-claw; the tree is never traversed unlatched.
- **safe node** — a node that will not propagate structural change to its ancestors under the current operation: won't split (insert) and won't underflow into redistribution/merge (delete). The definition is operation-dependent — that asymmetry is a named trap below.
- **optimistic lock coupling (OLC)** — readers take no latches; they read a per-node version counter, read the node, and re-read the counter to validate; writers bump the version under an exclusive latch. Mismatch ⇒ restart the descent. Vocabulary source: Leis, Haubenschild & Neumann, "Optimistic Lock Coupling: A Scalable and Efficient General-Purpose Synchronization Method" (IEEE Data Eng. Bulletin, 2019) and the precursor idea in "The ART of Practical Synchronization" (DaMoN 2016). Read for the *idea and its argument*, not as an implementation recipe.
- **seqlock pattern** — the OS-kernel ancestor of version validation: odd version = write in progress; readers retry. Useful mental model for where validation points must go.
- **linearizability** — the correctness criterion for single-key operations: each op appears to take effect atomically at some instant between invocation and return (Herlihy & Wing, 1990). The harness's atomic-counter probes are spot checks of exactly this.

### Memory reclamation
- **the reclamation problem** — after a merge deletes a page, an optimistic reader holding no latch may still be about to dereference it. Freeing immediately is a use-after-free; never freeing is a leak. Every latch-free or optimistic structure has this problem.
- **epoch-based reclamation (EBR)** — threads announce the epoch they operate in; retired objects are freed only once no thread can still be in an epoch that could reference them. Background: Fraser's thesis (2004); the comparison paper to know of is Hart et al., "Performance of Memory Reclamation for Lockless Synchronization" (JPDC 2007). The role here: pages retired by merges go on a deferred list and return to the free list only when provably unreachable.
- **quiesce** — a harness-visible moment when all worker threads have stopped and drained: the state where the oracle replay, the walker, and the retired==reclaimed assertion run.

### Tools
- **TSAN (ThreadSanitizer)** — compiler-instrumented data-race detection (`-fsanitize=thread`). A TSAN report is a bug, full stop; "it's benign" is not an accepted review answer in this repo.
- **📖 B-link trees** — Lehman & Yao, "Efficient Locking for Concurrent Operations on B-Trees" (TODS 1981): every node gets a right-sibling link and a high key, letting readers chase a split sideways instead of latching against it. Conversant-level only.

## How you know you're aligned (the cross-check)

The harness in `test/btree_concurrency/` is the continuous oracle. Its independent judges:

1. **Per-thread op-log replay.** N threads run seeded op mixes against your tree, each logging `(key, op, result, thread-local seq)`. At quiesce the harness replays a per-key last-writer reconstruction into `std::map` and compares: final tree contents must match, and every logged lookup result must be *explainable* by some legal interleaving of the logged writes. The oracle never links your tree code.
2. **Single-key linearizability probes.** Threads hammer one key with read-modify-write-shaped op sequences alongside an atomic shadow counter; divergence means an op wasn't atomic.
3. **The 2.1 structural walker, re-run at quiesce** — concurrency must never buy structural corruption. Same binary, unchanged.
4. **TSAN gate** — the full fuzz under `-fsanitize=thread`, mandatory green. ASan build additionally gates the reclamation rows.
5. **Throughput rows** — fixed seeded workloads at 1 and 8 threads, both modes; the harness records the table for RESULTS.md (it gates on completion and sanity, not on hitting a magic number — the *table* is the deliverable).

The harness sees only the public API, the `Stats` counters, and quiesce hooks. Latch placement and epoch internals are invisible to it — by design.

## The build, in parts (each gated independently by the harness)

### Part 1 — Page latches + pessimistic crabbing 🔨
A reader-writer latch per buffer-pool frame (or wired into 1.3's frame table — your call), then crabbing for lookup/insert/delete with a both-directions safe test. Write the protocol note: held-latch invariant + deadlock-freedom argument. Harness rows: N-thread fuzz (read-heavy and delete-heavy seeds), deadlock watchdog, walker at quiesce, TSAN.

### Part 2 — Optimistic lock coupling 🔨
Per-node version counters; optimistic read descent with validation; writers latch + bump; restart with backoff on validation failure. Mode selected at construction. Harness rows: same fuzz under OLC, hot-key restart-bound row, linearizability probes, TSAN.

### Part 3 — Epoch-based reclamation 🔨
Merged/deleted pages retire to a deferred list; reclamation only past the epoch horizon; freed pages actually return to reuse (a leak that never reclaims fails the row). Harness rows: concurrent delete/scan fuzz under ASan, `retired == reclaimed` at quiesce, bounded deferred-list growth under sustained churn.

### Part 4 — The harness as a published artifact 🔨
Everything green at full strength; produce the throughput table (crabbing vs OLC × 1 and 8 threads, plus restart counts). RESULTS.md: the table, the protocol notes, and a paragraph on the nastiest race TSAN or the replay caught.

## API contract (what the harness imports)

The 2.1 surface carries over **unchanged in shape** — `Insert`, `Remove`, `Lookup`, `Seek`/`SeekFirst`, `Iterator`, `RootPage` — with the thread-safety class upgraded: every operation callable from any thread with no external synchronization. Additions:

### Construction with a mode

```cpp
enum class LatchMode { kCrabbing, kOLC };
static Result<BPlusTree> Create(BufferPool& pool, LatchMode mode);
static Result<BPlusTree> Open(BufferPool& pool, PageId root, LatchMode mode);
```

- **What it does:** selects the protocol for this instance's lifetime.
- **Why it exists:** the throughput table needs both protocols behind one surface; keeping crabbing alive also gives you a bisection tool when OLC misbehaves.
- **Critical contract details:** both modes must pass the *same* correctness rows; OLC is not allowed a weaker contract.

### `Stats` — observability surface

```cpp
struct Stats {
  uint64_t olc_restarts;      // descents restarted on version mismatch
  uint64_t pages_retired;     // retired to deferred reclamation
  uint64_t pages_reclaimed;   // actually returned for reuse
};
Stats GetStats() const;       // thread-safe; meaningful precisely at quiesce
```

- **What it does:** exposes the three counters the harness gates on.
- **Why it exists:** restart storms and reclamation leaks are invisible from return values; these counters make them observable without exposing internals.
- **Side-effect requirement:** at quiesce after the reclamation fuzz, `pages_retired == pages_reclaimed`.
- **Critical contract details:** counter maintenance must not become the contention point (the harness's 8-thread rows will tell you if it did).

### Iterator under concurrency

The Phase 2.1 iterator surface is unchanged; **its validity contract must be re-issued for this phase** (`docs/btree_iterator_contract.md`, amended): what a scan guarantees while concurrent splits, merges, and inserts land. The harness's mutate-while-scanning rows test exactly the amended document. Weaker guarantees than 2.1's are expected and acceptable — undocumented ones are not.

**PRIVATE INTERNALS — deliberately unspecified:** latch placement (frame vs node header), version-counter encoding (and whether it shares a word with the latch), epoch granularity and advancement policy, deferred-list structure, backoff schedule. The harness never inspects them; designing them is the phase.

## Acceptance criteria (phase-level "done")
1. Harness: all rows PASS — N-thread replay fuzz (all seeds, both modes), linearizability probes, walker at quiesce, deadlock watchdog. **TSAN build green at full fuzz strength; ASan build green on the reclamation rows.**
2. Throughput table shipped: crabbing vs OLC at 1 and 8 threads, with restart counts, in RESULTS.md.
3. Protocol notes (crabbing + OLC) and the amended iterator-validity contract published; RESULTS.md rerunnable by a stranger.

## Principal-engineer traps (no solutions)
- **"Safe" checked only against splits.** Insert-only thinking makes delete descents release ancestors they still need; the result is a deadlock or a corrupted merge that the delete-heavy seed *will* find. Safe means won't split **and** won't merge.
- **S→X latch upgrade is a deadlock machine.** Two threads holding S on the same node, both wanting X, wait forever. Restructure the protocol so upgrades never happen, rather than "handling" upgrade failure.
- **OLC restart storms.** Under a hot key, naive immediate retry turns the benchmark into livelock — throughput approaches zero while CPUs spin at 100%. Backoff is part of the protocol, not a tuning afterthought; the hot-key row gates on bounded restarts.
- **Validation in the wrong place.** Reading a child pointer from a node, *then* validating the parent's version, *then* following the pointer — every ordering but the right one has a window. Write down where every validation sits and why before trusting the fuzz.
- **Reclaiming on operation completion instead of epoch horizon.** "The delete returned, so free the page" ignores the optimistic reader mid-descent with no latch. There is no shortcut here — epochs or equivalent deferral, and ASan as the judge.
- **Iterator pins vs latches confusion.** A scan that holds latches across `Next()` calls blocks writers indefinitely (latches held for a lock's duration — the exact sin this phase exists to name). Your amended contract has to resolve this tension explicitly.

## What you hand back for review
1. Implementation + full harness table + throughput table + the two protocol notes + amended iterator contract.
2. One sentence per trap: did it bite, how resolved.

Review attack: the deadlock-freedom argument and the OLC validation-point placement — defended at the whiteboard, then we scaffold 2.3.

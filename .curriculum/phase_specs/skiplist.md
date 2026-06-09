# Tier II · Phase 2.4 — Concurrent skiplist (scaffold)

**Deliverable of this phase:** a latched, in-memory, concurrent `SkipList` — insert, lookup, range iteration; no delete, no persistence — TSAN-clean under a multi-thread differential fuzz, with a tower-height distribution check and a shipped memory-overhead-per-entry number.
**What you'll own afterward:** the ordered in-memory map Tier IV is built on — the 4.2 version store (keyed by RID) and a 4.1 lock-table substrate — plus the discipline of choosing a synchronization design you can fully argue over a fashionable one you can't.
**Calibration:** 🧩 — scoped build: correctness and a defensible design, not a research artifact. Budget it accordingly; this is the palate-cleanser phase (the build plan floats it to after ARIES).
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; tower layout, latch granularity, and arena strategy are yours.

## What this is — and is not

**This is an in-memory ordered map with probabilistic balance, protected by latches you can fully explain.** A skiplist keeps entries in one sorted linked list at the bottom level, with each entry owning a randomized "tower" of express-lane pointers above it; search drops down the towers in expected O(log n). No rotations, no rebalancing, no split/merge machinery — the structure self-balances by coin flips, which is exactly why it's the classic concurrent ordered map (LevelDB/RocksDB memtables are skiplists for this reason).

It is **explicitly not lock-free.** The famous skiplists (java.util.concurrent, Fraser's) are CAS-based, and blog posts will tempt you to imitate them. Resist. A lock-free skiplist's correctness argument involves marked pointers, helping protocols, and ABA reasoning — a multi-week research-grade detour this curriculum spends elsewhere (the WAL and ARIES are the heart of chronos, not this). A reader-writer-latched skiplist whose every interleaving you can argue at a whiteboard *beats* a folklore CAS sculpture you copied and can't defend. That trade — owned simplicity over borrowed sophistication — is itself the lesson, and you should be able to articulate it in review.

It is also **not durable and not a disk structure.** No pages, no buffer pool, no WAL. It lives in memory and dies with the process; in 4.2 that's precisely correct, because the version store is rebuilt from heap files + WAL on recovery, not persisted. And it is not 2.1 rebuilt — no byte-budget nodes, no walker. Different niche: 2.1 is the *durable* ordered index; this is the *transient* one.

You're done with the framing when you can say: the deliverable is *a latched in-memory ordered map sized for Tier IV's version store*, not *a lock-free showpiece and not another B+Tree*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
Scoping a concurrent component: choosing the simplest synchronization design that meets the actual requirement (Tier IV's), implementing it cleanly, and *measuring* what it costs — per-entry memory, contention behavior — instead of optimizing on vibes. The engineer who can say "I chose latched over lock-free, here is the requirement it meets and the ceiling it accepts" is more senior than the one who ships CAS code they can't explain.

### Why "from scratch" is the right call here
- **4.2's version store is this structure.** Per-tuple version chains hang off a map keyed by RID; when MVCC visibility misbehaves, you'll be debugging *this* code's iteration and insertion semantics. A borrowed map makes that debugging archaeology.
- **The probabilistic-balance argument must be yours.** "Expected O(log n) because tower heights are geometric" is one sentence to say and one harness row to verify — and only a builder knows what breaks it (a biased RNG, a shared one, a capped level done wrong).
- **Concurrent-iteration semantics get *decided* here.** What a range scan sees while inserts land concurrently is a contract you'll write; consuming code in 4.1/4.2 depends on knowing exactly how weak it is.
- **Memory overhead is a real cost in Tier IV.** Version stores hold millions of entries; pointer towers aren't free. Shipping the bytes-per-entry number forces the layout to be a decision, not an accident.

### What carries forward to later tiers
- **Phase 4.2 (MVCC):** the version store is a skiplist keyed by RID, value = version-chain head. Insert-if-absent and ordered traversal (vacuum walks it) are exactly this phase's surface.
- **Phase 4.1 (lock manager):** a lock-table substrate — ordered map from lock-object id to queue head — can ride the same structure; even if you bucket-hash instead, the latched-map design rep transfers.
- **Phase 2.2's oracle method** (per-thread op logs replayed at quiesce) gets its second rep here, cementing it as your default way to judge any concurrent structure.
- **The scoping judgment** — 🧩 means "reimplement the core, skip the research frontier" — recurs at 4.3 (SSI is 🧩) and 6.1 (protocol subset). This phase is where you practice honoring a calibration.

### What good looks like
- The whole thing is small — a few hundred lines — and the latch story fits in one paragraph you can recite: what's latched, in what mode, for which operation, and why no deadlock.
- Tower heights match the geometric expectation (your documented `p`): the harness's distribution row passes without tuning, because the RNG and level cap were right by construction.
- No shared mutable RNG. Level generation never serializes inserts.
- The bytes-per-entry number is shipped, explained (towers + latch + allocation overhead, at your `p` and key sizes), and defensible against "why not p=1/4?"
- The iterator contract is short, honest about its weakness relative to 2.1's, and says *why* the weakness is acceptable for the 4.2/4.1 consumers.

### Why this is the shape of the deliverable
The risks here are concurrency races and silent distribution skew — neither visible in unit tests. So the artifact is the 2.2 oracle method re-applied (N threads, per-thread logs, quiesce replay against `std::map`, TSAN gate) plus a statistical row: tower heights against the geometric expectation, because a broken RNG degrades search to O(n) while every correctness test stays green. The memory number is in the deliverable because Tier IV will pay it per version, millions of times.

## Exam questions this phase targets (build-proven)
1. Implement insert/lookup such that the N-thread quiesce replay matches `std::map` across all seeds, TSAN-clean.
2. Implement per-thread (or per-insert, but never shared-mutable) level generation such that the tower-height distribution row passes — and state the expected height and search-cost argument from memory.
3. Write the range-iterator contract under concurrent inserts, and defend at a whiteboard why it's weaker than 2.1's and why 4.2 doesn't care.
4. Report bytes-per-entry at your chosen `p` and justify the choice against `p = 1/4`.

## Prerequisites — concepts this phase uses

### Skiplist vocabulary
- **skiplist** — William Pugh, "Skip Lists: A Probabilistic Alternative to Balanced Trees" (CACM, 1990). Read it — it's short, friendly, and the single authoritative source; it gives the structure and the expected-cost argument. The concurrent latching design is *not* in the paper; that's your build.
- **tower / node level** — each entry's stack of forward pointers. An entry of height `h` appears in levels `0..h-1`; level 0 is the complete sorted list, higher levels are express lanes.
- **`p` (level probability)** — the coin bias: an entry reaches height `h+1` with probability `p` given height `h`, so heights are geometrically distributed. `p = 1/2` and `p = 1/4` are the standard choices; `p` trades pointer memory against search constant. Yours to pick and defend.
- **max height cap** — a fixed ceiling on tower height (with expected list size in mind) so layout and search can use a fixed-size head tower. Sizing it is a documented decision.
- **probabilistic balance** — no rebalancing ever happens; the *distribution* of heights keeps expected search at O(log n). This is why a broken or biased RNG is a performance-correctness bug invisible to functional tests.

### Concurrency vocabulary (recap from 2.2, applied smaller)
- **latched (blocking) vs lock-free (non-blocking)** — latched: threads exclude each other with reader-writer latches; simple to argue, can't livelock, can convoy. Lock-free: CAS-based progress guarantees, vastly harder correctness arguments (for the genre, see Herlihy & Shavit, *The Art of Multiprocessor Programming*, ch. 14 — as context for what you are deliberately *not* building).
- **latch granularity** — one big reader-writer latch vs finer schemes. Both can pass this harness; the contract below only fixes the thread-safety class, not the granularity. Coarse-and-correct is an acceptable starting point for a 🧩 phase; measure before refining.
- **per-thread RNG** — each thread owns its generator state (seeded distinctly). The role: level generation sits on the insert hot path, and a shared generator is both a serialization point and, if unsynchronized, a data race.
- **quiesce** — as in 2.2: all workers stopped and joined; where the replay oracle and distribution checks run.

### Tier IV preview (why the surface looks like it does)
- **version store (4.2)** — map from RID to a tuple's version-chain head; written by every update, read by every visibility check, swept in order by vacuum. The reason ordered iteration and insert-if-absent are in the contract.
- **RID as key** — Phase 1.1's (page, slot) id, encoded as bytes so that your comparator orders it sensibly. Keys here are arbitrary bytes precisely so RIDs and lock-object ids both fit later.

## How you know you're aligned (the cross-check)

The harness in `test/skiplist/` is the continuous oracle — the 2.2 method, re-applied:

1. **Per-thread op-log replay vs `std::map`.** N threads run seeded insert/lookup/scan mixes, logging `(key, op, result, seq)`. At quiesce the harness reconstructs expected contents in `std::map` (which shares no code with yours) and compares full contents and ordered iteration; every logged lookup must be explainable by the logged inserts.
2. **TSAN gate** — the full fuzz under `-fsanitize=thread`, mandatory. ASan/UBSan builds also run (arena bugs live here).
3. **Tower-height distribution row** — at quiesce over a large single-threaded insert run, observed heights must fit the geometric expectation for your documented `p` (a generous chi-square-style tolerance — it catches broken RNGs and capped-level bugs, not noise).
4. **Memory row** — the harness records `ApproxBytesPerEntry()` for the RESULTS.md table at fixed key/value sizes; it gates on the number being reported and self-consistent (within sanity bounds), not on a magic target.

Build a part, run the suite, watch rows flip SKIP→PASS. The harness sees only the public surface below — never the tower layout or latch placement.

## The build, in parts (each gated independently by the harness)

### Part 1 — Single-threaded skiplist 🔨
Towers, level generation, sorted insert, lookup, ordered iteration. Document `p` and the height cap. Harness rows: single-threaded differential vs `std::map`, tower-height distribution, ASan/UBSan.

### Part 2 — Latching 🧩
The concurrency design: reader-writer latching at your chosen granularity, per-thread RNG, and a written half-page latch note (what's held, when, why no deadlock). Harness rows: N-thread fuzz seeds (read-heavy, write-heavy, hot-range), quiesce replay, TSAN.

### Part 3 — Range iteration + the contract + the memory number 🧩
`Seek`-and-scan iteration concurrent with inserts; write `docs/skiplist_iterator_contract.md` stating exactly what a scan guarantees (and doesn't) while inserts land — weaker than 2.1's contract, deliberately, with the why. Implement `ApproxBytesPerEntry()`. Harness rows: mutate-while-scanning rows testing exactly the documented guarantees, memory row.

### Part 4 — The harness as a published artifact 🔨
All rows green at full strength. RESULTS.md: the table, the bytes-per-entry number with its breakdown, the height-distribution result, and a paragraph defending latched-over-lock-free in your own words.

## API contract (what the harness imports)

Spellings adjustable via the harness shim; semantics are not. Keys and values are byte strings (`Slice` views in, list-owned copies inside — the list owns its memory; how is private). `Result<T>` carries allocation failure; no exceptions exist in this codebase.

### `SkipList` — lifecycle and operations

```cpp
SkipList();                                   // or with your arena/config params
Result<bool> Insert(Slice key, Slice value);  // insert-if-absent; false if key exists
Result<std::optional<Slice>> Lookup(Slice key) const;
size_t Size() const;                          // entry count; exact at quiesce
```

- **What it does:** an ordered map over byte keys with insert-if-absent semantics. `Insert` copies key and value into list-owned storage; `false` means the key already existed (existing value untouched). `Lookup` returns a view of the stored value.
- **Why it exists:** insert-if-absent is the version-store idiom (4.2): first writer installs the chain head; later writers find it. Note there is **no `Remove`** — deliberately. Deletion drags in concurrent-reclamation machinery (2.2's epoch lesson) that the 4.2 consumer handles at a different level (vacuum, with its own horizon); adding erase here would un-scope the 🧩.
- **Side-effect requirement:** after `Insert` returns true, the entry is visible to every subsequently *started* lookup and scan from any thread; the list's level-0 order is total and consistent with your documented comparator at all times.
- **Critical contract details:** fully thread-safe — any thread, no external synchronization. The `Slice` returned by `Lookup` must remain valid for the list's lifetime (no erase makes this promisable — say so in your docs). If you support value overwrite as an extension, document it and flip the harness switch; insert-if-absent is the required minimum.

### `SkipList::Iterator` — ordered scans

```cpp
Result<Iterator> Seek(Slice lower_bound);     // first key >= lower_bound
Result<Iterator> SeekFirst();
bool   Iterator::Valid() const;
Slice  Iterator::Key() const;
Slice  Iterator::Value() const;
Result<void> Iterator::Next();
```

- **What it does:** forward iteration in key order from the seek point; same shape as 2.1's iterator on purpose (consumers learn one idiom).
- **Why it exists:** vacuum (4.2) sweeps the version store in order; range probes over a lock-table substrate (4.1) use `Seek`.
- **Side-effect requirement:** governed by **your iterator contract**: the required minimum is that a scan visits a superset of the entries present at `Seek` and never visits a key twice or out of order; whether concurrently inserted keys appear is yours to specify. The harness mutate-while-scanning rows enforce exactly the document.
- **Critical contract details:** state explicitly whether the iterator blocks writers while positioned (a latch held across `Next()` calls is 2.2's named sin — if your granularity implies it, the contract must own that and bound it). Weaker than 2.1's contract is expected; *undocumented* is a failed row.

### Observability

```cpp
size_t ApproxBytesPerEntry() const;           // amortized, at current contents
std::vector<size_t> DebugTowerHistogram() const;  // count of entries per height; quiesce-only
```

- **What it does:** the two numbers the harness's distribution and memory rows read.
- **Why it exists:** memory cost and probabilistic health are this phase's shipped claims; they need a surface.
- **Critical contract details:** `DebugTowerHistogram` may be O(n) and is only called at quiesce; `ApproxBytesPerEntry` should account for towers, latches, and allocation overhead honestly — the review will probe the breakdown.

**PRIVATE INTERNALS — deliberately unspecified:** node/tower memory layout, latch granularity and placement, arena vs per-node allocation (your 0.2 allocators are available), RNG choice and seeding, sentinel/head design, height-cap value. The harness never sees them.

## Acceptance criteria (phase-level "done")
1. Harness: all rows PASS — N-thread replay fuzz across all seeds, mutate-while-scanning rows, distribution row, memory row. **TSAN green at full fuzz strength; ASan/UBSan green.**
2. Bytes-per-entry shipped in RESULTS.md with a breakdown; latch note and iterator contract published.
3. RESULTS.md rerunnable by a stranger; the latched-over-lock-free paragraph written in your own words.

## Principal-engineer traps (no solutions)
- **Lock-free seduction.** Mid-phase, a blog post will convince you marked pointers are "not that bad." The calibration is 🧩 and the requirement is Tier IV's, not java.util.concurrent's. A latched list you can argue beats CAS folklore you can't — and the review *will* make you argue whichever you shipped.
- **The shared RNG.** One `std::mt19937` behind a mutex serializes every insert; one without a mutex is a TSAN finding and a skewed distribution. The distribution row and the write-heavy seed exist to catch both halves.
- **Distribution rot from the height cap.** A clamp applied wrong (or a `p` arithmetic off-by-one) silently fattens towers or flattens the list; functional tests stay green while search degrades toward O(n). That's the entire reason the histogram row exists.
- **Iterator guarantees by accident.** Whatever your scan does under concurrent inserts *is* your contract once 4.2 builds on it. Write the document first, then make the rows match it — not the other way around.
- **Latch held across `Next()` without owning it.** If your granularity means a positioned iterator blocks writers, that's a defensible scoped choice only if the contract says it out loud and 4.2's vacuum pattern can live with it.
- **Dishonest memory accounting.** Counting node payload but not allocator overhead or latch bytes makes the shipped number a fiction; Tier IV will discover the truth at a million entries.

## What you hand back for review
1. Implementation + full harness table + bytes-per-entry breakdown + the latch note and iterator contract.
2. One sentence per trap: did it bite, how resolved.

Review attack: the latch note's deadlock argument and the iterator contract's fitness for 4.2's vacuum. Then Tier IV begins, and this list starts holding versions.

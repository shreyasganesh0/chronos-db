# Tier II · Phase 2.1 — B+Tree, single-threaded (scaffold)

**Deliverable of this phase:** a disk-resident `BPlusTree` living on buffer-pool pages — insert with splits, delete with redistribution-then-merge, point lookup, and a range iterator over the leaf sibling chain — green on a 10M-op differential fuzz vs `std::map` and on an independent structural-invariant walker, plus two written contracts: iterator validity and duplicate-key policy.
**What you'll own afterward:** the ordered index everything downstream stands on. 2.2 adds latches to *this* tree, 4.3 hangs phantom tracking off *this* iterator, 5.3 compiles WHERE bounds into *this* scan — and you'll be able to debug a corrupted range result at the level of "which separator is stale," not "the index is broken."
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals — node layout, split-point choice, where the level pointers live — are yours.

## What this is — and is not

**This is a disk structure, not a data structure.** A B+Tree node here is a pinned buffer-pool page (Phase 1.3) with a slotted interior (the Phase 1.1 idea, re-applied inside an index node). Fan-out is not a template parameter or a count — it is a *byte budget*: a node holds as many variable-length keys as fit in one page, and "full" means "the next key doesn't fit," not "N == ORDER." Every traversal step is a pin; every modification dirties a page the buffer pool will write back.

It is **not** `std::map` and not an in-memory pointer tree. If your nodes hold `std::string` keys, heap-allocated child pointers, or anything that doesn't survive being written to disk and read back, you've built the wrong artifact. `std::map` appears in this phase only on the *other* side of the differential harness, as the oracle your tree must agree with.

It is also **not yet concurrent and not yet transactional**. Single thread, no latches beyond the buffer pool's own internal safety, no logging. Those are Phases 2.2 and 3.x, and they will be brutal enough without fighting structural bugs at the same time.

You're done with the framing when you can say: the deliverable is *an on-page ordered index whose fan-out is a byte budget*, not *a balanced tree in memory*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
The ability to reason about a mutable on-disk tree as a set of invariants — sorted keys, uniform leaf depth, parent separators consistent with leaf contents, sibling chain intact — and to debug violations from raw page bytes. When a range scan three tiers from now skips a key, you want to reach for the walker and find the stale separator, not re-read your insert code for the tenth time.

### Why "from scratch" is the right call here
Use a library B+Tree and the following go permanently opaque:
- **2.2 is impossible.** Latch crabbing and OLC are defined in terms of *your* split/merge mechanics — "safe node" means "won't split or merge under *your* byte budget." You can't latch a structure you didn't shape.
- **4.3's phantom story has nowhere to live.** Next-key locking rides the iterator's notion of "the key after"; that notion is set by your leaf chain and your validity contract, written this phase.
- **Delete bugs are silent for months.** A tree that never merges still answers queries correctly — it just degrades. Only someone who built redistribution/merge knows what the degradation looks like and where to assert.
- **5.3's planner needs honest numbers.** Index-vs-seqscan heuristics rest on knowing what a tree probe actually costs in page pins — which you'll know because you counted them.

### What carries forward to later tiers
- **Phase 2.2** adds reader-writer latches and OLC to *exactly this structure* — same API, same nodes. Anything ambiguous in your node lifecycle now becomes a race then.
- **The iterator-validity contract is a load-bearing seam.** What you write in Part 4 becomes 2.2's latching contract for scans (what the iterator may hold across `Next()`) and 4.3's phantom-tracking substrate (next-key tracking is defined over the iterator's view of the leaf chain). Write it as if two future phases will hold you to every word — they will.
- **Phase 5.3** compiles `WHERE k >= a AND k < b` into `Seek(a)` + iterate-until-`b`. The IndexScan operator is a thin shim over this phase's iterator.
- **Phase 5.1** stores secondary-index roots in the catalog; every index it describes is an instance of this tree.
- **The walker discipline** — an independent binary that reads raw pages against a frozen format note — is the same pattern as 3.1's `waldump`. You're learning to ship verifiers that can't share bugs with the code they verify.

### What good looks like
- Every public operation pins O(depth) pages and unpins all of them on every path, including error paths. The harness's pin-leak check stays at zero.
- You can answer at a whiteboard, without code: *"Why does a leaf split copy the separator up but an internal split push it up?"* and *"In what order do you try redistribution vs merge on underflow, and why?"*
- The walker passes not only at the end of the fuzz but at every interleaved checkpoint — your tree is never transiently invalid between operations.
- Your duplicate-key policy and iterator-validity contract each fit on half a page and answer every edge case the harness probes, with no "undefined, probably fine" gaps.
- A key near the page-size budget either inserts correctly or is rejected with a clean `Result` error — never a corrupted node.

### Why this is the shape of the deliverable
B+Tree bugs are the canonical *silently-wrong* class: a mis-routed separator loses a key range but every individual lookup you hand-test still passes. Prose review can't catch that; only volume (10M randomized ops against an oracle that cannot share your bugs) and structure (a walker asserting the invariants directly from bytes) can. Hence a differential fuzz *and* an independent walker, not either alone — the fuzz proves behavior, the walker proves shape, and a bug that fools one rarely fools both.

## Exam questions this phase targets (build-proven)
1. Implement leaf splits (copy-up) and internal splits (push-up) such that the walker's separator-consistency row PASSes across 10M ops — and explain the asymmetry from memory.
2. Implement delete with redistribution-before-merge, including root collapse, such that the fuzz's delete-heavy seeds stay green.
3. State your iterator-validity contract from memory and defend it against: "what happens if a split lands between two `Next()` calls?"
4. Given variable-length keys, show how your split point is chosen and what happens when one key is larger than half the byte budget.

## Prerequisites — concepts this phase uses

These are vocabulary — recognize the role each plays. Implementing them is the build.

### Tree structure
- **B+Tree vs B-Tree** — in a B+Tree all values live in leaves; internal nodes hold only *separator keys* for routing. B-Trees store values at every level. Databases use B+ almost exclusively because leaves form a scannable chain and internal nodes pack more fan-out. Canonical orientation: Comer, "The Ubiquitous B-Tree" (ACM Computing Surveys, 1979); origins in Bayer & McCreight (1972). For the modern practitioner's view: Graefe, "Modern B-Tree Techniques" (Foundations and Trends in Databases, 2011) — chapters 2–3 only for this phase.
- **fan-out** — the number of children an internal node has. With variable-length keys this is a consequence of the byte budget, not a constant. Higher fan-out → shallower tree → fewer pins per probe.
- **separator key** — a key in an internal node dividing the key space between two children. It need not be a key that exists in any leaf — it only needs to route correctly. (This fact matters in delete.)
- **leaf sibling chain** — leaves linked left-to-right by page id, so a range scan walks leaves without re-descending. The substrate of your iterator and of 4.3's phantom tracking.
- **copy-up vs push-up** — the two split disciplines. A leaf split *copies* the boundary key into the parent (it must remain in the leaf — values live there); an internal split *pushes* the middle key up (it must leave the node — it's pure routing). Knowing *that* there are two disciplines is vocabulary; getting them right is the build.
- **redistribution vs merge** — the two underflow remedies on delete: borrow an entry from a sibling (and fix the parent separator), or coalesce two siblings into one (and remove a parent entry, possibly recursing).

### Storage substrate (recap from Tier I)
- **pin / unpin** — Phase 1.3's contract: a page may only be read or written while pinned; pins are scarce (pool-sized). Tree depth bounds your simultaneous pins.
- **slotted node** — the Phase 1.1 slot-directory idea applied inside an index node: an indirection array enables sorted order over variable-length cells without moving cell bytes on every insert.
- **`RID`** — Phase 1.1's stable record id (page, slot). It's the *value* type this tree maps keys to.

### Contracts you'll author
- **duplicate-key policy** — what `Insert` does when the key already exists: reject, overwrite, or store duplicates (e.g., key+RID composite). Real engines differ (a unique index rejects; a secondary index composites). You must pick one, document it, and the harness mirrors it.
- **iterator-validity contract** — the rules for what an iterator may assume between `Next()` calls while the tree mutates underneath it. The C++ standard library's "iterator invalidation" rules are the same genre of contract; yours must be written for a world where "invalidation" means "the page split."

## How you know you're aligned (the cross-check)

The harness in `test/btree/` is the continuous oracle — build a part, run it, watch rows flip SKIP→PASS. Two independent judges, neither of which can share your bugs:

1. **Differential fuzz vs `std::map`** (or `std::multimap`, matching your documented duplicate policy via a one-line harness switch). Seeded random streams of insert/delete/lookup/scan over variable-length keys — 10M ops at full strength, smaller gated rows per build part. *Every* lookup result is compared, and at intervals a full-range iteration must reproduce the oracle's exact key order and values. Multiple seeds, including delete-heavy and skewed-key mixes.
2. **The structural-invariant walker** — a standalone binary in `test/btree/` that opens the database file *read-only, raw, linking none of your tree code* and asserts: keys sorted within every node, every node within fan-out/occupancy bounds, all leaves at one depth, leaf-chain order consistent with parent separators, no unreachable pages claimed by the tree. Because your node layout is private, the walker is written against a **node-format note you freeze in Part 1** (a half-page doc: header fields, slot encoding, where the sibling pointer lives) — the `waldump` pattern from Phase 3.1, two phases early. If the note is ambiguous, the walker can't be written; that's the point.

The harness touches only the public API below plus raw file bytes. It never sees your descent logic, your split-point search, or your node structs.

## The build, in parts (each gated independently by the harness)

### Part 1 — Node format, point lookup, split-free insert 🔨
A leaf node format (slotted, variable-length keys), a root-only tree, `Lookup` and `Insert` while everything fits in one page. **Freeze the node-format note** — the walker gets written against it now. Harness rows: small-scale differential (no splits), walker on a single-leaf tree, pin-leak zero.

### Part 2 — Splits and root growth 🔨
Leaf splits (copy-up), internal splits (push-up), root split growing the tree by one level. Variable-length keys make the split point a byte-budget search, not `n/2`. Harness rows: 1M-op insert-heavy fuzz, walker after every 10k ops, depth-uniformity and separator-consistency rows.

### Part 3 — Delete: redistribution, merge, root collapse 🔨
Underflow handling: try redistribution, fall back to merge, fix parent separators, recurse upward, shrink the root when it has one child. Harness rows: delete-heavy fuzz including drain-to-empty and refill cycles, walker green throughout, occupancy-bound row.

### Part 4 — Range iterator + the two written contracts 🔨
`Seek`/`SeekFirst` and an iterator walking the leaf chain. Write `docs/btree_iterator_contract.md` (validity rules — this is the 2.2/4.3 seam) and the duplicate-policy note. Harness rows: full-scan order equality vs oracle, bounded-scan rows, interleaved mutate-and-scan rows that exercise exactly what your contract permits.

### Part 5 — The harness as a published artifact 🔨
All rows green at full strength: 10M ops, all seeds, ASan/UBSan builds. Write RESULTS.md from `templates/RESULTS.md`: the table, ops/sec and pins-per-probe observed, and a paragraph on the worst structural bug the walker caught.

## API contract (what the harness imports)

Spellings are adjustable via the harness shim; semantics are not. `Slice` is a non-owning byte view (`std::span<const std::byte>` or your equivalent from Tier I). All fallible operations return `chronos::Result<T>` — no exceptions exist in this codebase.

### `BPlusTree` — lifecycle

```cpp
static Result<BPlusTree> Create(BufferPool& pool);            // allocates an empty tree
static Result<BPlusTree> Open(BufferPool& pool, PageId root); // attaches to an existing one
PageId RootPage() const;                                      // current root (it moves on root splits)
```

- **What it does:** binds a tree to the buffer pool; `RootPage()` exposes where the tree currently starts.
- **Why it exists:** Phase 5.1's catalog will persist root page ids; until then the harness and the walker need `RootPage()` to find the tree in the file.
- **Side-effect requirement:** after `Create`, the tree is empty and valid (the walker accepts it). Persisting the root id across process restarts is *not* this phase's problem.
- **Critical contract details:** single-threaded — externally synchronized; the thread-safety class upgrades in 2.2 without the signatures changing.

### `BPlusTree` — point operations

```cpp
Result<bool> Insert(Slice key, RID rid);
Result<bool> Remove(Slice key);
Result<std::optional<RID>> Lookup(Slice key);
```

- **What it does:** ordered-map semantics under *your documented duplicate policy*. `Insert` returns whether a new entry was added; `Remove` returns whether anything was removed; `Lookup` returns the RID or `nullopt`.
- **Why it exists:** this is the index probe every executor operator (5.3) and every constraint check ultimately issues.
- **Side-effect requirement:** on return, all pages pinned by the operation are unpinned (error paths included), all modified pages are marked dirty, and every structural invariant the walker checks holds — operations are never observably "in progress" between calls.
- **Critical contract details:** keys are arbitrary bytes, compared by your documented comparator (memcmp order is fine — document it; the harness oracle uses the same order). A key too large for your node budget must fail with a clean error, never corrupt a node. Empty keys: decide and document.

### `BPlusTree::Iterator` — range scans

```cpp
Result<Iterator> SeekFirst();
Result<Iterator> Seek(Slice lower_bound);   // positions at first key >= lower_bound
bool   Iterator::Valid() const;
Slice  Iterator::Key() const;               // valid only while Valid()
RID    Iterator::Value() const;
Result<void> Iterator::Next();
```

- **What it does:** forward iteration in key order along the leaf chain, from the seek position to the end (callers impose upper bounds themselves — 5.3 will).
- **Why it exists:** range predicates are *the* reason this is a B+Tree and not a hash table; this iterator is the surface 4.3's next-key tracking and 5.3's IndexScan are built on.
- **Side-effect requirement:** whatever pages the iterator holds pinned must be released by destruction at the latest — a dropped iterator must not leak pins (the harness checks).
- **Critical contract details:** behavior when the tree mutates between `Next()` calls is governed by **your iterator-validity contract** — the harness tests exactly what that document promises, no more. Promise little and keep it absolutely, rather than promising snapshot semantics you can't deliver single-threaded.

**PRIVATE INTERNALS — deliberately unspecified:** node header layout, slot encoding, split-point selection, occupancy thresholds, whether internal nodes also use slots, prefix truncation (optional, do it only if it tempts you). The walker reads your format *note*, not your headers; designing the layout is the work.

## Acceptance criteria (phase-level "done")
1. Harness: all rows PASS — 10M-op differential fuzz across all shipped seeds, walker green at every checkpoint, pin-leak counter zero. ASan and UBSan builds both green.
2. `docs/` contains the frozen node-format note, the iterator-validity contract, and the duplicate-key policy — each consistent with observed harness behavior.
3. RESULTS.md published with the table, ops/sec, and pins-per-probe; a stranger can rerun everything.

## Principal-engineer traps (no solutions)
- **Copy-up vs push-up.** A leaf split *copies* the separator into the parent; an internal split *pushes* it up and out. Mix the disciplines and ranges go silently wrong — lookups for the boundary keys still mostly work, which is why the walker exists.
- **Delete is where B+Trees die.** Redistribution must be attempted before merge; both must fix parent separators; merges recurse and can collapse the root. Most "complete" B+Trees you'll find online quietly never implemented delete. The drain-to-empty seed is there because emptiness is an edge case nobody tests.
- **Stale separators after redistribution.** Borrowing from a sibling moves the boundary; the parent's separator must move with it. The walker's separator-consistency row exists for exactly this.
- **Iterator validity under mutation.** A split or intra-node compaction between `Next()` calls can move the bytes your iterator points into. Define now what survives — slot index? key copy? page id + reposition? — because 2.2 inherits this contract verbatim and adds threads.
- **Variable-length split points.** "Half the entries" can be nowhere near half the bytes. And a single key larger than half the budget can make *no* valid split point exist — decide what happens before the fuzz finds it.
- **Pin discipline on the descent.** Recursive descent + splits is the easiest place in the project to leak a pin or hold pins proportional to node count instead of depth. The pool is small on purpose in the harness config.

## What you hand back for review
1. Implementation + the full harness table (fuzz seeds, walker output) + the three documents (node format, iterator contract, duplicate policy).
2. One sentence per trap above: did it bite, and how was it resolved.

Review is principal-engineer style: the interview attack will be on delete ordering and on the iterator contract's edge cases, because 2.2 is about to inherit both. Then we scaffold 2.2.

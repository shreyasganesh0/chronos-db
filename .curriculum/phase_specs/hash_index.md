# Tier II · Phase 2.3 — Extendible hash index (scaffold)

**Deliverable of this phase:** a disk-resident `ExtendibleHashIndex` — directory with global depth, bucket pages with local depths, bucket splits, directory doubling, point lookup and delete — green on a differential fuzz vs `std::unordered_map` and on an independent directory-invariant checker.
**What you'll own afterward:** the second index family in the engine and the judgment to know when it's the *wrong* one — so that when Phase 5.3's planner has to choose hash vs B+Tree by predicate shape, the trade-off is something you've built both sides of, not a rule you memorized.
**Calibration:** 🔨 (linear hashing: 📖)
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; directory layout, bucket format, and hash-bit conventions are yours.

## What this is — and is not

**This is a dynamic disk-resident hash table, not `std::unordered_map`.** Extendible hashing solves the problem ordinary hash tables can't: growing gracefully on disk. An in-memory table rehashes everything when it grows — fine for RAM, catastrophic when every entry is a page I/O. Extendible hashing grows *incrementally*: one bucket splits at a time, and the directory — an array of bucket-page pointers indexed by a prefix of the hash — doubles only when a split needs more address bits than the directory has. Both the directory and the buckets live on buffer-pool pages.

It is **not an ordered index, and that is a feature with teeth.** O(1) point lookups; *no* range scans, no ordered iteration, nothing useful for `WHERE k > 10` or `ORDER BY`. Half the lesson of this phase is negative knowledge: a hash index on a column queried by range is a planning bug, and in Phase 5.3 *your* planner must refuse to make it. If you find yourself adding an iterator to this structure, stop — you're rebuilding 2.1 badly.

It is also **single-threaded this phase** (externally synchronized), like 2.1 was. The concurrency rep was 2.2; this phase spends its budget on the directory/bucket dance.

You're done with the framing when you can say: the deliverable is *an incrementally growable on-disk hash structure for exact-match probes*, not *an unordered map and not an ordered index*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
Reasoning about a two-level addressing scheme under growth: which directory slots alias to the same bucket, what splitting a bucket does to that aliasing, and when the address space itself must double. It's the same engineering muscle as page tables and radix structures — a prefix-indexed directory with sharing — and once built, you can debug any "pointer-array-with-aliasing" structure from its invariants.

### Why "from scratch" is the right call here
- **The aliasing invariant is invisible from outside.** `2^(global − local)` directory slots point at each bucket; a library user never sees it, so a corrupted directory looks like "occasional wrong lookups." Build it and you know exactly which pointers a split must rewrite and why.
- **The termination argument is yours to own.** Splitting a bucket whose keys all share the next hash bit produces an empty sibling and a still-full bucket — the naive loop never terminates. Knowing *why* it can recurse and what bounds it is the difference between an engine and a demo.
- **5.3's planner choice needs a builder's cost model.** "Hash for equality, B+Tree for ranges" is a slogan; you'll know the actual constants — directory probe + one bucket page vs O(depth) tree pins.
- **📖 Linear hashing stays honest.** You can only meaningfully compare against Litwin's alternative (no directory, split pointer marching in order) after building the directory version.

### What carries forward to later tiers
- **Phase 5.1:** the catalog's `chronos_indexes` table records index *kind*; hash indexes become a second concrete kind with their own root (directory) page.
- **Phase 5.3:** the binder/planner picks hash vs B+Tree by predicate shape — equality probes route here, range predicates must not. The required-refusal is a harness row *in that phase*, seeded by what you learn in this one.
- **The directory-doubling pattern** — grow an address array by replication, then lazily de-alias — recurs in any extendible structure: radix trees, page-table-style maps, resizable partition maps. You'll recognize the shape on sight.
- **The format-note + independent-checker discipline** from 2.1 gets its second rep here; by 3.1 (`waldump`) it should feel like the obvious way to ship a structure.

### What good looks like
- Lookup touches exactly two pages in the common case: one directory page, one bucket page. If it's more, you've over-built; if directory access ever scans, you've under-built.
- You can answer cold: *"Global depth 3, a bucket with local depth 1 — how many directory slots point at it, and which ones?"* — and the inverse question.
- Splits rewrite only the directory slots they must; doubling copies the directory once, with no per-key rehashing of untouched buckets.
- The recursive-split case (all keys share the next bit) terminates, with a documented bound and a clean failure mode for the pathological worst case (e.g., more identical-hash keys than a bucket holds).
- Your delete-time merge/shrink decision is written down with a reason — "supported, because X" or "not supported, because Y" — not silently absent.

### Why this is the shape of the deliverable
Hash-index bugs are aliasing bugs: a directory slot pointing at the wrong bucket only misbehaves for keys whose hash lands in that slot — a tiny, hash-dependent slice of the key space that hand testing essentially never hits. So the artifact is a seeded differential fuzz (volume across the hash space) *plus* a checker that asserts the aliasing invariants directly from the raw pages (structure, independent of any lookup). A bug that produces right answers from a wrong directory fails the checker; one that keeps the invariants but routes wrongly fails the fuzz.

## Exam questions this phase targets (build-proven)
1. Implement bucket split + directory doubling such that the checker's `2^(global−local)` row stays green across a fuzz that forces multiple doublings.
2. Implement the recursive split (all keys share the next bit) such that the adversarial-keys seed terminates — and state the termination bound from memory.
3. Demonstrate, with the checker, exactly which directory slots a split rewrote — and defend why the others were untouched.
4. 📖 Whiteboard linear hashing: how it grows without a directory, and one workload where each scheme beats the other.

## Prerequisites — concepts this phase uses

### Extendible hashing vocabulary
- **extendible hashing** — the directory-based dynamic hashing scheme from Fagin, Nievergelt, Pippenger & Strong, "Extendible Hashing — A Fast Access Method for Dynamic Files" (ACM TODS, 1979). The original paper is short and readable; read it for the *scheme's shape*, not as a recipe — the design decisions (bit convention, layouts) are yours.
- **directory** — an array of `2^global_depth` bucket-page pointers, indexed by `global_depth` bits of the key's hash. Multiple slots may point at the same bucket; that aliasing is the whole trick.
- **global depth** — how many hash bits the directory currently uses. Doubling the directory increments it.
- **local depth** — per bucket: how many hash bits all keys in this bucket are known to share. Always ≤ global depth. The aliasing invariant: exactly `2^(global − local)` directory slots reference a bucket with local depth `local`.
- **bucket split** — on overflow: allocate a sibling page, increment local depth, redistribute the bucket's keys by the newly significant bit, rewrite the aliased directory slots between the two buckets.
- **directory doubling** — when a splitting bucket already has `local == global`: replicate the directory (every old slot becomes two), increment global depth, then split. Note which half of the new slots aliases where — the stride question is a named trap.
- **bit convention (LSB vs MSB)** — whether the directory indexes by the hash's low or high bits. Either works; the choice changes the doubling copy pattern and the slot arithmetic everywhere. Pick one, document it in the format note, never mix.
- **📖 linear hashing** — Litwin, "Linear Hashing: A New Tool for File and Table Addressing" (VLDB, 1980): grows one bucket at a time in a fixed order with a split pointer and *no directory*, tolerating overflow chains in exchange. Conversant-level: be able to compare, not build.

### Storage substrate (recap)
- **bucket page** — a buffer-pool page holding key/RID entries for one bucket; full = the next entry doesn't fit the byte budget (variable-length keys, same discipline as 2.1).
- **hash function** — your choice (you already hand-rolled CRC32 in 0.1; any documented, deterministic, well-mixing function is fine). It must be stable across runs — the checker recomputes it from your format note.

## How you know you're aligned (the cross-check)

The harness in `test/hash_index/` is the continuous oracle. Two independent judges:

1. **Differential fuzz vs `std::unordered_map`.** Seeded streams of insert/lookup/delete with variable-length keys, comparing every lookup result. Seeds include: uniform random keys; insert-heavy runs sized to force several directory doublings; delete-heavy runs; and an **adversarial seed** of keys constructed to collide on long hash-bit prefixes, which forces the recursive-split path and probes your termination bound. The oracle shares no code with your index — not even the hash function.
2. **The directory-invariant checker** — a standalone binary reading the raw database file against your frozen **format note** (directory layout, bucket header, bit convention), linking none of your index code. It asserts: exactly `2^(global − local)` directory slots per bucket, and they are the *correct* slots (all sharing the bucket's `local`-bit prefix under your documented convention); every key in every bucket hashes to that bucket's prefix; `local ≤ global` everywhere; no orphan buckets (allocated but unreachable) and no dangling slots.

Build a part, run the suite, watch rows flip SKIP→PASS. The harness sees only the public API plus raw bytes; your layouts stay private behind the format note.

## The build, in parts (each gated independently by the harness)

### Part 1 — Directory, buckets, split-free operation 🔨
Directory page(s) + bucket pages; insert/lookup/delete while no bucket overflows; **freeze the format note** (the checker is written against it now, including your bit convention). Harness rows: small-scale differential, checker on a fresh index, two-page-probe row.

### Part 2 — Bucket splits and directory doubling 🔨
Overflow → split; `local == global` → double, then split; the recursive case when redistribution leaves one side still overfull. Harness rows: doubling-forced fuzz, the adversarial collision seed, checker after every operation batch, slot-rewrite minimality row (doubling must not touch bucket pages).

### Part 3 — Delete + the merge/shrink decision 📖→🔨
Delete with the differential fuzz green through drain/refill cycles. **Decide explicitly** whether empty/underfull buckets merge and the directory ever shrinks — and write the decision note with the reason (real engines mostly don't shrink; "no, because…" is a fully acceptable answer, silence is not). Whatever you decide, the checker must still pass — a supported merge must restore the aliasing invariant exactly. Also: the half-page 📖 linear-hashing comparison note.
Harness rows: delete fuzz seeds; if you chose merging, merge-specific checker runs.

### Part 4 — The harness as a published artifact 🔨
All rows green at full strength under ASan/UBSan. RESULTS.md: the table, pages-touched-per-probe, doubling count under the growth seed, and a paragraph on the recursive-split case.

## API contract (what the harness imports)

Spellings adjustable via the harness shim; semantics are not. `Slice`, `RID`, `Result<T>`, `BufferPool`, `PageId` as established in Tiers 0–II.

### `ExtendibleHashIndex` — lifecycle

```cpp
static Result<ExtendibleHashIndex> Create(BufferPool& pool);
static Result<ExtendibleHashIndex> Open(BufferPool& pool, PageId directory_root);
PageId   DirectoryRoot() const;
uint32_t GlobalDepth() const;
```

- **What it does:** binds the index to the pool; exposes the directory's root page (the checker's entry point) and the current global depth.
- **Why it exists:** Phase 5.1 will persist `directory_root` in the catalog; `GlobalDepth()` lets the harness assert growth actually happened under the growth seed.
- **Side-effect requirement:** after `Create`, the index is empty and checker-valid (a defined initial global depth — your choice, documented).
- **Critical contract details:** single-threaded / externally synchronized. Directory larger than one page: support it or reject growth past one page with a clean error — decide, document, and the harness growth seed respects your documented cap.

### `ExtendibleHashIndex` — operations

```cpp
Result<bool> Insert(Slice key, RID rid);
Result<bool> Remove(Slice key);
Result<std::optional<RID>> Lookup(Slice key);
```

- **What it does:** exact-match map semantics. Unique keys this phase (duplicate insert returns `false`, no overwrite) — the secondary-index composite-key trick from 2.1 is how real duplicates would arrive, and that's a 5.1 concern.
- **Why it exists:** the equality-probe path 5.3's planner will route `WHERE k = ?` through.
- **Side-effect requirement:** on return, all pins released (error paths included), dirty pages marked, and every checker invariant holds — splits and doublings complete or don't happen; the structure is never observably mid-split between calls.
- **Critical contract details:** no ordered iteration exists on this type, deliberately — the absence is part of the contract. A key too large for a bucket's byte budget fails cleanly. More identical-hash keys than one bucket can hold: define the failure (clean error after a bounded split cascade), don't loop forever — the adversarial seed checks this.

**PRIVATE INTERNALS — deliberately unspecified:** directory page layout, bucket header format, LSB-vs-MSB convention, hash function, initial depth, split/doubling mechanics. All frozen only in your format note, for the checker's eyes.

## Acceptance criteria (phase-level "done")
1. Harness: all rows PASS — full differential fuzz (all seeds including adversarial collisions), checker green at every checkpoint, ASan/UBSan builds green.
2. Format note, merge/shrink decision note, and 📖 linear-hashing comparison note published.
3. RESULTS.md published with the table and the growth/probe numbers; a stranger can rerun.

## Principal-engineer traps (no solutions)
- **The non-terminating split.** Split a bucket where every key shares the next hash bit: one side empty, the other still overfull, so it splits again — possibly doubling again — and with enough identical-hash keys, forever. Know your termination bound and your bail-out before the adversarial seed teaches it to you.
- **Doubling stride/aliasing reversal.** When the directory doubles, each old slot maps to two new ones — and which of the two inherits the old pointer vs aliases the future sibling depends entirely on your bit convention. Getting it mirrored produces an index that works until the *next* split, which is the cruelest possible failure timing.
- **Rewriting the wrong slot set on split.** The slots to repoint are exactly the aliased group sharing the bucket's old prefix — half keep the old bucket, half get the sibling. Off-by-one in the group arithmetic corrupts a hash-dependent sliver of the key space that point tests won't find; the checker will.
- **Local depth drift.** Forgetting to increment local depth on split (or incrementing the wrong bucket's) keeps lookups working for a while — the aliasing invariant is the only thing that notices immediately.
- **Punting the merge/shrink decision.** Not supporting deletion-time merging is defensible engineering; not *deciding* is spec rot. The decision note is a deliverable, and "what would shrink cost?" is review whiteboard material.
- **Range-scan temptation.** The moment a test or future phase makes you want ordered iteration here, the answer is "wrong index" — that instinct is the planner lesson of 5.3, available early.

## What you hand back for review
1. Implementation + full harness table + checker output + the three notes (format, merge/shrink decision, linear-hashing comparison).
2. One sentence per trap: did it bite, how resolved.

Review attack: the doubling arithmetic and the termination bound, at the whiteboard. Then 2.3 closes and the path to Tier III is open (2.4 floats — see the build plan's ordering note).

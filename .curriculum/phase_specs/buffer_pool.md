# Tier I · Phase 1.3 — Buffer pool (scaffold)

**Deliverable of this phase:** a thread-safe, write-back `BufferPool` over the Phase 1.2 disk manager — frame table + open-addressed page table, non-copyable RAII pin guards, dirty tracking, clock then LRU-K eviction with scan resistance, miss-in-flight coordination, a prefetch hook — TSAN-clean, with the harness in `test/buffer_pool/` green: trace-replay differential vs a standalone cache simulator, pin-leak quiesce checks, and a hit-rate table (clock vs LRU-K vs sequential flood).
**What you'll own afterward:** the memory hierarchy of the engine. When throughput collapses under a mixed workload two tiers from now, you'll reason in pins, dirty frames, and eviction pressure — not "the cache is slow."
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is a write-back, pin-disciplined, WAL-coupled page cache — not a generic LRU cache.** You built a cache at quackdb L09-10; that contrast is the lesson of this phase. quackdb's pool was a single-threaded read cache over immutable columnar blocks: nothing was ever dirty, nothing was ever pinned against eviction mid-mutation, no two threads ever raced to fault the same block, and eviction never had to ask anyone's permission. chronos's pool inverts every one of those properties. Pages are mutated in place, so the pool owns *dirty* state and writes back through the io engine. Callers hold references into frames while mutating them, so eviction must be *pin-gated* or you hand out dangling memory. Multiple threads miss on the same page simultaneously, so the pool needs *miss-in-flight* frame states or one thread's read clobbers another's. And in Tier III, eviction of a dirty page becomes legal only when the WAL says so — a check this phase must leave a shaped seam for.

If you catch yourself building a key→value cache with get/put and an eviction list, stop — that's quackdb's artifact. The right artifact is: *a pin-disciplined frame manager whose eviction is a privilege the rest of the engine grants, not a right the cache exercises.*

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
You'll be the kind of engineer who can debug a database's memory hierarchy under concurrency: who reads "pin count 3, dirty, recLSN unset" off a frame and knows what's allowed to happen to it, who recognizes sequential flooding from a hit-rate graph, and who knows why "just evict the coldest page" is wrong in three separate ways (it's pinned; it's dirty and unflushed; its WAL records aren't durable yet). Buffer management is where storage, concurrency, and recovery meet — owning this junction is most of owning an OLTP engine.

### Why "from scratch" is the right call here
Treat the pool as a black box and you cannot reason about:

- Phase 3.1's flushedLSN protocol — the rule "never evict a dirty page whose pageLSN exceeds flushedLSN" *is* the WAL contract, and it's enforced *here*, in the eviction path you're writing now. Don't own the path, can't own the rule.
- Phase 3.2's dirty page table — checkpoints serialize exactly the bookkeeping this phase shapes (dirty set + per-frame recLSN slot).
- Phase 2.2's latch crabbing — its safety argument is "a pinned page cannot move or vanish"; that guarantee is manufactured here and nowhere else.
- The classic production fire: a pin leak slowly starving the pool until every fetch blocks. You'll have built the detector before you've built the bug's habitat.
- Phase 6.3's pool-size ablation — interpreting those curves requires knowing what the replacement policy actually does under your workload mix, not what its name suggests.

### What carries forward to later tiers
- **Phase 2.1/2.2:** every B+Tree node access is `fetch_page` + pin guard; the guard's lifetime discipline becomes the latching discipline.
- **Phase 3.1:** stamps `pageLSN` on frames it mutates and wires `flushedLSN` into the eviction gate seam you leave now — the load-bearing seam of this phase.
- **Phase 3.2:** the dirty-page bookkeeping (with its recLSN slot) is read out as the DPT in begin-checkpoint records.
- **Phase 3.3:** recovery's redo faults pages through this pool; repeat-history performance is your miss path's performance.
- **Phase 6.3:** the hit-rate table you build here becomes the methodology for the pool-size ablation.
- quackdb L09-10 overlap, restated: that was the read-cache half of the idea; this phase is the half OLAP never needed.

### What good looks like
- The public surface is `fetch`/`new`/`flush` + a guard type. The guard is non-copyable, movable, and unpins exactly once — no manual `unpin(page_id)` calls anywhere in the API for callers to forget.
- At any quiesce point: all pins zero, dirty set exactly equal to the simulator's, no frame in an in-flight state. These hold by construction and the harness asserts them.
- You can answer cold: *"Two threads fetch the same non-resident page simultaneously — walk me through both threads' paths"* and *"Why can't you evict a dirty page in Tier III without consulting the WAL, and where in your code will that check live?"*
- A sequential flood of N× pool-size pages leaves the hot set's hit rate intact under your final policy — and you can show the same flood demolishing plain clock, because you measured the failure before fixing it.
- TSAN-clean across the full multithreaded replay, not just the happy path.

### Why this is the shape of the deliverable
Replacement-policy bugs are statistical (a subtly wrong clock hand still mostly works) and concurrency bugs are probabilistic (the double-fetch race fires one run in a thousand) — neither yields to example-based tests. So the harness is a **trace-driven differential**: a recorded access trace replayed through your pool and through a tiny standalone simulator with zero shared code, demanding the identical hit/miss/evict *sequence* for deterministic policies — which converts statistical wrongness into a first-divergence failure — plus TSAN-supervised multithreaded replay with quiesce invariants, which converts the probabilistic races into assertion failures. The hit-rate table exists because scan resistance is a *measured* property, not a code-review property.

## Exam questions this phase targets (build-proven)
1. Implement a pin-disciplined buffer pool such that the trace-replay differential matches the standalone clock simulator's hit/miss/evict sequence exactly, and LRU-K's hit rate lands within the harness tolerance of the simulator's.
2. Implement miss-in-flight coordination such that the harness's concurrent same-page-miss row passes under TSAN: one disk read, both threads get the page, no torn frame.
3. Reproduce sequential flooding with plain clock (the harness *requires* the bad number), then ship the scan-resistant configuration and report both hit rates side by side.
4. Show where Tier III's flushedLSN-gated eviction will attach, and defend why no other placement is correct.

## Prerequisites — concepts this phase uses

Vocabulary, not the build. Definitions say what role each plays — implementing them is the phase.

### pool anatomy
- **frame** — a pool-owned, page-sized, O_DIRECT-aligned memory slot (allocated via the 0.2 slab allocator). Frames are the fixed real estate; pages move in and out of them.
- **page table** — the map from resident `PageId` → frame index. Spec'd here as an open-addressed hash table (you built the discipline's pieces in Tier 0; the table is yours to write — no `std::unordered_map`, which heap-allocates per node and is the wrong tool under a latch).
- **pin count** — per-frame count of outstanding references. A pinned frame must not be evicted or have its contents replaced; pinning is how callers make "this pointer stays valid" true. See any engine's docs — e.g. PostgreSQL's buffer manager README: https://github.com/postgres/postgres/blob/master/src/backend/storage/buffer/README
- **dirty flag / write-back** — a frame whose bytes have diverged from disk is dirty; *write-back* means the pool defers the disk write until eviction or an explicit flush, rather than writing through on every mutation.
- **RAII pin guard** — an object whose constructor acquires the pin and whose destructor releases it; non-copyable so a pin can't be silently duplicated or double-released. The C++ idiom that makes pin leaks a type-system problem instead of a code-review problem.

### replacement policy
- **clock (second-chance)** — the classic approximation of LRU: frames in a ring, a reference bit per frame, a sweeping hand that clears bits and evicts the first unset frame. Cheap, concurrent-friendly, and famously vulnerable to scans.
- **LRU-K** — eviction by the K-th most recent reference time rather than the most recent, so one-touch pages (a scan) can't masquerade as hot. The original paper is the authoritative source: O'Neil, O'Neil & Weikum, "The LRU-K Page Replacement Algorithm for Database Disk Buffering," SIGMOD 1993 — https://www.cs.cmu.edu/~natassa/courses/15-721/papers/p297-o_neil.pdf
- **scan resistance / sequential flooding** — a full-table scan touching N× pool-size pages exactly once evicts the entire hot set under recency-only policies. The canonical buffer-pool pathology; this phase requires you to reproduce it before fixing it.

### concurrency
- **miss-in-flight state** — a frame state meaning "this page is being read from disk right now." Latecomers for the same page must wait on that state, not issue a second read — the alternative is the **thundering herd / double-fetch** race where two reads land in different frames (or worse, the same one).
- **quiesce invariant** — a property checked when all worker threads have finished: pins all zero, no in-flight frames, dirty set exactly as the model predicts. Violations indicate leaks or lost state that the busy period masked.
- **TSAN (ThreadSanitizer)** — the compiler-instrumented data-race detector (`-fsanitize=thread`); "TSAN-clean" means the full multithreaded replay reports zero races. https://clang.llvm.org/docs/ThreadSanitizer.html

### the Tier III seam (vocabulary only — nothing implements it this phase)
- **recLSN** — per-dirty-frame: the LSN of the *first* record that dirtied it since it was last clean. Tier III's checkpoint (3.2) and recovery (3.3) consume it; this phase only shapes a slot for it.
- **flushedLSN-gated eviction** — Tier III's rule that a dirty page may be written back only when the WAL is durable up to that page's `pageLSN`. This phase doesn't enforce it; this phase makes it *attachable* at one point.

## How you know you're aligned (the cross-check)

The harness in `test/buffer_pool/` is the continuous oracle — build a part, run it, watch rows flip SKIP→PASS. The independent oracle is a **standalone cache simulator**: a tiny model of clock and LRU-K living in its own translation unit under `test/buffer_pool/`, sharing zero code and zero headers with `src/` (the harness verifies the include rule). The harness replays recorded access traces through both:

- For **clock**, the sequences of hits, misses, and evicted page ids must match the simulator *exactly*, event for event — a deterministic policy has one correct behavior, and first divergence pinpoints the bug.
- For **LRU-K**, where tie-breaking legitimately varies, the contract is hit-rate within the harness's stated tolerance of the simulator across the trace corpus.
- For **concurrency**, multithreaded replays run under TSAN with quiesce assertions: all pins zero, dirty set identical to the simulator's, exactly one disk read per distinct miss (the pool's stats counters are how the harness sees this).

The harness touches only the public surface in the API contract. Frame-table layout, hash probing scheme, clock-hand mechanics, LRU-K history structure, latching granularity — all private, all yours.

## The build, in parts (each gated independently by the harness)

### Part 1 — Frames, page table, pin guards, write-back 🔨
Single-threaded core: fetch (hit and miss paths), new-page, non-copyable RAII guards, dirty marking, clock eviction, write-back through the io engine, flush. **Harness rows:** single-threaded clock trace differential (exact event match), pin-blocks-eviction row (a fetch that *would* evict the only unpinned frame while everything else is pinned), quiesce pin/dirty assertions, write-back row (evicted dirty page's bytes visible via DiskManager afterward; clean evictions issue no write).

### Part 2 — Concurrency and miss-in-flight 🔨
Make it thread-safe: the page table under latches, frame state machine including I/O-in-flight, coordinated miss handling. **Harness rows:** N-thread replay TSAN-clean, the same-page concurrent-miss row (stats must show one read), eviction-vs-fetch race rows, quiesce invariants after every replay.

### Part 3 — LRU-K, the scan-flood demonstration, prefetch 🔨
First, the required failure: run the harness's sequential-flood trace against plain clock and record the hot-set hit-rate collapse — **a harness row requires the bad behavior be reproduced and its number captured**. Then LRU-K and a scan-resistant configuration; the same trace must now leave the hot set intact. Wire the `prefetch` hook (fire-and-forget read into a frame; correctness rows ensure it never evicts pinned/protected pages and coordinates with miss-in-flight). **Harness rows:** LRU-K hit-rate-within-tolerance across the trace corpus, the before/after flood table, prefetch-correctness rows.

### Part 4 — The Tier III seam, shaped and documented 🔨
Per-dirty-frame bookkeeping gains its recLSN slot (set to a sentinel; nothing writes real LSNs yet), the pool exposes the dirty-frame snapshot accessor, and *every* eviction-of-a-dirty-frame decision routes through the single registered eviction-gate hook (default: permit all). **Harness rows:** the gate row — harness registers a deny-gate and verifies a dirty page is flushed-or-skipped rather than evicted past it; the dirty-snapshot row — snapshot contents match the simulator's dirty set.

### Part 5 — The harness as a published artifact 🔨
Every row green under TSAN and ASan/UBSan builds. Write `RESULTS.md` from `templates/RESULTS.md`: the table, the clock-vs-LRU-K-vs-flood hit-rate table, and a paragraph on the nastiest race Part 2 surfaced. Publish.

## API contract (what the harness links against)

Code under `src/storage/`. `Result<T>` error returns; no exceptions, no RTTI. Thread-safety class for the whole public surface: fully thread-safe, callable from N threads concurrently.

**Public surface vs. private internals.** Below is everything the harness touches. The page-table hash design, frame state encoding, latch placement and granularity, clock/LRU-K internal structures, and write-back batching are **PRIVATE INTERNALS — deliberately unspecified**. The seam requirements in Part 4 constrain *where decisions route*, not how anything is structured.

### `chronos::storage::BufferPool`

```cpp
struct BufferPoolStats {   // monotonic counters; the harness's window into events
  uint64_t hits, misses, evictions, disk_reads, disk_writes;
};
enum class EvictionPolicy { kClock, kLruK };

class BufferPool {
  static Result<BufferPool> create(size_t frame_count, DiskManager& dm,
                                   io::Engine& io, EvictionPolicy policy);
  Result<PageGuard> fetch_page(PageId id);    // pins; blocks on miss I/O and full-pool pressure
  Result<PageGuard> new_page(PageId& id_out); // allocates via DiskManager, pins, zero-filled
  Result<void> flush_page(PageId id);         // write back if resident+dirty; clean ⇒ no-op
  Result<void> flush_all();
  void prefetch(PageId id);                   // advisory, fire-and-forget, never blocks caller

  BufferPoolStats stats() const;
  void set_trace_sink(void (*fn)(const TraceEvent&, void* ctx), void* ctx);
      // TraceEvent: {HIT|MISS|EVICT, PageId, frame, dirty} — the differential harness's tap

  // ---- Tier III seam (shaped now, consumed in 3.1/3.2) ----
  using EvictionGate = bool (*)(PageId id, Lsn page_lsn, void* ctx); // false ⇒ may not write back yet
  void set_eviction_gate(EvictionGate gate, void* ctx);              // default nullptr ⇒ permit
  Result<size_t> snapshot_dirty(std::span<DirtyEntry> out);          // DirtyEntry{PageId, Lsn rec_lsn}
};
```

- **What it does:** keeps up to `frame_count` pages resident; `fetch_page` returns a pinned guard over the current bytes (one disk read per residency, however many threads ask); eviction selects per `policy` among unpinned frames, writing back dirty victims through the io engine after consulting the gate.
- **Why it exists:** the engine's working set far exceeds RAM is the founding assumption of disk-based OLTP; everything above Tier I (B+Tree, recovery, executors) accesses pages exclusively through this surface.
- **Side-effect / durability requirement:** a dirty page's bytes reach disk (via DiskManager, hence CRC-stamped) before its frame is reused — *never after*, *never not at all*. `flush_page`/`flush_all` returning success means those pages' bytes are handed to DiskManager (durability beyond that is the caller's `sync` per the 1.2 contract). After `flush_all` at quiesce, the dirty set is empty.
- **Critical contract details:** `fetch_page` on a non-existent page propagates DiskManager's error. When every frame is pinned, `fetch_page` blocking vs returning a distinct pool-exhausted error is your documented choice — the pin-pressure row checks consistency with your docs. `stats()` and the trace sink are exact, not sampled (the differential depends on it; the sink may be called under internal latches — the harness's sink is non-reentrant and cheap, yours must tolerate that and no more). The eviction gate is consulted on **every** dirty write-back-for-eviction decision; a denied frame must be skipped (or flushed via a permitted path) — never evicted dirty past a denial. This routing is the load-bearing seam: Phase 3.1 installs `flushedLSN ≥ pageLSN` here without the pool changing shape.

### `chronos::storage::PageGuard`

```cpp
class PageGuard {            // non-copyable, movable; destructor unpins exactly once
  std::span<const std::byte, kPageSize> data() const;
  std::span<std::byte, kPageSize>       data_mut();   // marks the frame dirty
  PageId page_id() const;
  void   mark_dirty();       // explicit form; data_mut() implies it
  // no copy ctor / copy assign — a pin is not duplicable by accident
};
```

- **What it does:** scoped ownership of one pin. While alive, the frame's bytes stay valid and the page cannot be evicted; destruction releases the pin.
- **Why it exists:** pin leaks are the memory leaks of databases — this type makes "forgot to unpin" unwritable rather than uncommon. It is also the object Phase 2.2's latching discipline will be built around.
- **Side-effect / durability requirement:** any mutation through `data_mut()` is observed by the dirty tracking before the guard drops — a mutated-then-evicted page must hit disk.
- **Critical contract details:** moved-from guards are inert (destruction is a no-op). Guard lifetime must not outlive the pool — a documented precondition, not a runtime check. Holding a guard across a blocking `fetch_page` is legal and must not deadlock the pool at sane sizes (the pin-pressure rows probe this).

## Acceptance criteria (phase-level "done")
1. `test/buffer_pool/` — all rows PASS: clock exact-sequence differential, LRU-K hit-rate within tolerance, miss-in-flight single-read row, pin/dirty quiesce invariants, eviction-gate and dirty-snapshot rows.
2. TSAN-clean across the full multithreaded replay corpus; ASan/UBSan builds green on everything else.
3. The hit-rate table published: clock vs LRU-K vs sequential flood — including the *required* plain-clock flood failure number alongside the fixed number.
4. `RESULTS.md` published; a stranger can rerun everything, simulator included.

## Principal-engineer traps (no solutions)
- **Pin leaks are the memory leaks of databases.** One code path that drops a guard's discipline (an early error return, a moved-from guard double-counted) starves the pool hours later. The non-copyable guard plus the quiesce assertion is your detector — make sure every error path in your own pool internals respects it too.
- **The page-miss thundering herd.** Two threads faulting the same page must coordinate through an I/O-in-flight state, or one read clobbers the other — possibly a *torn frame* if both reads target the same memory. The single-disk-read stat row exists because this race otherwise passes nine runs in ten.
- **Evicting a dirty page will later require `flushedLSN ≥ pageLSN`.** Leave the seam now — one decision point, gated — or rebuild the pool's eviction path in Tier III with a B+Tree already on top of it. The gate row is in the harness specifically so the seam exists *and works* before anything depends on it.
- **A full-table scan must not flush the hot set** — but earn the fix: the harness requires you to reproduce the failure with plain clock first and keep the number. A fix you never saw fail is a fix you can't defend at a whiteboard.
- **The trace sink and stats are part of correctness here, not observability garnish.** If your counters drift from reality under concurrency (lost increments, double-counted hits on the in-flight path), the differential fails in ways that look like policy bugs. Decide their synchronization deliberately.
- **Latch ordering between page table and frames** is where TSAN earns its keep — the fetch-vs-evict and evict-vs-flush interleavings are the classic deadlock/race nests. Write the ordering down before writing the code.

## What you hand back for review
1. Implementation + harness table + the hit-rate table (clock / LRU-K / flood before-and-after) + TSAN/ASan/UBSan run evidence.
2. One sentence per trap: did it bite, how resolved.

Review is principal-engineer style: the interview attack ("walk both threads through a concurrent miss"; "where exactly does Tier III's gate attach and why nowhere else?"), overstated claims, the next upgrade. Then Tier II — the B+Tree gets built on these frames.

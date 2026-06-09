# Tier 0 · Phase 0.2 — Arena & slab allocators (scaffold)

**Deliverable of this phase:** a page-aligned slab allocator for O_DIRECT-able buffer-pool frames plus bump arenas with explicit lifetime classes (frame / transaction / query), fuzz-clean against a malloc-backed shadow ledger under ASan and UBSan, with a cost-per-alloc microbenchmark table vs malloc.
**What you'll own afterward:** memory in chronos stops being "the heap" and becomes a designed resource with owners and lifetimes — so when a frame is recycled under an in-flight I/O three phases from now, you debug it as a lifetime-class violation you can name, not heap corruption you can't.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface (pointers out, alignment, contents integrity, no-heap-on-hot-path); the internals — freelist encoding, chunk growth policy, header placement — are yours.

## What this is — and is not

**These are special-purpose allocators, not a malloc replacement.** A general-purpose allocator must serve any size, any lifetime, any thread, forever — and pays for that generality on every call. You are building the opposite: two narrow allocators whose rules are dictated by the I/O stack above them. The **slab allocator** hands out fixed-size, page-aligned frames that `O_DIRECT` can write from and io_uring can register — alignment is a correctness requirement imposed by Phase 0.1's contract, not a performance nicety. The **arenas** hand out short-lived odd-sized allocations (lock-request nodes, version nodes, executor scratch) that die together at a known instant — frame eviction, transaction end, query end — so individual `free` doesn't exist; `reset` does.

This is also **not a garbage collector** and not clever: no thread caches, no size-class binning, no defragmentation. If you find yourself building a malloc, stop — the constraint set (fixed frame size; lifetimes known at design time) is precisely what makes these allocators simple and fast, and learning to *exploit* known lifetimes instead of paying for generality is the lesson.

Right-artifact test: you're done with the framing when you can say the deliverable is "two allocators whose alignment and lifetime rules are dictated by O_DIRECT and async I/O — not a general-purpose malloc."

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

Designing memory ownership explicitly: deciding, per allocation, *who* frees it, *when*, and *what is allowed to still point at it* — and encoding those decisions in types and lifetime classes rather than comments. You'll be the kind of engineer who reads a use-after-free crash dump and asks "which lifetime class did this pointer outlive?" instead of sprinkling defensive copies.

### Why "from scratch" is the right call here

Take frames from `malloc` (or `std::vector<char>`) and the following go opaque, each a concrete failure waiting downstream:

- Phase 1.3's buffer pool needs thousands of 4 KB frames, every one of them legal for `O_DIRECT`. `malloc(4096)` gives you 16-byte alignment and a hidden header; the first direct write returns `EINVAL` — or worse, works on your machine's allocator and fails on another.
- Phase 0.3 registers the frame set with io_uring *once* (fixed buffers). That requires the frames to be a stable, contiguous-per-slab, never-moving region — a property no general allocator promises.
- Phase 4.1's lock manager allocates a request node per lock acquisition on the hottest path in the system. If that's a `malloc`, your lock manager's throughput ceiling is the allocator's, and you won't know it until the 8-thread benchmark.
- Use-after-free under async I/O (frame freed while an SQE still references it) is invisible until it corrupts a page. Owning the allocator is what lets you add poisoning, canaries, and debug ledgers exactly where chronos needs them.

### What carries forward to later tiers

- **Phase 1.3 (buffer pool):** every frame in the pool comes from this slab; the pool's frame table is built over slab-owned memory.
- **Phase 0.3 (io_uring engine):** the registered-buffer set is slab frames, registered once at startup — the slab's stable-address guarantee is what makes registration legal.
- **Phase 4.1 (lock manager) / Phase 4.2 (MVCC):** lock-request nodes and version nodes live in transaction-lifetime arenas; vacuum and txn-end are arena resets, not node-by-node frees.
- **quackdb overlap (L01):** you built a bump arena for column vectors there. The new difficulty here is exactly what the overlap map says: O_DIRECT alignment and async-I/O-owned lifetimes — an arena whose memory the *kernel* may still be reading.

### What good looks like

- The slab's public surface is small: create, alloc, free, counters. If it's growing methods, the design is leaking.
- Every pointer the slab returns satisfies `ptr % frame_align == 0` with the *full* frame size usable — no allocator header inside or before the frame stealing the alignment.
- Steady-state alloc/free touches no global heap — the harness's interposed malloc counter reads zero across the hot loop.
- You can answer, without running it: "what happens if someone writes to a frame after `free`?" (you know exactly which structure it corrupts and what your debug build does about it) and "why can't an arena allocation be individually freed?" (you can defend the answer with the lifetime-class design).
- The microbenchmark shows slab alloc/free and arena alloc each costing a small handful of nanoseconds — and you can explain *why* malloc costs more, not just that it does.

### Why this is the shape of the deliverable

Allocator bugs are silent and delayed: a misaligned frame fails three phases later inside a syscall; an overlap between two live allocations corrupts whichever is written second. So the proof is differential and adversarial — a randomized fuzz whose every live allocation is mirrored into a malloc-backed shadow ledger with pattern-stamped contents, verified on free and reset, the whole run repeated under ASan/UBSan as a second referee that shares no code with the ledger. The microbenchmark is part of the ship because "no heap on hot paths" is a performance *claim*, and claims ship with numbers.

## Exam questions this phase targets (build-proven)

1. Implement `SlabAllocator` such that the `slab-alignment` and `slab-fuzz-ledger` rows PASS — every returned frame page-aligned, full frame usable, contents never overlapping a live allocation across 10M randomized ops.
2. Implement `Arena` with lifetime classes such that the `arena-fuzz-ledger` and `arena-reset` rows PASS — including the row where stamped contents of all live allocations are verified immediately before `reset`.
3. Measure cost-per-alloc: report ns/op for slab alloc+free, arena alloc, and the same sizes through malloc/free, from the harness's benchmark rows — and state the no-heap-on-hot-path result from the interposition row.

## Prerequisites — concepts this phase uses

Vocabulary for a strong C/C++ systems programmer who has never built a database. Recognize the role of each; the build is yours.

### Allocation strategies

- **bump / arena allocation** — an allocator that satisfies each request by bumping a pointer through a large chunk; individual free doesn't exist, the whole arena is `reset` at once. The fastest possible alloc; the price is that all allocations share one lifetime. Background: region-based memory management.
- **slab allocation** — carve big aligned chunks ("slabs") into equal fixed-size objects; free objects are tracked and recycled. Invented for kernel object caches: [Bonwick, "The Slab Allocator" (USENIX '94)](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps). Here the "object" is a 4 KB I/O frame.
- **intrusive freelist** — a freelist whose link nodes live *inside the freed memory itself* (the first bytes of a free frame hold the next-pointer), so tracking free objects costs zero extra allocation. The standard trick; its hazard (a use-after-free overwrites your links) is yours to confront.
- **lifetime class** — a named, design-time answer to "when does this memory die": with the frame, with the transaction, with the query. The phase's organizing idea — lifetimes are declared, not discovered.

### Alignment

- **page alignment** — an address divisible by 4096. Required by Phase 0.1's `O_DIRECT` contract for every I/O buffer; the slab must deliver it on every frame, not just the first.
- **`aligned_alloc` / `posix_memalign` / `mmap`** — the system-level ways to obtain aligned backing memory for your slabs ([aligned_alloc(3)](https://man7.org/linux/man-pages/man3/aligned_alloc.3.html), [mmap(2)](https://man7.org/linux/man-pages/man2/mmap.2.html)). Which you use for chunk acquisition is your call — chunk acquisition is allowed to hit the system; per-frame alloc/free is not.
- **`alignof` / over-alignment** — C++'s alignment vocabulary; arena `alloc(size, align)` must honor caller alignment up to at least `alignof(std::max_align_t)`, and the frame slab far beyond it.

### Verification machinery

- **shadow ledger** — the harness's independent model: a malloc-backed map of every live allocation (address, size, stamp seed). Differential testing — your allocator and the ledger must agree forever about what's live and what it contains.
- **pattern stamping** — filling each allocation with a seeded byte pattern at alloc and verifying it at free/reset. Catches overlap and premature recycling that address bookkeeping alone misses.
- **ASan / UBSan** — compiler sanitizers catching memory errors and undefined behavior ([AddressSanitizer docs](https://clang.llvm.org/docs/AddressSanitizer.html)). The second, code-independent referee. Caveat that matters here: ASan sees your slab chunk as *one* allocation — overflows between adjacent frames inside it are invisible to ASan. The ledger's stamps are what catch those; understand which referee covers which bug class.
- **malloc interposition** — intercepting malloc/free (LD_PRELOAD or link-order) to count calls. How the harness proves "no global heap on hot paths" instead of trusting it.

### Benchmarking

- **ns/op microbenchmark** — tight-loop cost measurement: warmup, many iterations, monotonic clock, and a compiler barrier (the role `benchmark::DoNotOptimize` plays elsewhere) so the loop isn't optimized away. The harness provides the driver; honest numbers are the contract.

## How you know you're aligned (the cross-check)

The harness in `test/allocators/` is your **continuous alignment oracle** — never build all of it and check at the end. Its independence is two-layered and deliberately non-circular: the **shadow ledger** tracks every live allocation in plain malloc'd memory with seeded pattern stamps, sharing zero code or assumptions with your allocator — if your allocator ever hands out overlapping memory, recycles a live frame, or misaligns, the ledger's verify-on-free catches it without knowing *how* you allocate. The **sanitizer builds** re-run the identical fuzz with referees written by compiler engineers, not by you or the harness author.

Build a part, run the suite, watch rows flip SKIP → PASS. The harness sees only the public surface — pointers, alignment, contents, counters, malloc-interposition counts. Your freelist encoding, chunk growth, and debug poisoning are invisible to it and yours to design.

## The build, in parts (each gated independently by the harness)

### Part 1 — The slab allocator 🔨
Fixed-size page-aligned frames out of large aligned chunks; an intrusive freelist; zero heap traffic per alloc/free at steady state. The central idea: the *full* frame is the caller's — any metadata you need lives outside it.
**Harness rows:** `slab-alignment` (every pointer across a growth-forcing run is frame-aligned with full size usable), `slab-fuzz-ledger` (10M randomized alloc/free vs the ledger, stamps verified), `slab-no-heap` (interposed malloc count is zero across the steady-state loop), `slab-exhaustion` (alloc at capacity fails cleanly via `Result`, then recovers after frees).

### Part 2 — Arenas with lifetime classes 🔨
Bump arenas tagged frame / txn / query; `alloc(size, align)` and `reset`. Allocations have stable addresses until reset; reset reclaims everything at once.
**Harness rows:** `arena-alignment` (requested alignments honored, including over-aligned), `arena-fuzz-ledger` (randomized alloc/reset cycles vs the ledger; all live stamps verified immediately before each reset), `arena-stable` (addresses unchanged by later allocs until reset), `arena-no-heap` (chunk reuse after reset — steady-state cycles stop acquiring new chunks).

### Part 3 — The combined adversarial soak 🔨
The full fuzz — interleaved slab and arena traffic from one seeded schedule, long-running — under all three builds: plain, ASan, UBSan.
**Harness rows:** `soak-plain`, `soak-asan`, `soak-ubsan` (same seed set, all green; any sanitizer report is a FAIL).

### Part 4 — The microbenchmark + the harness as a published artifact 🔨
Make every row green, run the benchmark rows (`bench-slab`, `bench-arena`, `bench-malloc-baseline`), and write `RESULTS.md` from `templates/RESULTS.md`: the table, the ns/op comparison, sanitizer status, and a paragraph on where your numbers come from. Publish.

## API contract (what the harness links against)

Code lives under `src/mem/`. Everything below is the **public surface**. **PRIVATE INTERNALS — deliberately unspecified:** freelist representation, chunk acquisition and growth policy, where slab metadata lives (only *not inside the frame*), debug poisoning, reset implementation.

### `chronos::mem::SlabAllocator`

```cpp
class SlabAllocator {
  static Result<SlabAllocator> create(size_t frame_size, size_t frame_align,
                                      size_t max_frames);
  Result<void*> alloc();          // one frame, frame_align-aligned, frame_size usable
  void free(void* frame);         // frame returns to the slab
  size_t live() const;            // currently-allocated frame count
  size_t capacity() const;        // max_frames
};
```

- **What it does:** hands out fixed-size frames, each aligned to `frame_align` (4096 for buffer-pool use) with all `frame_size` bytes caller-usable; recycles freed frames.
- **Why it exists:** the buffer pool's frame source (1.3) and the io_uring registered-buffer set (0.3). Every byte of every database page lives in memory this slab owns.
- **Side-effect / durability requirement:** a frame's address is stable from `alloc` to `free` — it never moves, which is what makes one-time io_uring registration and long-held frame pointers legal. `free(p)` is the caller's *assertion* that nothing — including in-flight I/O — still references `p`; the slab is entitled to recycle it immediately.
- **Critical contract details:** alloc at capacity returns an error via `Result`, never blocks. No malloc/free on the alloc/free path at steady state (chunk acquisition at `create`/growth is exempt and bounded). Not thread-safe — callers serialize; the buffer pool adds its own synchronization in 1.3. Double-free and foreign-pointer free are contract violations (UB), but a debug build is expected to catch the easy cases loudly — your choice how.

### `chronos::mem::Arena`

```cpp
enum class Lifetime { kFrame, kTxn, kQuery };

class Arena {
  static Result<Arena> create(Lifetime cls, size_t chunk_size);
  Result<void*> alloc(size_t size, size_t align);
  void reset();                   // frees EVERYTHING allocated since create/last reset
  size_t bytes_allocated() const;
  Lifetime lifetime() const;
};
```

- **What it does:** bump-allocates variably-sized blocks honoring `align`; `reset` invalidates every outstanding allocation at once and recycles the arena's chunks for reuse.
- **Why it exists:** the allocation regime for everything that dies in groups — lock-request nodes (4.1), version nodes (4.2), executor scratch (5.3). `reset` at txn-end is the entire deallocation story for a transaction.
- **Side-effect / durability requirement:** between `create`/`reset` and the next `reset`, every returned pointer is valid and stable. After `reset`, every previously returned pointer is dangling by contract — the lifetime class names whose job it was to know that.
- **Critical contract details:** no per-object free exists — by design, not omission. Steady-state cycles (alloc to a high-water mark, reset, repeat) stop touching the system allocator once chunks are warm. Zero-size alloc is legal and returns a unique or null pointer — pick one, document it, the harness accepts either consistently. Not thread-safe; one arena per owner.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS — including the 10M-op ledger fuzzes — on plain, ASan, and UBSan builds with the same seeds.
2. `slab-no-heap` / `arena-no-heap` green: zero interposed malloc calls in steady state.
3. Benchmark table in RESULTS.md: ns/op for slab alloc+free, arena alloc, and malloc/free baselines, with hardware noted.
4. RESULTS.md published; a stranger can rerun fuzz and bench with one command each.

## Principal-engineer traps (no solutions)

- **Alignment must survive your own header.** A 16-byte bookkeeping header in front of a "4096-aligned" frame silently un-aligns every frame after the first, or steals usable size. Where metadata lives relative to frames is *the* slab design decision; the `slab-alignment` row checks every frame, not the first.
- **Buffer lifetime is owned by the in-flight I/O, not the caller.** This phase's `free` contract — "nothing still references this frame" — sounds trivial until 0.3, when the kernel holds a reference your code can't see. Design the contract language now so the buffer pool inherits it correctly; recycling a frame under a live SQE is the bug class this whole tier exists to make impossible-by-construction.
- **Arenas make free trivial and dangling pointers epidemic.** Nothing stops a query-lifetime structure from holding a pointer into a txn arena that resets first. The lifetime classes are only as real as your discipline in tagging allocations with the *shortest-lived* thing that references them.
- **The intrusive freelist is stored in memory you just declared dead.** A use-after-free through a stale frame pointer doesn't crash — it corrupts your freelist links, and the *next* alloc returns a garbage address. Think about what a debug build can stamp or check to turn that delayed detonation into an immediate one.
- **ASan can't see inside your slab.** To the sanitizer, a chunk is one big allocation; frame-to-frame overflow within it is invisible. Don't let three green sanitizer rows lull you — know which referee (ledger stamps vs ASan) covers which bug, and don't claim coverage you don't have.
- **Benchmark honesty.** A bump alloc the compiler hoisted out of the loop benchmarks at 0 ns. If your numbers look too good, they are.

## What you hand back for review

1. Implementation + harness table (all three builds) + the ns/op benchmark table.
2. One sentence per trap above: did it bite you, and how did you resolve it?

I'll review principal-engineer style: correctness, the interview attack ("a txn arena reset while the io engine still owns a frame from it — whose bug, by contract?"), whether the benchmark numbers are honest, and the next upgrade. Then we advance to Phase 0.3.

*Start Part 1 when ready — chunk layout and freelist are yours to design from a blank file.*

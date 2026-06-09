# Tier I · Phase 1.2 — Disk manager & heap files (scaffold)

**Deliverable of this phase:** a `DiskManager` + `HeapFile` over the Phase 0.3 io engine — page allocation/deallocation, file extension, a rebuildable free-space map, per-page CRC enforced on every read, metadata page 0 protected by the 0.1 atomic-write primitive — with the harness in `test/disk_manager/` green, including close/reopen invariance and a crash-mid-allocate torture pass.
**What you'll own afterward:** the mapping from `page_id` to durable bytes. When a page reads back wrong three tiers from now, you'll know exactly which layer to suspect — codec, this mapping, or the cache above it — because you built the seams between them.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is the mapping from `page_id` to durable bytes — not a buffer pool and not a filesystem.** It owns one file (plus its metadata page 0), knows which page ids are allocated, extends the file when it runs out, hands pages back with their CRC verified, and exposes a heap-file abstraction (unordered tuple bag with insert and full scan) on top. **There is no caching here.** Every `read_page` goes to the io engine; every `write_page` goes to the io engine. The buffer pool is Phase 1.3 and it sits *on* this layer — if you find yourself keeping page copies around to avoid I/O, you are building next phase's artifact in the wrong place. Likewise, you are not re-implementing the filesystem's job: block placement, journaling, and caching of the file's own metadata belong to the kernel; your job is the page-granular contract above it.

You're done with the framing when you can say: the deliverable is *a crash-tolerant `page_id` → bytes mapping plus a tuple bag over it*, not *a cache* and not *a storage stack*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
You'll be the kind of engineer who reasons correctly about what survives a crash at this layer and what doesn't — who knows that "the file got longer" and "the page exists" are separate facts with a window between them, that a free-space map is an accelerator whose lies must be survivable, and that exactly one page in the file (page 0) is allowed to be the single point of truth and must therefore be the single page written atomically. This is the metadata-vs-data discipline that separates storage engineers from people who call `write()` and hope.

### Why "from scratch" is the right call here
Take a stock storage layer and these go opaque — and each is a future debugging moment in this very plan:

- Phase 3.3's recovery rewrites pages through this layer; if you don't own the allocation/extension semantics, you can't reason about redo onto a page the crash left past end-of-valid.
- The crash-torture rig (here and at Phase 6.2) injects failures at the `io::` seam below you; interpreting its findings requires knowing exactly what this layer promised and what it merely cached.
- A CRC failure on read is your first line of corruption detection forever after — where it's enforced, and what the error path does, is a design you must own before Tier III piles WAL semantics on top.
- The FSM-as-cache discipline (truth lives in the pages, the map only accelerates) is the same truth-vs-cache split you'll re-apply to the Phase 5.1 schema cache and the Phase 3.2 dirty page table.
- Page 0's atomic update is the first *real* consumer of the 0.1 primitive — using your own primitive under torture is how you find out whether 0.1's contract was actually sufficient.

### What carries forward to later tiers
- **Phase 1.3** sits directly on this: the buffer pool's fetch/flush are calls into `DiskManager`, and its frames hold pages this layer vended.
- **Phase 3.x** recovery reads and rewrites pages through it, and inherits your answer to "what is a valid file after a crash during extension?"
- **Phase 5.1** catalog tables are just heap files — `chronos_tables` is a `HeapFile` like any other, bootstrapped from roots stored in page 0.
- **Phase 6.2**'s fault-injecting shim slots in at the `io::` seam below this layer; the invariants your torture verifier checks here become rows in that rig.
- The per-page CRC is the integrity floor every later phase silently relies on.

### What good looks like
- `DiskManager` is small. Allocation, deallocation, read, write, sync, open/close — if the surface is sprouting helpers, the design is leaking.
- You can answer cold: *"After a crash during `allocate_page`, what states can the file be in, and how does reopen handle each?"* and *"What happens if the FSM claims page 12 has 500 free bytes and it has 40?"* (Required answer to the second: an insert retry/skip, never corruption.)
- Close/reopen is invariant: anything written and synced reads back identical, CRC-verified, forever.
- A torn or bit-flipped page surfaces as a CRC error at read time — a `Result` error, never a parse of garbage.
- The FSM can be deleted on disk and rebuilt from the pages alone, with identical subsequent behavior.

### Why this is the shape of the deliverable
The failure mode of this layer is not "wrong answer" — it's "wrong answer *after a crash, on the reopen path, weeks later*." Ordinary tests can't see that; only a torture loop that kills the process at randomized points during allocation/extension, then runs an independent verifier over the raw file, can. Hence the shape: a library plus a crash rig (reusing 0.1's SIGKILL machinery) plus a shadow-model differential for the non-crash semantics — three oracles for the three ways this layer lies.

## Exam questions this phase targets (build-proven)
1. Implement `DiskManager` page alloc/dealloc/read/write with file extension and per-page CRC such that the harness's shadow-model differential and close/reopen rows PASS.
2. Survive the crash-mid-allocate torture loop: across hundreds of randomized SIGKILL points during allocation/extension, the independent verifier finds a readable, self-consistent file every time — with trailing garbage past the last allocated page tolerated and reclaimed.
3. Demonstrate the FSM is a cache: delete it, reopen, and pass the same insert workload; feed it a deliberately corrupted FSM (a harness row does) and corrupt nothing.

## Prerequisites — concepts this phase uses

Vocabulary, not the build. A strong systems programmer who has never built a database should recognize each term's role here.

### file-level storage
- **heap file** — the unordered tuple bag: a sequence of slotted pages with insert-anywhere and full-scan semantics, no ordering. The default table representation in row stores (PostgreSQL's "heap": https://www.postgresql.org/docs/current/storage-file-layout.html).
- **page allocation / deallocation** — minting a fresh `page_id` backed by real file bytes, and returning one to a reusable pool. The bookkeeping of *which ids are live* is this layer's core state.
- **file extension** — growing the file to back new pages (`ftruncate`/`fallocate`/writing past EOF — the mechanism is your choice; the crash semantics of the choice are the build). Extension and allocation are distinct events with a crash window between them.
- **metadata page / page 0** — the one page describing everything else: file geometry, allocation roots, heap-file first-page pointers (Phase 5.1 bootstraps from it). The file's superblock, in filesystem terms.

### integrity & crash tolerance
- **per-page CRC** — the Phase 0.1 slice-by-8 CRC32 stored in each page on write and verified on every read. Detects torn writes and bit rot at the earliest possible moment; the role is *detection*, never correction.
- **torn write** — a crash leaving a page partially old, partially new (Phase 0.1 demonstrated this). Any data page can tear; CRC catches it on read. Page 0 cannot be allowed to tear at all — hence the 0.1 atomic-write primitive.
- **atomic page-write primitive (Phase 0.1)** — your shipped mechanism guaranteeing a page update is all-or-nothing across a crash. Here it guards exactly one customer: page 0.
- **crash consistency / reopen invariant** — the property that after SIGKILL at any instant, reopening yields a self-consistent state: everything acknowledged-durable is intact, and anything in-flight is either absent or safely reclaimable.

### caches vs truth
- **free-space map (FSM)** — a per-page free-byte summary that accelerates "which page can take this tuple?" Its defining property here: it is a **rebuildable cache, never truth**. Truth is the pages' own headers. PostgreSQL's equivalent: https://www.postgresql.org/docs/current/storage-fsm.html
- **shadow model** — the harness's trivially-correct in-memory `page_id → bytes` map, compared against your layer after every operation and after reopen.

## How you know you're aligned (the cross-check)

The harness in `test/disk_manager/` is the continuous oracle — build a part, run it, watch rows flip SKIP→PASS. It is independent of your code three ways:

1. **Shadow-model differential:** the harness maintains its own `page_id → bytes` map. Randomized allocate/write/read/deallocate workloads run against both; any read divergence fails immediately. Then: close, reopen, full-scan compare — the durability half of the same oracle.
2. **Crash torture:** reusing the Phase 0.1 SIGKILL rig, the harness kills the engine at randomized points during allocation- and extension-heavy workloads, then runs an **independent verifier over the raw file bytes** (built on the 1.1 format spec + this phase's page-0 layout doc, no engine headers). The verifier demands a readable, self-consistent file: page 0 valid, every allocated page CRC-clean, every acknowledged write present. **Trailing garbage past the last allocated page is allowed and expected** — the verifier checks that *your reopen path* tolerates and reclaims it, not that it's absent.
3. **FSM independence rows:** the harness deletes the on-disk FSM (rebuild must succeed) and corrupts it (inserts must still land safely).

The harness sees only the public surface below. Allocation bookkeeping structure, extension batching, FSM encoding, page-0 internal layout (beyond what your doc freezes) — private, yours.

## The build, in parts (each gated independently by the harness)

### Part 1 — DiskManager core 🔨
Open/create over the 0.3 io engine; `allocate_page`/`deallocate_page` with id reuse; `read_page`/`write_page` with CRC stamped on write, enforced on read; `sync`. Page 0 holds the truth and is updated only through the 0.1 atomic-write primitive. **Harness rows:** shadow differential, CRC-detection (harness flips bits in the raw file; the next read must return the documented error), close/reopen invariance, alloc-after-dealloc id-reuse semantics.

### Part 2 — Crash-tolerant extension & allocation 🔨
Make the crash window between file extension and "page exists" survivable: after SIGKILL anywhere in the allocate path, reopen yields a consistent manager and leaked file space is reclaimed (immediately or lazily — your documented choice). **Harness rows:** the torture loop (hundreds of seeded crash points), the verifier's self-consistency pass, a post-recovery workload row proving the reopened manager allocates/reads/writes correctly.

### Part 3 — HeapFile + the FSM as a rebuildable cache 🔨
`HeapFile` insert/erase/read/scan over `DiskManager`, with an FSM steering inserts to pages with room. **Harness rows:** heap differential fuzz vs a shadow multiset of tuples, scan-completeness after reopen, FSM-delete-and-rebuild, FSM-corruption-tolerance (lying FSM ⇒ retried/redirected insert, never a corrupt page).

### Part 4 — The harness as a published artifact 🔨
Every row green, including torture under ASan/UBSan builds. `docs/format/file_v1.md` documents page 0 and the allocation structures the verifier reads. Write `RESULTS.md` from `templates/RESULTS.md`: the table, torture iteration count, and a paragraph on the crash window you found nastiest. Publish.

## API contract (what the harness links against)

Code under `src/storage/`. `Result<T>` error returns throughout; single-threaded contract this phase (Phase 1.3 adds the concurrency story above this layer).

**Public surface vs. private internals.** The harness touches only what's below. Allocation bookkeeping (bitmap? freelist? linked pages?), extension batch size, FSM representation, and lazy-vs-eager reclamation are **PRIVATE INTERNALS — deliberately unspecified**; the only constraint is that `docs/format/file_v1.md` freezes whatever the independent verifier must parse (page 0, allocation truth).

### `chronos::storage::DiskManager`

```cpp
class DiskManager {
  static Result<DiskManager> open(const char* path, io::Engine& io);  // creates if absent
  Result<PageId> allocate_page();
  Result<void>   deallocate_page(PageId id);
  Result<void>   read_page (PageId id, std::span<std::byte, kPageSize> out);
  Result<void>   write_page(PageId id, std::span<const std::byte, kPageSize> in);
  Result<void>   sync();          // all previously acknowledged writes durable on return
  Result<void>   close();
  uint64_t       allocated_page_count() const;
};
```

- **What it does:** the durable `page_id → bytes` mapping. `allocate_page` returns an id backed by real file bytes; `read_page` returns exactly the last written bytes for that id or a CRC/error result; `deallocate_page` retires an id for reuse.
- **Why it exists:** every real engine has this seam (PostgreSQL's smgr/md layer, InnoDB's fil/fsp). It is where "the database" meets "a file," and where crash semantics of allocation live.
- **Side-effect / durability requirement:** after `write_page` + `sync` return success, the bytes survive SIGKILL and power-cut-modeled torture, and read back CRC-verified after reopen. After a crash at *any* point, `open` on the same path succeeds and yields a manager whose allocated pages all read back consistently. Page-0 updates are atomic across crashes (0.1 primitive).
- **Critical contract details:** `read_page` of an unallocated/deallocated id is an error. A CRC mismatch is a distinct error code from I/O failure — callers (and Phase 3.3) must be able to tell them apart. Reads/writes are whole-page only, buffers aligned per the 0.3 engine's O_DIRECT requirements (allocated via the 0.2 slab allocator). `sync` failure follows the 0.1 durability contract — no retry-and-pretend.

### `chronos::storage::HeapFile`

```cpp
class HeapFile {
  static Result<HeapFile> create(DiskManager& dm);          // returns a heap rooted at a first page
  static Result<HeapFile> open(DiskManager& dm, PageId root);
  Result<Rid>  insert(std::span<const std::byte> tuple);
  Result<void> erase(Rid rid);
  Result<size_t> read(Rid rid, std::span<std::byte> out);   // bytes copied, or error
  Result<HeapScan> scan();                                  // forward iterator over live tuples
  PageId root() const;                                      // stored by callers (page 0 / catalog)
};

class HeapScan {
  Result<bool> next(Rid& rid_out, std::span<std::byte> tuple_out, size_t& len_out);
  // false ⇒ exhausted; tuples appear exactly once, order unspecified
};
```

- **What it does:** an unordered tuple bag. `insert` finds a page with room (FSM-accelerated), grows the heap when none has space, returns a stable RID; `scan` visits every live tuple exactly once.
- **Why it exists:** the table representation until Tier II adds indexes — and *still* the table representation after: indexes point into heaps. Phase 5.1's system catalogs are exactly these.
- **Side-effect / durability requirement:** an inserted tuple, once the enclosing sync discipline is satisfied, survives reopen and appears in `scan`. RIDs remain valid across close/reopen.
- **Critical contract details:** `insert` must succeed despite an FSM that is stale, missing, or corrupt — the FSM may cost extra page reads, never correctness. `read`/`erase` of a dead RID is an error. Scan order is explicitly unspecified (don't let the harness's shadow-comparison tempt you into promising one).

### Independent verifier (separate binary)

```
tools/filecheck <dbfile>   → exit 0 iff page 0 parses per docs/format/file_v1.md,
                             every allocated page is CRC-clean, allocation state is
                             self-consistent; trailing unallocated bytes are ignored
```

Built from the format docs only — no `src/` headers (the harness verifies the include rule). This is what the torture loop runs after every kill.

## Acceptance criteria (phase-level "done")
1. `test/disk_manager/` — all rows PASS under ASan+UBSan: shadow differential, CRC fault-injection, close/reopen invariance, FSM delete/corrupt rows.
2. Crash torture: ≥ 500 seeded SIGKILL iterations through allocation/extension paths, `tools/filecheck` green after every one, post-recovery workload row green.
3. `docs/format/file_v1.md` published (page 0 + allocation truth); `tools/filecheck` shares zero engine headers.
4. `RESULTS.md` published; a stranger can rerun everything, torture included.

## Principal-engineer traps (no solutions)
- **File extension and "page exists" are not atomic.** After a crash the file may be longer than the highest valid page. The wrong instinct is to treat file length as allocation truth; the file *will* lie. Tolerate the trailing garbage and reclaim it — the torture verifier explicitly permits it on disk and demands your reopen path handle it.
- **The free-space map is a cache.** Corrupting the database because the FSM lied is a classic real-world bug class. Every consumer of the FSM must verify against the page itself before committing bytes to it.
- **Page 0 is the single point of torn-write death.** Every other page tearing is detectable-and-survivable via CRC; page 0 tearing is game over. It goes through the 0.1 primitive, *every* update, no fast paths — torture will find the one you skipped.
- **Deallocated-id reuse vs the shadow model:** the harness's differential will expose any window where an old id's stale bytes are readable under a reused id. Decide when reused pages are scrubbed/reinitialized and make read semantics airtight.
- **CRC enforced on *every* read means every read** — including reads inside your own allocation and FSM-rebuild paths. The bit-flip row flips bits in pages you read internally, too.

## What you hand back for review
1. Implementation + harness table + torture iteration count + `docs/format/file_v1.md` + `tools/filecheck`.
2. One sentence per trap: did it bite, how resolved.

Review is principal-engineer style: the interview attack ("enumerate the crash states of `allocate_page` and your reopen handling for each"), overstated claims, the next upgrade. Then Phase 1.3.

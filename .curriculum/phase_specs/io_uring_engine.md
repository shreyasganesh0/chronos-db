# Tier 0 · Phase 0.3 — io_uring async I/O engine (scaffold)

**Deliverable of this phase:** `chronos::io` — a real async I/O engine over io_uring (SQ/CQ management, registered buffers and fds, read/write/fsync with completion callbacks, short-I/O resubmission, queue-depth backpressure), proven by a randomized differential fuzz against a synchronous pread/pwrite reference plus a strace-measured syscalls-per-op budget that shows submission batching is real.
**What you'll own afterward:** the single seam through which every byte of the database will flow — and the ability to reason about asynchronous completion, buffer ownership, and kernel-visible ordering when a page goes missing four tiers from now.
**Calibration:** 🔨 (SQPOLL and IOPOLL modes are 📖 — understand them, don't ship them)
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface (file bytes, read results, callback semantics, syscall counts); ring management internals are yours. Raw syscalls vs liburing is **your call, made and documented in this phase** — the contract below is identical either way.

## What this is — and is not

**This is an I/O engine, not an event framework.** It owns one io_uring instance and exposes exactly three operations — read, write, fsync — submitted asynchronously and completed via callbacks, with the unglamorous production concerns handled inside: short reads and short writes resubmitted at the right offset, backpressure when the queue is full, completions never lost to CQ overflow. It is **not a thread pool** (no hidden threads; callbacks run on the thread that reaps completions), **not a generic reactor** (no timers, no signals, no event abstraction — network I/O arrives in 6.1 as a *consumer* of this engine, not a redesign), and **not the disk manager** (it knows offsets and buffers, never pages or files-with-meaning — that's 1.2's job, built on top).

The reason this engine earns a whole phase: it is the seam. Every page read, every WAL flush, every network byte eventually passes through this interface — which is exactly why Phase 6.2 injects torn writes, fsync lies, and EIO *here* and nowhere else. An interface important enough to be the fault-injection seam is important enough to be built deliberately.

Right-artifact test: you're done with the framing when you can say the deliverable is "the one async I/O seam everything flows through — not a thread pool, not a framework, not the disk manager."

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

Thinking in asynchronous completions: an operation is not "done" when you submit it, a buffer is not "yours" while the kernel holds it, and "I submitted A before B" implies nothing about order. You'll be the kind of engineer who, facing a corrupted page under load, asks "what was in flight, who owned that buffer, and what ordering did we actually request from the kernel" — the three questions that solve most async-I/O bugs and that no amount of synchronous-I/O experience teaches.

### Why "from scratch" is the right call here

Wrap a ready-made async runtime around the database and the following go opaque, each a later debugging moment in this plan:

- Phase 3.1's group commit is the literal performance thesis of this engine: many transactions, one batched submission, one fsync. If you can't see the submission boundary, you can't build or measure group commit.
- Phase 6.2's fault shim wraps this interface. Faults you inject must be faithful to real failure modes — short writes, lying fsyncs, dropped completions — which you only know intimately by having handled them here.
- A buffer recycled while an SQE references it corrupts a random page at a random later time. Frameworks hide buffer ownership behind futures; chronos's contract (0.2's slab + this engine) makes it explicit, and you need to have *written* the ownership rule to enforce it in the buffer pool.
- When 6.3's benchmarks underperform, the first suspect is syscalls-per-op and queue depth. You'll have measured both with strace in this phase, on your own engine.

### What carries forward to later tiers

- **Phase 1.2 (disk manager):** the first consumer — page reads/writes become engine ops at `page_no * 4096` offsets in slab frames.
- **Phase 3.1 (WAL):** group commit rides this engine's batched submission and ordered fsync; the commits/sec-vs-latency curve is this engine's batching made visible.
- **Phase 6.1 (network server):** the same ring carries socket I/O (multishot accept, recv/send) — the engine you ship here is the server's event loop substrate.
- **Phase 6.2 (torture rig):** the fault-injecting shim implements this exact interface; every fault the rig can express is a behavior this phase's contract names.
- **Phase 0.2 backward edge:** registered buffers are slab frames — the stable-address guarantee you built last phase is what makes one-time registration legal.

### What good looks like

- The public surface is one engine class and three operation entry points; consumers never see an SQE, CQE, or ring offset.
- You can answer, without running it: "what happens if I submit at full queue depth?" (a clean `kQueueFull`, by contract — never a silent drop or a block) and "when is the caller's buffer safe to reuse?" (exactly at callback, never before — including across internal resubmissions).
- A burst of completions larger than the CQ does not lose a single callback — and you can explain which kernel flag tells you it almost did.
- The differential fuzz is byte-exact against the synchronous reference across every seed, including schedules engineered to force short I/O and overflow.
- The strace row shows real batching: far fewer `io_uring_enter` calls than operations at depth, and you can state your measured ratio.

### Why this is the shape of the deliverable

Async I/O bugs are reordering and lifetime bugs — invisible to unit tests that await each op in sequence. The honest oracle is differential: run the *same* seeded schedule of reads, writes, and fsyncs through your engine and through a trivially-correct synchronous pread/pwrite reference, then require the final file bytes and every read's returned bytes to match exactly. The reference is ~50 lines, obviously correct, and shares nothing with your engine — that's what makes the green row proof. The strace budget exists because "async" without batching is synchronous I/O with extra steps; the claim ships with a measured number.

## Exam questions this phase targets (build-proven)

1. Implement `chronos::io::Engine` such that the `differential-fuzz` rows PASS: across N seeded randomized schedules of read/write/fsync, final file contents and every read result are byte-identical to the synchronous reference.
2. Implement short-I/O resubmission such that the `short-io` row PASSes — the harness's schedules force partial transfers, and every callback must still report the full requested transfer (or true EOF), exactly once.
3. Measure syscalls per operation with the `strace-batching` row: prove ≥ 8 operations per `io_uring_enter` at queue depth ≥ 8 on the batched schedule, and report your ratio.
4. Implement backpressure and overflow handling such that `queue-full` (clean `kQueueFull` at depth, recovery after reaping) and `cq-overflow` (no completion lost under a burst exceeding CQ size) both PASS.

## Prerequisites — concepts this phase uses

**Already banked — not re-taught here:** you built io_uring fundamentals during the partial `cat` clone (pre-reset git history): the SQ/CQ ring model, SQE/CQE field layout, `io_uring_setup`/`io_uring_enter`, and mmap'ing the rings. This spec assumes all of that cold. What follows is only what's **new** for a production engine. Reference set: [io_uring_setup(2)](https://man7.org/linux/man-pages/man2/io_uring_setup.2.html), [io_uring_enter(2)](https://man7.org/linux/man-pages/man2/io_uring_enter.2.html), [io_uring_register(2)](https://man7.org/linux/man-pages/man2/io_uring_register.2.html), Axboe's ["Efficient IO with io_uring"](https://kernel.dk/io_uring.pdf), and [Lord of the io_uring](https://unixism.net/loti/).

### Registered resources

- **registered (fixed) buffers** — `io_uring_register(IORING_REGISTER_BUFFERS)` hands the kernel a set of buffers once; it pins their pages and skips per-op page-table walks. Ops then use `IORING_OP_READ_FIXED`/`WRITE_FIXED` with a buffer *index*. Role here: the slab's frames, registered at startup — the performance reason 0.2 guaranteed stable addresses.
- **registered files** — `IORING_REGISTER_FILES` pre-registers fds so each op skips the per-op fd refcount (`fget`/`fput`); ops pass an index with `IOSQE_FIXED_FILE`. Matters at high op rates; mandatory vocabulary for 6.1's socket loop.

### Ordering & completion hazards

- **`IOSQE_IO_LINK` / `IOSQE_IO_HARDLINK`** — chains consecutive SQEs: each starts only after the previous completes. On a link member's failure, the rest of the chain completes with `-ECANCELED` (hardlink continues regardless). One of the tools for ordering fsync after writes.
- **`IOSQE_IO_DRAIN`** — this SQE waits for *all* prior submitted ops; a sledgehammer ordering primitive with a throughput cost. Knowing when drain vs link vs wait-then-submit is appropriate is a design decision this phase forces.
- **CQ overflow** — if completions outrun your reaping, the kernel either drops them (signaled via `IORING_SQ_CQ_OVERFLOW` in the SQ ring flags) or, with `IORING_FEAT_NODROP`, queues them internally until you make room. An engine that never checks is an engine that silently loses callbacks under burst.
- **short I/O (`res < len`)** — a CQE's `res` is bytes transferred; less than requested is *not* an error (signals, EOF proximity, fs quirks). The op must be resubmitted for the remainder at an adjusted offset — distinguishing "short, retry" from "short because EOF" (the retry returns 0) is part of the build.

### Backpressure vocabulary

- **queue depth** — the engine's cap on in-flight operations, set at ring creation. The unit 6.3's benchmarks will sweep.
- **backpressure** — what the engine does at the cap: this contract says *reject cleanly* (`kQueueFull`), pushing the flow-control decision to the caller — the disk manager and WAL will each have their own policy.

### 📖 Understand, don't ship

- **SQPOLL** — a kernel thread polls the SQ so submission needs no syscall at all; costs a busy CPU and privileged setup. Know the trade; chronos doesn't ship it.
- **IOPOLL** — completion by polling the device (no interrupts) for low-latency NVMe; requires O_DIRECT and a polling-capable device. Same: know why it exists, where it wins, why it's out of scope.
- **liburing** — Axboe's thin userspace wrapper (ring setup, SQE helpers, barrier-correct ring updates). The build decision of this phase: raw syscalls (you own the memory-barrier discipline yourself — you've touched it in the cat clone) vs liburing (the plan's one permitted dependency). Either satisfies the contract; the choice and its rationale go in RESULTS.md.

## How you know you're aligned (the cross-check)

The harness in `test/io_uring_engine/` is your **continuous alignment oracle**. Its independence is structural: the **synchronous reference executor** replays the identical seeded schedule with plain `pread`/`pwrite`/`fdatasync` — code so simple it is its own proof — and the rows compare final file bytes and every read result byte-for-byte. Your engine and the reference share a schedule format and nothing else; there is no way to pass by implementing the same misunderstanding twice. The **strace rows** count syscalls from outside the process — your code can't influence the referee. Callback-discipline rows (exactly-once, correct totals, no callback after `kQueueFull` rejection) are asserted by the harness's own bookkeeping over the public callback surface.

Build a part, run the suite, watch rows flip SKIP → PASS. The harness never sees your SQE construction, ring bookkeeping, or resubmission machinery — only files, callbacks, return codes, and syscall counts. A green row is proof of observable shape, whatever the internals.

## The build, in parts (each gated independently by the harness)

### Part 1 — Ring lifecycle + single-op round trips 🔨
Engine create/destroy, one read, one write, one fsync, each completing through a callback with correct `Result`. Get the lifecycle boring before anything is concurrent.
**Harness rows:** `roundtrip-read`, `roundtrip-write`, `roundtrip-fsync`, `error-surface` (read from a bad offset / closed fd arrives as a callback error, not a crash or a lost op).

### Part 2 — Registered buffers, registered fds, batched submission 🔨
Register the slab's frame set and the working fd set; submit many ops per `io_uring_enter`. This is where the engine stops being a syscall wrapper.
**Harness rows:** `fixed-buffers` (differential mini-fuzz through registered frames), `fixed-files`, `strace-batching` (≥ 8 ops per enter at depth ≥ 8 on the batched schedule; ratio reported).

### Part 3 — The production concerns: short I/O, ordering, backpressure, overflow 🔨
Resubmission at adjusted offsets with exactly-once callbacks; `FsyncOrder::kAfterPriorWrites` honored; clean `kQueueFull` at depth; zero completions lost under CQ-overflow bursts.
**Harness rows:** `short-io` (schedules engineered to force partial transfers; callbacks report full totals or true EOF, exactly once), `fsync-order` (across adversarial schedules, an ordered fsync's callback never fires before any prior write's callback — the observable necessary condition; the durable sufficient condition is proven under 6.2's fault injection), `queue-full`, `cq-overflow` (completion burst > CQ entries; every callback still arrives).

### Part 4 — The differential soak + the harness as a published artifact 🔨
The full randomized fuzz: long seeded schedules mixing all ops, registered and unregistered paths, forced shorts and bursts — byte-exact vs the reference, repeated across seeds, ASan/UBSan builds included. Then `RESULTS.md` from `templates/RESULTS.md`: the table, the syscalls-per-op measurement, and the raw-vs-liburing decision with one honest paragraph of rationale.
**Harness rows:** `differential-fuzz` (N seeds, zero divergence), `soak-asan`, `soak-ubsan`.

## API contract (what the harness links against)

Code lives under `src/io/`. Everything below is the **public surface**. **PRIVATE INTERNALS — deliberately unspecified:** SQE/CQE bookkeeping, how you track in-flight ops and resubmission state, raw-vs-liburing, ring sizes vs `depth` (so long as the contract holds), memory-barrier discipline.

### `chronos::io::Engine`

```cpp
struct Config { unsigned depth; /* max in-flight ops */ };
using Callback = void (*)(void* ctx, Result<size_t> bytes);

class Engine {
  static Result<Engine> create(const Config&);
  // non-copyable, movable; destruction with ops in flight is a contract violation

  Result<BufGroup> register_buffers(std::span<const iovec> frames); // once, at startup
  Result<void>     register_files(std::span<const int> fds);

  Result<void> read (int fd, uint64_t off, std::span<std::byte> buf, Callback cb, void* ctx);
  Result<void> write(int fd, uint64_t off, std::span<const std::byte> buf, Callback cb, void* ctx);
  Result<void> fsync(int fd, FsyncOrder order, Callback cb, void* ctx);

  Result<unsigned> submit();                    // flush queued SQEs to the kernel
  Result<unsigned> poll_completions();          // reap + run callbacks; non-blocking
  Result<unsigned> wait_completions(unsigned min_n); // block until ≥ min_n reaped
};

enum class FsyncOrder { kUnordered, kAfterPriorWrites };
```

- **What it does:** queues asynchronous reads, writes, and fsyncs; `submit` pushes them to the kernel in batches; `poll`/`wait` reap completions and invoke each op's callback exactly once with the op's final result.
- **Why it exists:** the database's only I/O path. Batching here is group commit's substrate (3.1); the callback discipline is the buffer pool's frame-state machine (1.3); the interface itself is the fault seam (6.2).
- **Side-effect / durability requirement:** a write's callback with `Ok(n)` means `n` bytes are in the file (not necessarily durable — durability is fsync's job). An fsync callback with `Ok` means data written *and completed* before it is durable; with `kAfterPriorWrites`, all writes submitted to this engine before the fsync are durable on its `Ok` — by whatever internal mechanism (link, drain, wait-then-submit) you choose.
- **Critical contract details, the load-bearing ones:**
  - **Buffer lifetime:** the caller's buffer must remain valid and untouched from the op call until its callback runs — through any internal resubmissions. The callback is the *only* signal of buffer release.
  - **Exactly-once callbacks:** every accepted op gets exactly one callback — success, error, or cancellation at shutdown-with-error. An op rejected at the call site (`kQueueFull`, bad args) gets **no** callback.
  - **Short I/O is internal:** callbacks report total bytes for the whole requested range; `Ok(n)` with `n < len` is legal only at true EOF on reads. Callers never see partial transfers mid-range.
  - **Backpressure:** at `depth` in-flight ops, op calls return `kQueueFull` immediately — never block, never silently queue unboundedly.
  - **Threading:** the engine is single-threaded by contract — all calls and all callbacks on the owner's thread. (Cross-thread submission is a 6.1-era problem; don't pre-build it.)
  - **No completion loss:** CQ overflow must be detected and handled such that every accepted op's callback eventually arrives.

### `test/io_uring_engine/ref_exec` (the harness's reference — named so it's visibly non-circular)

The synchronous reference executor is **harness property, not yours to write**: it replays schedules with `pread`/`pwrite`/`fdatasync` in schedule order. Your only obligation to it is the schedule semantics above — if your engine's contract holds, byte-equality follows.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS — `differential-fuzz` across all N seeds with zero byte divergence, on plain, ASan, and UBSan builds.
2. `strace-batching` green with the measured ops-per-enter ratio recorded in RESULTS.md.
3. `short-io`, `queue-full`, `cq-overflow`, `fsync-order` all green — the production-concern rows are the phase, not extras.
4. RESULTS.md published: table, syscall numbers, kernel version, and the raw-vs-liburing decision with rationale. A stranger can rerun it.

## Principal-engineer traps (no solutions)

- **The kernel copies the SQE at submit — but not the buffer.** The SQE can be reused immediately after `io_uring_enter`; the *buffer* belongs to the kernel until the CQE. Conflating these two lifetimes is the classic io_uring use-after-free: it passes every light test and corrupts pages under load.
- **`res < len` is not an error.** Treat a short write as success and you've silently dropped a tail; treat it as failure and you'll "handle" an op that half-happened. Resubmission at the adjusted offset must survive *another* short result — and your exactly-once callback accounting must survive the resubmission.
- **CQ overflow is silent if you don't look.** Outrun the CQ without checking the overflow flag and completions vanish: a callback never fires, a buffer never releases, and the leak presents weeks later as pool exhaustion. The `cq-overflow` row exists because nobody checks until it's bitten them.
- **"I submitted the fsync last" is not happens-after.** io_uring executes independent SQEs in any order; an unlinked fsync can complete before the writes it was meant to cover, making it durability theater. You must *construct* the ordering — link, drain, or wait-then-submit — and know what each costs.
- **Backpressure by blocking is deadlock bait.** If a full queue makes the submit path block inside the engine, the caller that must reap completions to drain the queue may be the caller that's blocked. The contract says reject; respect why.
- **Error CQEs release buffers too.** An op that fails still owned its buffer until that failing CQE; releasing on the error path is where exactly-once accounting usually breaks first.

## What you hand back for review

1. Implementation + harness table + the syscalls-per-op measurement + the raw-vs-liburing decision paragraph.
2. One sentence per trap above: did it bite you, and how did you resolve it?

I'll review principal-engineer style: correctness, the interview attack ("walk me through buffer ownership from submit to a resubmitted short write's completion — who can touch the frame at each instant?"), whether the batching number is honest, and the next upgrade. Then Tier 0 is done and we advance to Phase 1.1.

*Start Part 1 when ready — and make the raw-vs-liburing call before you write a line, in writing, so the decision is yours instead of inertia's.*

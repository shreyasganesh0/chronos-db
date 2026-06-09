# Tier 0 · Phase 0.1 — The durability contract (scaffold)

**Deliverable of this phase:** a checksummed atomic-page-write primitive + a torn-write demonstrator, proven by a SIGKILL torture loop whose independent raw-format verifier asserts every page on disk is old-complete, new-complete, or CRC-detectably torn — plus a written durability-contract doc (`DURABILITY.md`) stating exactly what the kernel and the disk do and do not promise.
**What you'll own afterward:** every durability claim chronos ever makes — "this commit is on disk," "recovery will find this record" — reduces to physics you have personally demonstrated. When ARIES misbehaves two tiers from now, you debug it as "which write wasn't where I proved it must be," not "fsync is mysterious."
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface (the on-disk format frozen below, the CLI contracts, the exit codes); the internals are yours.

## What this is — and is not

**This is the physics layer, not a storage engine.** What you build here is a single primitive — write one 4 KB page such that a crash at any instant leaves the on-disk page in exactly one of three states: the complete old contents, the complete new contents, or a state your checksum detects as torn — plus the empirical proof that the primitive holds under hundreds of randomized SIGKILLs. It is not a file format (Phase 1.1), not a page allocator (1.2), not the WAL (3.1). There is no "database" anywhere in this phase: no pages with slot directories, no records, no recovery. If you catch yourself designing a header layout with tuple counts or a free-space pointer, stop — that is Tier I's artifact.

It is also **not a guarantee of atomicity** — that's the point. Disks do not promise 4 KB atomic writes, and nothing you write in userspace can make them. The deliverable is the honest version: *detectable* tearing plus *proven* durability ordering. Every later crash-safety mechanism (WAL valid-prefix, the master record, ARIES redo) is built to live within exactly the promises you document here, and no more.

Right-artifact test: you're done with the framing when you can say the deliverable is "a primitive + proof that a 4 KB page write is either old-complete, new-complete, or detectably torn — not a database."

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

The ability to reason about crash consistency from first principles: what `write(2)` actually promises (nothing durable), what `fsync` actually promises (less than you think, and it can lie once), what the disk promises (at most one sector), and how to compose a real guarantee out of those weak parts. You'll be the kind of engineer who, told "the data was committed but it's gone," asks for the strace and the block-device sector size instead of blaming the filesystem.

### Why "from scratch" is the right call here

Use a library's "atomic write" or trust the filesystem's defaults and the following stay opaque, each of which is a debugging moment later in this plan:

- Phase 3.1's WAL correctness rests entirely on "an fsync'd prefix of the log survives, and the CRC finds the torn tail." If you've never produced and detected a torn write yourself, you'll write a recovery loop that trusts garbage.
- Phase 3.2's master record is overwritten in place; it survives crashes only because this phase's primitive makes overwrite-in-place crash-detectable. Treat that as magic and the checkpoint design has a silent hole.
- Phase 6.2's fault injector simulates fsync lies and torn writes. You can't simulate faithfully what you've never observed.
- The fsyncgate failure mode (a failed fsync whose retry "succeeds" over lost data) took down real PostgreSQL deployments and was misunderstood by professional database authors for a decade. It's a behavior you verify against your kernel, not a fact you read.

### What carries forward to later tiers

- **Phase 1.2 (disk manager):** metadata page 0 is written with this exact primitive; per-page CRC-on-read is this phase's read path generalized.
- **Phase 3.1 (WAL):** the fsync-before-ack discipline proven here by strace becomes the group-commit protocol; the CRC-detected torn tail becomes the valid-prefix rule.
- **Phase 3.2 (checkpoints):** the master record is the primitive's flagship consumer — a single page whose overwrite must be old/new/detectable.
- **Phase 6.2 (torture rig):** the fork-SIGKILL-verify rig shape you meet here is generalized to full-SQL workloads; the independent raw-format verifier is the ancestor of the rig's invariant scanners.
- **`Result<T>` discipline:** this phase establishes chronos's no-exceptions error convention; every later API contract uses it.

### What good looks like

- You can answer, without notes: "why doesn't fsyncing a newly created file make it survive a crash?" and "what is the largest write Linux + your disk guarantee is atomic?" — both with build-proven specifics from your own machine.
- Your CRC32 is slice-by-8 and measurably fast (multiple GB/s on a modern core; the harness compares it against a bitwise reference for both correctness and speedup).
- The primitive's failure modes are exhaustively enumerated in `Result` codes — a torn page is distinguishable from an I/O error, which is distinguishable from a never-written page.
- Hundreds of torture iterations produce zero verifier violations, *and* at least some iterations produce genuinely torn pages in the naive-writer control — proof the rig can actually tear, so the green run means something.
- `DURABILITY.md` reads like a contract, not an essay: numbered promises, each tied to the harness row or man-page section that backs it.

### Why this is the shape of the deliverable

Crash-timing bugs are silent by construction: code that loses data on power failure passes every test that doesn't cut the power. The only honest oracle is an external process that kills the writer at uncontrolled instants and inspects the raw bytes with independent code — hence a torture loop, hence a verifier that knows only the frozen on-disk format and never links your code, hence strace as a third referee for the ordering claim. The written doc is part of the ship because the *contract* — not the code — is what every later phase imports.

## Exam questions this phase targets (build-proven)

1. Implement `crc32` (slice-by-8) such that the harness's known-answer rows PASS, and measure its throughput against the harness's bitwise reference — report both numbers.
2. Demonstrate a real torn write: the `naive-writer` control row must observe at least one CRC-detected torn page across the torture run, proving the rig tears and the checksum catches it.
3. Implement `write_page_atomic` / `read_page_verified` such that the SIGKILL torture row PASSes: across ≥ 300 randomized kill points, the independent verifier finds every page old-complete, new-complete, or detectably torn — and every page the writer *acked as committed* present and intact.
4. Prove with the strace row that an `fdatasync`/`fsync` on the data file precedes every "committed" ack, and explain — from the harness row that creates files — why the parent directory fsync is load-bearing.

## Prerequisites — concepts this phase uses

These appear in the spec, the contract, and the harness. Recognize each term and its role; implementing the build around them is yours. Audience: a strong C/C++ systems programmer who has never built a database.

### The Linux I/O path

- **page cache / write-back** — the kernel buffers `write(2)` data in RAM and flushes it to disk later, at its leisure. A successful `write` proves nothing about durability; a crash discards everything unflushed. This single fact motivates the whole phase. Read: [LWN, "Ensuring data reaches disk"](https://lwn.net/Articles/457667/).
- **`O_DIRECT`** — an `open(2)` flag that bypasses the page cache: your buffer goes (more) directly to the device. The price: buffer pointer, file offset, *and* length must each be aligned to the device's logical block size. See the NOTES section of [open(2)](https://man7.org/linux/man-pages/man2/open.2.html).
- **`fsync(2)` vs `fdatasync(2)`** — block until file data (and, for `fsync`, metadata like mtime) is on stable storage. `fdatasync` may skip metadata-only flushes — but still flushes metadata needed to *find* the data, like file size. [fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html).
- **directory fsync** — creating a file durably is *two* operations: the file's data, and the directory entry pointing at it. The latter lives in the directory, which must itself be fsync'd. A fresh systems dev meets this the first time a "synced" file vanishes after power loss.
- **logical block size** — the device's atomic I/O unit as reported by the kernel; queried per-device (`ioctl(BLKSSZGET)` on the block device, or `statx`/`stat.st_blksize` heuristics on the file). The alignment unit `O_DIRECT` demands. Never assume 512.
- **fsyncgate** — on many kernels/filesystems, a *failed* fsync may mark the dirty pages clean anyway; the retry returns success over data that was never written. Documented in ["Can Applications Recover from fsync Failures?" (ATC '20)](https://www.usenix.org/conference/atc20/presentation/rebello). The contract consequence: a failed fsync on a data path is not retryable — it's fatal for that file's durability claims.

### What disks actually promise

- **sector** — the disk's atomic write unit: 512 bytes historically, sometimes 4 KB (4Kn drives). At most one sector is all-or-nothing. A 4 KB page spans up to eight sectors — up to eight independent chances to tear.
- **torn write** — a multi-sector write interrupted by crash/power-loss such that some sectors hold new data and others old. The central villain of this phase; background: ["All File Systems Are Not Created Equal" (OSDI '14)](https://www.usenix.org/conference/osdi14/technical-sessions/presentation/pillai).
- **write barriers / FLUSH / FUA** — the device-level mechanisms fsync rides on: a cache-flush command, or a per-write force-unit-access flag. 📖 vocabulary only — you invoke them via fsync, never directly.

### Checksums

- **CRC32** — a 32-bit cyclic redundancy check; detects all burst errors up to 32 bits and random corruption with probability ~1−2⁻³². Not cryptographic — it detects accidents, not adversaries, which is exactly the threat model for torn pages. This phase freezes the parameters: the IEEE 802.3 polynomial, reflected, init and final-xor `0xFFFFFFFF` — the variant whose check value for the ASCII bytes `"123456789"` is `0xCBF43926`. Reference: [Ross Williams, "A Painless Guide to CRC Error Detection Algorithms"](http://ross.net/crc/download/crc_v3.txt).
- **slice-by-8** — the standard table-driven technique processing 8 input bytes per iteration via 8 precomputed 256-entry tables. Its *role* is throughput (the WAL will checksum every record); designing the tables and the loop is the build.

### Testing machinery

- **SIGKILL semantics** — the process dies instantly: no atexit handlers, no buffered-stdio flush, no destructors. The closest userspace approximation of power loss (it does not defeat the kernel page cache — which is why the contract requires fsync *before* the ack the verifier trusts).
- **strace** — syscall tracer; `strace -f -e trace=pwrite64,fsync,fdatasync,write` gives the harness an ordering record it can assert against. [strace(1)](https://man7.org/linux/man-pages/man1/strace.1.html).
- **`Result<T>`** — chronos's value-or-error return type (the codebase is `-fno-exceptions`). This phase establishes it: a `Result<T>` holds either a `T` or an error code (negative errno, or a chronos-defined code like `kTornPage`). Its design is yours; the harness checks only observable success/failure semantics.

## How you know you're aligned (the cross-check)

You are never meant to build the whole phase blind and find out at the end. The harness in `test/durability_contract/` is your **continuous alignment oracle**, and it is deliberately stronger than prose: its verifier binary is written from the frozen on-disk format in this spec and *never links or sees your code* — it reads raw bytes and recomputes CRCs with its own independent implementation. Prose you can implement subtly wrong and still believe; an independent decoder over raw disk bytes you cannot fool. The CRC rows use published known-answer vectors; the ordering rows use strace, which your code cannot influence.

The loop: build one part, run the harness, watch that part's rows flip SKIP → PASS. A green row is proof your build has the required *observable* shape — the harness touches only the public surface (the on-disk page layout, the CLI contracts, exit codes, `Result` semantics). How you stage the write, order the syscalls internally, or structure the CRC tables is invisible to it and entirely your design.

## The build, in parts (each gated independently by the harness)

### Part 1 — CRC32, slice-by-8 🔨
The checksum everything else stands on. Frozen parameters above; known-answer vectors plus randomized cross-checks against the harness's bitwise reference.
**Harness rows:** `crc-known-answer`, `crc-incremental` (seeding a CRC continuation matches one-shot), `crc-vs-reference-fuzz`, `crc-throughput` (reports GB/s yours vs bitwise; PASS requires a real slice-by-8-class speedup).

### Part 2 — Direct I/O file layer 🔨
Opening files for direct, durable page I/O: `O_DIRECT` with queried (not assumed) logical block size, aligned buffers, and durable file *creation* — the parent-directory fsync.
**Harness rows:** `open-direct-alignment` (misaligned buffer/offset/length each rejected cleanly via `Result`, no crash), `block-size-queried` (works on a loopback device configured with a non-512 logical block size), `create-durable` (strace asserts fsync of the parent directory on create).

### Part 3 — The atomic-page primitive + torn-write demonstrator 🔨
`write_page_atomic` (checksum-then-write-then-sync, per the frozen layout) and `read_page_verified`, plus `tools/page_writer` — the loop binary the torture rig drives, including its `--naive` control mode that writes pages without the primitive's protection.
**Harness rows:** `roundtrip` (write/read/verify), `torn-detect-synthetic` (harness flips bytes / splices half-old-half-new pages; reads must return `kTornPage`, never garbage-as-success), `ack-after-sync` (strace: no "committed" line precedes the covering fdatasync).

### Part 4 — SIGKILL torture 🔨
The main event. The harness forks `page_writer` against a fresh file, SIGKILLs it at a randomized delay, runs the verifier over the raw file plus the writer's acked-commit log; repeat ≥ 300 seeded iterations.
**Harness rows:** `torture-atomic` (zero violations: every page old/new/detectably-torn; every acked page present and intact), `torture-naive-control` (the `--naive` mode produces at least one detected torn page across the run — proof the rig tears).

### Part 5 — The harness as a published artifact 🔨
Every row green, then two documents: `DURABILITY.md` — the numbered contract (what is promised, by whom, proven by which row) — and `RESULTS.md` from `templates/RESULTS.md` with the table, torture iteration count, CRC throughput numbers, and hardware/filesystem details (they matter here). Publish.

## API contract (what the harness links against / executes)

Code lives under `src/dur/`. Everything below is the **public surface** — the only things the harness touches. **PRIVATE INTERNALS — deliberately unspecified:** how you stage writes, your table layout for slice-by-8, your `Fd` wrapper's design, how `Result<T>` is represented. The on-disk page layout, however, is **frozen** (the independent verifier is built from it):

> **Frozen on-disk format — atomic page.** A page is exactly 4096 bytes at file offset `page_no * 4096`. Bytes 0–3: CRC32 (parameters frozen in Prerequisites) of bytes 4–4095, stored little-endian. Bytes 4–4095: caller payload.

### `chronos::crc32`

```cpp
uint32_t crc32(const std::byte* data, size_t len, uint32_t seed = kCrcInit);
```

- **What it does:** computes the frozen CRC-32 variant over `data[0..len)`; `seed` allows incremental computation over discontiguous buffers (continuing must equal one-shot over the concatenation).
- **Why it exists:** torn-page detection now; every WAL record and every heap-page read later. It is on the hot path of all of Tier III — hence slice-by-8.
- **Side-effect / durability requirement:** pure function; none.
- **Critical contract details:** must match the known-answer vector (`"123456789"` → `0xCBF43926`). No heap allocation. Thread-safe (no mutable state after table init).

### `chronos::dur::open_direct`

```cpp
Result<Fd> open_direct(const char* path, bool create);
```

- **What it does:** opens (optionally creates) `path` for direct I/O and discovers the device's logical block size; the returned handle exposes it (`fd.block_size()`).
- **Why it exists:** every durable file in chronos — heap files, WAL segments — is opened through this gate, so alignment and creation-durability are decided once.
- **Side-effect / durability requirement:** if `create` made a new file, the file's existence survives a crash after `Ok` returns (the directory entry is durable).
- **Critical contract details:** errors via `Result` (errno-style), no exceptions. Block size is queried, never assumed.

### `chronos::dur::write_page_atomic` / `read_page_verified`

```cpp
Result<void> write_page_atomic(const Fd& fd, uint64_t page_no, std::span<std::byte, 4096> page);
Result<void> read_page_verified(const Fd& fd, uint64_t page_no, std::span<std::byte, 4096> out);
```

- **What it does:** write — stamps the CRC into bytes 0–3 of the caller's buffer (computed over bytes 4–4095), writes the page at `page_no * 4096`, and makes it durable before returning `Ok`. Read — reads the raw page, recomputes the CRC, returns `Ok` only on match.
- **Why it exists:** the in-place-overwrite primitive under the WAL master record (3.2) and metadata page 0 (1.2) — the two places chronos overwrites a critical page without a log to protect it.
- **Side-effect / durability requirement:** after `Ok` from write, the page survives SIGKILL/power-loss intact — this is exactly what the torture row proves. After a crash mid-write, a subsequent `read_page_verified` returns either the old page, the new page, or `kTornPage` — never a silently wrong page.
- **Critical contract details:** `page` must satisfy the fd's alignment (caller's problem; rejected via `Result`, not UB). `kTornPage` is distinct from I/O errors. A failed sync poisons the durability claim — the fsyncgate rule: do not retry-and-trust. Not thread-safe per-fd; callers serialize (Tier I adds the locking story).

### `tools/page_writer` (CLI contract — the torture rig's subject)

```
page_writer <file> <page_count> <seed> [--naive]
```

- **What it does:** loops forever: pick a pseudorandom page in `[0, page_count)` (seeded), fill its payload with a version-stamped pattern **specified by the harness's pattern doc** (page number + monotonically increasing version, repeated to fill — so the verifier can classify old/new), write it via the primitive, then print `committed <page_no> <version>\n` to stdout and flush. `--naive`: same loop, but each page is written as two raw unsynced half-page `pwrite`s with no CRC discipline — the tearable control.
- **Why it exists:** the torture rig's subject process; the ancestor of every workload binary 6.2 drives.
- **Side-effect / durability requirement:** a `committed` line is an *ack* — the page at that version must survive SIGKILL delivered any instant after the line is written. (In `--naive` mode no such promise is made; that's the point.)
- **Critical contract details:** stdout lines are the verifier's trust anchor — never print before the primitive returns `Ok`.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS, including `torture-atomic` at ≥ 300 seeded iterations with zero verifier violations, on both regular and ASan/UBSan builds (the torture loop itself may run the regular build; unit rows run sanitized).
2. `torture-naive-control` shows ≥ 1 detected torn page — the rig provably tears.
3. `crc-throughput` reports your slice-by-8 vs bitwise numbers; both land in RESULTS.md.
4. `DURABILITY.md` published: numbered promises, each citing its harness row or man-page authority.
5. RESULTS.md published with hardware/filesystem details; a stranger can rerun everything.

## Principal-engineer traps (no solutions)

- **fsync of a file does not durably create the file.** The directory entry is data *in the parent directory*. The `create-durable` row exists because nearly everyone loses a file to this exactly once.
- **O_DIRECT alignment is three-fold:** buffer pointer AND file offset AND length, each to the *logical block size* — which you query, not assume. 512 is a guess that works until the first 4Kn drive or exotic loopback config.
- **fsyncgate:** after a failed fsync, dirty pages may be marked clean; the retry "succeeds" over lost data. Decide — and write into `DURABILITY.md` — what your primitive does on sync failure, and why retrying is the wrong answer.
- **Disks promise at most one sector.** A 4 KB write is up to eight independent sector writes — eight chances to tear. Any design that assumes "4 KB writes are atomic on my SSD" is folklore until your torture rig says otherwise, and even then it's only folklore about *your* SSD.
- **Checksum-then-write ordering is load-bearing.** Stamp the CRC after the payload is final; any later mutation of the buffer before the write lands silently widens the undetectable-corruption window.
- **The ack is part of the protocol.** Printing `committed` before the sync returns turns the torture row's strongest assertion into a lie; buffered stdio that flushes at exit (which SIGKILL skips) does the same thing in the other direction — think about what "the line was written" means under SIGKILL.

## What you hand back for review

1. Implementation + the harness table + torture iteration count + CRC throughput numbers + `DURABILITY.md`.
2. One sentence per trap above: did it bite you, and how did you resolve it?

I'll review principal-engineer style: correctness, any durability claim stated stronger than the harness actually proves, the interview attack ("your disk just lied about a flush — which of your promises still hold?"), and the next upgrade. Then we advance to Phase 0.2.

*Start Part 1 when ready — the CRC tables are yours to derive from the polynomial.*

# Tier I · Phase 1.1 — Slotted pages & tuple layout (scaffold)

**Deliverable of this phase:** a `Page`/`Tuple` byte codec for one 4 KB page and one tuple, a written on-disk format spec, the fuzz harness in `test/slotted_pages/` reporting every row PASS, and **golden hex dumps of page images checked into the repo** — the format is frozen the day this ships.
**What you'll own afterward:** every byte of every page chronos will ever write. When a B+Tree node looks corrupt two tiers from now, or recovery replays a record onto the wrong slot, you'll read the hex dump directly and know which field lied.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is a byte-codec plus a frozen format — not storage.** You are building two encoders/decoders: one that lays tuples, a slot directory, and a header into a single 4096-byte buffer, and one that lays typed column values (null bitmap, fixed-width columns, length-prefixed varlena columns) into a tuple's bytes. The page never touches a file in this phase. There is no disk manager, no heap file, no notion of "the database" — those are Phase 1.2. If you catch yourself opening a file descriptor or thinking about which page comes next, stop: that is the wrong artifact.

It is also not a transient in-memory container. The byte layout you choose here is a **public, permanent contract**: Phase 2.1 stores B+Tree nodes on these pages, Phase 3.1 stamps the `pageLSN` field you reserve now, Phase 5.1's catalog readers parse this format from the spec document alone. The golden images make drift impossible — change one byte of the layout later and a checked-in file fails the diff.

You're done with the framing when you can say: the deliverable is *a codec and a frozen format with stable slot-indirected RIDs*, not *a place to keep data*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
You'll be the kind of engineer who can take a raw hex dump of a database page and narrate it — header, slot directory growing down, tuple bytes growing up, which slots are live, where the free-space hole sits — and who designs binary formats with the discipline that they must be parseable forever by code that has never seen your headers. This is the core competency behind every storage-engine postmortem: corruption debugging starts at the byte level or it doesn't start.

### Why "from scratch" is the right call here
Use somebody else's page layout and the following stay opaque, and each one is a place you will bleed later:

- Why RIDs survive intra-page compaction (Phase 2.1's leaf entries and every secondary index point at RIDs; if you don't own the indirection, you can't reason about when it breaks).
- Why the `pageLSN` lives at a fixed header offset and what reserves it (Phase 3.1/3.3 — redo is gated on comparing it; a format that didn't plan for it gets rebuilt).
- Why a deleted slot is tombstoned vs reused, and what each choice costs recovery and MVCC (Phase 3.3 redo idempotence, Phase 4.2 version chains both inherit your answer).
- Why a grown UPDATE is a policy decision, not a code path (Phase 4.2's in-place vs out-of-place versioning sits exactly on this seam).
- Why an off-by-one in null-bitmap sizing silently shifts every subsequent field — the bug class behind a whole genre of "works until column 9" corruption reports.

### What carries forward to later tiers
- **Every later structure lives on these pages.** Phase 2.1's B+Tree nodes are slotted pages with a different payload discipline; the slot directory you build now is the node format then.
- **The reserved `pageLSN` slot** is the load-bearing seam for Tier III: Phase 3.1 stamps it on every modification, Phase 3.3's redo pass compares against it to decide whether to repeat history.
- **Stable RIDs** are the address space of the whole engine: Phase 2.x indexes store them, Phase 4.2 keys version chains by them, Phase 5.3's executors pass them between operators.
- **The grown-update policy** you decide and document now is inherited verbatim by Phase 4.2 MVCC.
- **The written format spec + independent decoder** pattern recurs at Phase 3.1 (`waldump` is built the same way) and Phase 5.1 (catalog readers parse this format independently).
- quackdb's L09 touched pages as immutable columnar blocks; this is the mutable, slot-indirected, row-oriented counterpart — the part the OLAP build never needed.

### What good looks like
- The header is small, fixed-layout, and fully documented in the format spec — including the `pageLSN` field that nothing writes yet. Every offset in the spec is a number, not prose.
- After any interleaving of inserts/deletes/updates, an insert that fits in *total* free space succeeds even when the free space is fragmented — and the RIDs of surviving tuples are unchanged afterward. You can state why without running anything.
- You can answer cold: *"What happens to slot 3's RID when slots 1 and 2 are deleted and the page compacts?"* and *"Where does the first varlena column start for a 9-column tuple with 3 nulls?"*
- The decode-only checker binary shares zero headers with the engine and still parses every page the engine produces.
- The format spec alone is sufficient for a stranger to write that checker — because that's literally how the harness validates it.

### Why this is the shape of the deliverable
A codec's bugs are silent: a wrong offset still "works" until a specific shape of tuple lands on a specific shape of free space. The only honest proof is differential — thousands of randomized operations compared against a trivially-correct shadow — plus a frozen-format guard, because the second failure mode of binary formats is *drift*: the code quietly changes the layout and every old page becomes garbage. Hence the fuzz harness, the spec-derived independent decoder, and the golden images that pin the bytes forever.

## Exam questions this phase targets (build-proven)
1. Implement a 4 KB slotted page (header with reserved `pageLSN`, slot directory growing down, tuple bytes growing up, intra-page compaction) such that the harness's insert/delete/update fuzz vs its shadow model passes 1M+ operations with zero divergence and zero RID instability.
2. Implement the tuple codec (null bitmap, fixed-width columns, length-prefixed varlena) such that the golden-image rows PASS byte-for-byte and the spec-derived checker decodes every fuzz-produced page.
3. State your tombstone-vs-reuse policy and your grown-update policy, and defend each against "what does Tier III/IV inherit from this choice?"

## Prerequisites — concepts this phase uses

These are vocabulary, not the build. Recognize the role each plays; designing the actual layout is the phase.

### page anatomy
- **slotted page** — the classic heap-page organization: a header at the front, an array of slot entries growing downward from the header, tuple bytes growing upward from the end, free space in the middle. The canonical design appears in every textbook engine; see the PostgreSQL page layout docs for a production example: https://www.postgresql.org/docs/current/storage-page-layout.html
- **slot directory** — the array of per-tuple entries (offset + length, plus whatever flags you need). Its index is the stable half of a tuple's address; the offset it stores is the *unstable* half that compaction may rewrite.
- **RID (record/row identifier)** — `(page_id, slot_index)`. The engine-wide address of a tuple. Stability of the slot index across compaction is what makes RIDs durable addresses.
- **pageLSN** — a header field holding the LSN of the last log record that modified the page. It does nothing in Tier I; Phase 3.1 stamps it and Phase 3.3's redo is gated on it. You reserve the bytes now so the format never changes.
- **intra-page compaction** — sliding live tuple bytes together to coalesce fragmented free space into one hole, updating slot offsets. A defragmentation of one page, in place.

### tuple anatomy
- **null bitmap** — one bit per column marking NULL; NULL columns occupy zero data bytes. Sizing and alignment of this bitmap determine where the first real column byte sits.
- **fixed-width column** — a type whose encoded size is known from the schema (e.g. a 4-byte integer). Decodable by offset arithmetic alone.
- **varlena (variable-length) column** — a length-prefixed byte string (the name is PostgreSQL's: https://www.postgresql.org/docs/current/storage-toast.html). Decoding requires walking, not arithmetic.
- **schema** — the ordered list of column types/nullability the codec needs to interpret tuple bytes. Tuples do not carry their own schema; the bytes are meaningless without it.

### format discipline
- **golden file / golden image** — a checked-in expected output compared byte-for-byte. Here: hex dumps of specific pages, freezing the format against drift.
- **endianness** — byte order of multi-byte integers in the format. Pick one, write it in the spec, never rely on "whatever the host does" without saying so.
- **differential fuzzing** — driving randomized operations through the system under test and a trivially-correct shadow model simultaneously, failing on first divergence.

## How you know you're aligned (the cross-check)

The harness in `test/slotted_pages/` is your continuous alignment oracle — build a part, run it, watch rows flip SKIP→PASS. It is deliberately stronger than prose, and it is **not circular**, three ways:

1. **Shadow-model fuzz:** randomized insert/delete/update sequences run against your `Page` and against an in-memory `std::map<slot, bytes>` shadow the harness maintains itself. After every operation, every live slot must read back identical bytes, and RIDs handed out earlier must still resolve.
2. **Spec-derived decoder:** a separate decode-only checker binary that parses pages from the *written format spec*, never including engine headers. If your code and your spec disagree, this catches it — which is the point.
3. **Golden images:** scripted operation sequences must produce pages byte-identical to the checked-in hex dumps. From the day these are committed, any layout change fails the suite.

The harness touches only the public surface in the API contract below. How you arrange the header internally (beyond the spec'd fixed fields), how compaction moves bytes, how you track the free-space hole — all private, all yours.

## The build, in parts (each gated independently by the harness)

### Part 1 — The page codec 🔨
A `Page` over a caller-provided 4096-byte buffer: init, insert, read, erase, update. Header with the reserved `pageLSN` field, slot directory down, tuple bytes up. **Harness rows:** basic round-trip, fill-to-capacity, erase-then-read returns the documented error, header fields match the spec.

### Part 2 — Compaction and stable RIDs 🔨
Insert into fragmented free space must succeed when total free space suffices — which forces compaction — and *no surviving slot index may change*. Decide tombstone-vs-reuse and the grown-update policy; write both into the format spec. **Harness rows:** the fragmentation row (insert succeeds where contiguous space alone wouldn't allow it), the RID-stability fuzz (1M+ ops, RIDs captured early still resolve correctly late), the grown-update row (your documented policy's observable outcome, whichever you chose, applied consistently).

### Part 3 — The tuple codec 🔨
Encode/decode against a schema: null bitmap, fixed-width columns, varlena columns. **Harness rows:** round-trip fuzz over randomized schemas and values (heavy on NULLs adjacent to varlenas), the all-null and no-null edge rows, max-columns bitmap-boundary rows.

### Part 4 — The harness as a published artifact 🔨
Make every row green, including the spec-derived checker against fuzz-produced pages and the golden-image diffs. Generate and commit the golden hex dumps. Write `RESULTS.md` from `templates/RESULTS.md` with the passing table and one paragraph on the policy decisions (tombstone, grown update) and what they cost later tiers. **This freezes the format. Publish.**

## API contract (what the harness links against)

Code lives under `src/storage/`; the harness includes the public header and links your objects. `Result<T>` is your no-exceptions error type (error enum + value); its exact shape is yours, but every fallible call below returns through it. All multi-byte format fields use the endianness your spec declares.

**Public surface vs. private internals.** Below is everything the harness touches. The header's internal arrangement beyond the spec'd fixed fields, the slot-entry encoding, the compaction algorithm, and free-space tracking are **PRIVATE INTERNALS — deliberately unspecified** here; they are, however, frozen by your format spec + golden images once Part 4 ships. The *page format* is public forever; the *code paths* are yours.

### `chronos::storage::Page`

```cpp
static constexpr size_t kPageSize = 4096;

class Page {  // non-owning view over a caller-provided, kPageSize-byte buffer
  static Page init(std::span<std::byte, kPageSize> buf);    // format an empty page in-place
  static Result<Page> load(std::span<std::byte, kPageSize> buf); // adopt existing bytes, validate header
  Result<SlotId> insert(std::span<const std::byte> tuple);
  Result<std::span<const std::byte>> read(SlotId slot) const;
  Result<void> erase(SlotId slot);
  Result<void> update(SlotId slot, std::span<const std::byte> tuple);
  size_t free_space() const;       // total reclaimable bytes (counts fragmented space)
  uint16_t live_slot_count() const;
  Lsn  page_lsn() const;           // reserved field; reads back what was set
  void set_page_lsn(Lsn lsn);      // nothing calls this until Phase 3.1
};
```

- **What it does:** a codec over one 4 KB buffer. `insert` places tuple bytes and returns a slot id; `read` returns a view of the live bytes; `erase`/`update` follow your documented policies. `insert`/`update` must succeed whenever `free_space()` suffices, compacting internally if needed.
- **Why it exists:** the universal unit of disk-resident storage. Everything chronos persists — heap tuples, B+Tree nodes, catalog rows — is bytes on a page in this format.
- **Side-effect / durability requirement:** none — this phase is memory-only. The requirement is *format*: the buffer's bytes after any operation must be parseable by the spec-derived checker, and slot ids returned by `insert` must remain valid and stable until that slot is erased, across any number of compactions.
- **Critical contract details:** `read`/`erase`/`update` on a dead or out-of-range slot return an error, never UB. A tuple larger than the format's max returns an error from `insert`. `update` that grows beyond available space follows your documented grown-update policy — the harness checks consistency with your spec, not a particular choice. Single-threaded this phase; no locking contract.

### `chronos::storage` tuple codec

```cpp
struct Column { TypeId type; bool nullable; };       // TypeId: at minimum INT32, INT64, VARCHAR
using Schema = std::span<const Column>;
// Value: your tagged union for a typed cell, including NULL

Result<size_t> tuple_encode(Schema s, std::span<const Value> row,
                            std::span<std::byte> out);          // returns bytes written
Result<void>   tuple_decode(Schema s, std::span<const std::byte> in,
                            std::span<Value> out);              // out.size() == s.size()
size_t         tuple_max_size(Schema s, std::span<const Value> row); // exact or safe upper bound
```

- **What it does:** serializes a typed row to the frozen tuple format — null bitmap, then fixed-width columns, then varlena columns, at the offsets your spec defines — and back.
- **Why it exists:** pages store bytes; the executor (Phase 5.3) and catalog (Phase 5.1) speak typed values. This is the boundary, and Phase 5.1's readers will re-derive it from the spec.
- **Side-effect / durability requirement:** `decode(encode(row)) == row` for every schema-valid row; encoded bytes match the golden images for the scripted cases.
- **Critical contract details:** NULL columns contribute zero data bytes. A row that violates the schema (wrong type, NULL in non-nullable column) is an encode error, not a crash. `out` too small is an error with nothing partially written that the caller could mistake for a tuple.

### `chronos::storage::Rid`

```cpp
struct Rid { PageId page_id; SlotId slot; };  // ordered, hashable, trivially copyable
```

- **What it does / why it exists:** the engine-wide stable address of a tuple. This phase only mints the `slot` half, but the type is public now because Phases 2.x/4.2/5.3 store and compare it.

### Spec-derived checker (separate binary)

```
tools/pagecheck <file-of-raw-pages>   → exit 0 iff every page parses per docs/format/page_v1.md
```

Built from the written spec only — it must not include `src/storage/` headers. The harness runs it over fuzz output. (You write both the spec and the checker; the independence is enforced by the include rule, which the harness verifies.)

## Acceptance criteria (phase-level "done")
1. `test/slotted_pages/` — all rows PASS under ASan+UBSan builds: shadow fuzz ≥ 1M operations with zero divergence, RID-stability fuzz green, tuple round-trip fuzz green.
2. `tools/pagecheck` (zero shared headers, verified) parses every fuzz-produced page; golden hex images committed and byte-identical on rerun.
3. `docs/format/page_v1.md` published: every field, offset, and policy (tombstone, grown update, endianness) written down — sufficient for a stranger to re-implement the checker.
4. `RESULTS.md` published; a stranger can rerun everything.

## Principal-engineer traps (no solutions)
- **Compaction may move tuple bytes but must NEVER renumber slots.** RIDs outlive compaction or every future index is wrong. The fuzz row that captures RIDs early and resolves them late exists for exactly this.
- **Reuse a deleted slot, or tombstone it?** The two choices behave differently under Phase 3.3 redo (is replaying an insert onto that slot idempotent?) and Phase 4.2 version chains (does a RID ever mean two different logical rows?). Decide now, document in the spec, be able to defend it.
- **Null-bitmap sizing off-by-one corrupts the first varlena.** The boundary cases — column counts straddling a bitmap byte boundary, NULL immediately before a varlena — are precisely the harness's edge rows.
- **An UPDATE that grows past free space needs a policy today** — fail it? forward-pointer it? Tier IV inherits whatever you choose, so choose deliberately and write down why.
- **`free_space()` lying after compaction-adjacent operations** is the quiet way to fail the fragmentation row: the accounting and the bytes must agree at all times, not just at insert.

## What you hand back for review
1. Implementation + the harness table + `docs/format/page_v1.md` + the committed golden images + `tools/pagecheck`.
2. One sentence per trap above: did it bite you, and how did you resolve it?

Review is principal-engineer style: the interview attack ("walk me through this hex dump"; "why is your tombstone choice safe under redo?"), any overstated claims, the next upgrade. Then Phase 1.2.

# Tier V · Phase 5.1 — Catalog (scaffold)

**Deliverable of this phase:** a self-describing system catalog — `chronos_tables`, `chronos_columns`, `chronos_indexes` stored as ordinary heap files inside your own engine — that survives `kill -9` in the middle of a CREATE TABLE, proven by the crash-torture rig plus an independent low-level page reader.
**What you'll own afterward:** the moment your engine stops being a pile of subsystems and becomes a database: metadata is data, DDL is a transaction, and the storage/WAL/recovery stack you built in Tiers I–III is now load-bearing for the system's own self-knowledge.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**The catalog IS tables, not a manager beside them.** This phase ships three system tables — `chronos_tables`, `chronos_columns`, `chronos_indexes` — whose rows are ordinary tuples in ordinary heap files (Phase 1.2), on ordinary slotted pages (Phase 1.1), written through the ordinary WAL (Phase 3.1), recovered by ordinary ARIES (Phase 3.3). When you run CREATE TABLE, the engine inserts rows into its own heap files inside a logged transaction. That's the entire design.

It is **not** a config file, **not** a JSON/flatbuffer sidecar with its own load/save path, and **not** the in-memory metadata map you built in quackdb L23 — that was a `HashMap<String, Schema>` that evaporated on restart, which was fine for an analytics scratchpad and is disqualifying here. It is also not "a `CatalogManager` class with its own persistence format." The instant you write a serialization routine for catalog state that doesn't go through your heap files and WAL, you've built the wrong artifact — and you've forfeited the payoff, which is that catalog durability comes *for free* from the recovery machinery you already proved in 3.3.

The one genuinely new problem is **bootstrap**: the table that describes tables must describe itself. `chronos_tables` has a schema; that schema lives in `chronos_columns`; finding `chronos_columns` requires reading `chronos_tables`. Every real engine breaks this cycle the same way — a small set of hardcoded facts (here: the page-0 roots and the wired-in schemas of the three system tables), from which everything else is derived by reading.

You're done with the framing when you can say: the deliverable is *three self-describing heap tables plus a bootstrap sequence and a cache*, not *a metadata manager with a persistence feature*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

You'll be the kind of engineer who can reason about self-hosting metadata: where the fixed point of "the system describes itself" has to be hardcoded, what DDL atomicity actually requires (a half-created table must vanish entirely on crash — heap file, catalog rows, index entries, all or nothing), and why schema caches are a coherence problem, not a convenience. When someone asks "what actually happens, page by page and log record by record, when Postgres runs CREATE TABLE?", you answer from having built the same shape.

### Why "from scratch" is the right call here

Use any embedded metadata library, or keep quackdb's in-memory map, and the following go permanently opaque:

- **The bootstrap fixed point.** You'll never confront why `pg_class` has a hardcoded relfilenode and why initdb exists — until you hit the cycle yourself and have to decide exactly how little to hardcode.
- **Crash-atomic DDL.** "CREATE TABLE is just a transaction" sounds trivial until a crash lands between allocating the new table's heap file and committing its catalog rows. Whose job is the orphaned file? You only develop an opinion by owning the failure.
- **Schema-cache coherence.** Every engine caches the catalog; every engine has a story for invalidating it transactionally. Skip the cache and DDL "works" while a concurrent transaction plans against a schema that was never committed.
- **Dogfooding pressure.** The catalog is the first *client* of your engine that isn't a test harness. If your heap-file or transaction API is awkward, the catalog build is where you find out — before 5.3 multiplies the cost.

### What carries forward to later tiers

- **Phase 5.2 → 5.3:** the binder resolves every table and column name in every SQL statement against this catalog; the planner reads `chronos_indexes` to decide index-vs-seq scan. A wrong catalog row becomes a wrong query plan.
- **Phase 6.1:** every server session reads the catalog concurrently; the cache-invalidation contract you set here is what keeps a hundred sessions from each re-scanning catalog heap pages per statement.
- **Phase 6.2:** the torture rig replays DDL-heavy schedules through the fault-injecting `io::` shim; the catalog's crash behavior under torn writes is part of the standing invariant scan.
- **Phase 6.4:** the capstone's consistency checks start by walking the catalog — it is the root of trust for "what should exist."

### What good looks like

- Bootstrap hardcodes the minimum: page-0 roots and the wired-in schemas of the three system tables. Everything else — user tables, their columns, their indexes — is discovered by *reading*, through the same scan path user queries will use.
- CREATE TABLE is one transaction: catalog inserts, file/root allocation, and (for CREATE INDEX) index creation commit or vanish together. There is no window, at any crash point, where the catalog references storage that doesn't exist or vice versa — or if file allocation is non-transactional in your design, orphans are detected and reaped, and you can say which.
- The schema cache is provably a *cache*: you can delete it (or disable it with a flag) and every behavior except speed is identical. Truth lives in the heap pages.
- Uncommitted DDL is invisible: a concurrent transaction's name lookup does not see a table whose CREATE hasn't committed, and a cached entry never outlives the commit/abort that should change it.
- You can answer at a whiteboard, without notes: "walk me through the reads your engine does between process start and serving its first name lookup" and "what happens if the process dies after the catalog insert but before commit?"

### Why this is the shape of the deliverable

The correctness problem here is crash timing against a multi-object mutation (catalog rows + storage allocation + cache), so the deliverable is shaped like Phase 3.3's: a torture loop that kills the process at randomized points inside DDL, plus an *independent* reader that inspects the aftermath without trusting the engine. Clean-exit tests cannot prove DDL atomicity; only a verifier that reads the raw pages after a kill can. The catalog also gets a structural oracle no other component has: it describes itself, so the verifier can cross-check the catalog's self-description against the bytes it actually occupies.

## Exam questions this phase targets (build-proven)

1. Implement a bootstrap sequence such that a cold start with an empty data directory and a cold start with 500 existing tables both reach "first name lookup served" — and you can enumerate every hardcoded fact it relies on.
2. Make CREATE TABLE crash-atomic: across hundreds of randomized `kill -9` points, the post-recovery schema set equals the set of acknowledged DDL — no half-tables, no orphaned catalog rows — with the harness's independent reader as judge.
3. Demonstrate transactional cache coherence: a concurrent reader never resolves a table name whose CREATE has not committed, and never fails to resolve one whose CREATE has.

## Prerequisites — concepts this phase uses

These are vocabulary, not implementation guidance. You should recognize each term and its role; building it is the phase.

### System-catalog vocabulary

- **system catalog** — the set of tables in which a DBMS stores its own metadata: what tables exist, their columns and types, their indexes. In PostgreSQL these are `pg_class`, `pg_attribute`, `pg_index`; the role (not the layout) is what `chronos_tables` / `chronos_columns` / `chronos_indexes` mirror. Reference at vocabulary level: PostgreSQL docs, "System Catalogs" chapter.
- **self-describing / dogfooding** — the property that catalog tables are stored in the same format, through the same engine, as user tables. The catalog has rows *about itself* in its own tables.
- **bootstrap catalog** — the chicken-and-egg breaker: a minimal set of hardcoded facts (fixed root locations, wired-in schemas for the system tables) sufficient to start reading the catalog, after which all further metadata comes from reads. PostgreSQL's analogue is the initdb/genbki bootstrap.
- **table id / OID** — a stable integer identity for a catalog object, independent of its name. Names are for humans and the binder; ids are what other catalog rows and storage structures reference.

### DDL & transactional metadata

- **DDL (Data Definition Language)** — CREATE/DROP TABLE, CREATE INDEX: statements that mutate the catalog rather than user data. "Transactional DDL" means these run as ordinary logged transactions — atomic, recoverable, isolated — which is exactly the property the torture loop checks.
- **schema cache** — an in-memory copy of catalog content kept so the binder doesn't re-scan heap pages per statement. It is a *cache* of the tables, never a second source of truth, and it must be invalidated in step with DDL commit/abort.
- **cache invalidation (transactional)** — the discipline that a schema change becomes visible to other transactions' caches exactly when it commits — not when the DDL statement executes, not when the process feels like it. PostgreSQL's `sinval` traffic plays this role.

### Already yours (built in earlier phases — listed so you wire, not rebuild)

- **heap file / RID** (1.2, 1.1) — catalog rows are ordinary tuples with stable RIDs in ordinary heap files.
- **logged transaction + ARIES recovery** (3.1/3.3) — DDL atomicity is *inherited* from this machinery, not re-implemented beside it.
- **snapshot visibility** (4.2) — uncommitted catalog rows are invisible to other transactions by the same rules as user rows.

## How you know you're aligned (the cross-check)

The harness in `test/catalog/` is your continuous oracle — build a part, run it, watch rows flip SKIP→PASS. It judges only the public surface in the API contract below, plus the bytes on disk; your row encodings for the catalog columns, your cache structure, and your bootstrap internals are invisible to it except as frozen by this contract.

Two independent oracles keep it non-circular:

1. **The low-level reader.** A standalone decoder built from your Phase 1.1 frozen page/tuple format spec — sharing zero code with the engine, in the `waldump` tradition of 3.1. After each torture iteration it opens the data directory cold, walks the catalog heap pages from the hardcoded page-0 roots, and reconstructs the schema set from raw bytes. Your engine never gets to grade its own recovery.
2. **The shadow model.** The torture driver (reusing the 3.3 rig) records every DDL statement the engine *acknowledged as committed* before each `kill -9`. Post-recovery, the reader's reconstructed schema set must equal the shadow set exactly: every acknowledged table present and complete (all columns, all indexes), every unacknowledged table absent in full — no orphan `chronos_columns` rows pointing at a table id that doesn't exist, no `chronos_indexes` row without its index root.

The coherence rows use the engine's public lookup surface from concurrent transactions and compare against the same shadow model. If a row is green, your build has the required observable shape regardless of how you got there.

## The build, in parts (each gated independently by the harness)

### Part 1 — Bootstrap: the catalog describes itself 🔨
Decide the wired-in facts (page-0 roots, the three system-table schemas), then make a cold start read everything else. The acid test the harness applies: the system tables appear *as rows in themselves* — `chronos_tables` contains a row for `chronos_tables` — and after close/reopen, the full self-description round-trips. **Harness rows:** bootstrap-empty-dir, bootstrap-reopen, self-description (the reader decodes the system tables' rows about themselves and checks them against this spec's frozen catalog schemas).

### Part 2 — Transactional DDL 🔨
CREATE TABLE and CREATE INDEX as ordinary logged transactions: catalog inserts plus storage allocation, committing or vanishing atomically through your existing WAL/ARIES path. Then the torture loop: randomized `kill -9` inside DDL, hundreds of iterations, reader-vs-shadow-model comparison after each recovery. **Harness rows:** ddl-commit-visible, ddl-abort-invisible, torture-create-table, torture-create-index, no-orphans (referential closure across the three tables and the storage they name).

### Part 3 — Schema cache with transactional invalidation 🔨
A cache in front of the heap-backed truth, invalidated exactly at commit/abort boundaries. The harness runs interleaved DDL and lookups across concurrent transactions and checks that visibility through the cache equals visibility through a cold uncached read — and that the cache actually caches (a repeated-lookup row bounds catalog page reads). **Harness rows:** cache-coherence-commit, cache-coherence-abort, cache-is-a-cache, TSAN-clean concurrent lookup soak.

### Part 4 — The harness as a published artifact 🔨
Every row green, including the sanitizer builds and a full overnight-scale torture count. Write `RESULTS.md` from `templates/RESULTS.md`: the passing table, torture iteration count, and a short note on where bootstrap forced its hand. Publish so a stranger can rerun.

## API contract (what the harness links against)

Names below are the public surface (namespace/exact spelling adjustable via the harness's shim — semantics are not). **PRIVATE INTERNALS — deliberately unspecified:** the cache's data structure, the bootstrap code path, the catalog rows' byte-level value encoding (it's whatever your 1.1 tuple codec produces for the frozen column lists), and how DDL composes catalog inserts with allocation. One thing this contract *does* freeze, because the independent reader must decode it: the logical schemas (column names, types, order) of the three system tables, which you will write into this spec's companion `catalog_schema.md` before coding and never silently change.

### `Catalog`

```
static Result<Catalog> Catalog::Open(DiskManager&, BufferPool&, TransactionManager&);
```

- **What it does:** runs bootstrap against an existing or empty data directory and returns a catalog ready to serve lookups. On an empty directory it creates and self-registers the three system tables.
- **Why it exists:** the single entry point that turns "a directory of pages" into "a database that knows its own schema" — what initdb-plus-startup is in PostgreSQL.
- **Side-effect requirement:** after `Open`, the system tables exist as heap files, describe themselves in their own rows, and all of that is durable through your normal WAL path. No metadata exists anywhere except in those heap files (plus the wired-in bootstrap facts).
- **Critical contract details:** must succeed identically on first-ever start and on restart after crash (it runs *after* ARIES recovery, never instead of it). Errors via `Result`; no exceptions anywhere.

### DDL

```
Result<TableId> Catalog::CreateTable(Transaction&, const TableDef&);   // name + column defs
Result<IndexId> Catalog::CreateIndex(Transaction&, const IndexDef&);   // table, column(s), unique?
```

- **What it does:** registers a new table (or index) and provisions its storage, all inside the caller's transaction.
- **Why it exists:** this is what a CREATE statement bottoms out in; 5.3's executor calls exactly this.
- **Side-effect requirement:** atomic with the transaction — after commit, lookups (any transaction, cached or cold) see the complete object and its storage exists; after abort or crash, no trace remains visible and no orphaned catalog rows survive recovery. Duplicate name within the visible schema fails cleanly with no side effects.
- **Critical contract details:** uncommitted DDL is invisible to other transactions (standard 4.2 visibility — say explicitly in your design notes which visibility rules catalog reads use). Thread-safe against concurrent lookups.

### Lookup

```
Result<TableSchema>  Catalog::GetTable(Transaction&, std::string_view name);
Result<TableSchema>  Catalog::GetTable(Transaction&, TableId);
Result<std::vector<IndexSchema>> Catalog::GetIndexes(Transaction&, TableId);
```

- **What it does:** resolves a name or id to a full schema — column names/types/order, heap-file identity, index roots — as visible to the calling transaction.
- **Why it exists:** the binder (5.2/5.3) calls this for every name in every statement; the planner calls `GetIndexes` to enumerate plan choices.
- **Side-effect requirement:** none observable; may populate the cache. Results must equal what a cold heap scan of the catalog would return for the same transaction — the cache-coherence rows check exactly this equivalence.
- **Critical contract details:** "not found" is a distinct, non-fatal `Result` error (the binder turns it into a user-facing message, not a crash). Concurrent lookups during DDL are safe and TSAN-clean.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS, including ASan/UBSan builds and a TSAN build of the concurrent-lookup soak.
2. Torture: the 3.3-style kill loop over DDL workloads, hundreds of randomized crash points minimum, reader-vs-shadow comparison green on every iteration — zero unexplained discrepancies.
3. `catalog_schema.md` (the frozen system-table schemas) checked in before implementation began; the independent reader derives from it and the 1.1 format spec only.
4. `RESULTS.md` published; a stranger can rerun everything.

## Principal-engineer traps (no solutions)

- **The bootstrap chicken-and-egg.** Reading `chronos_tables` requires schema that lives in `chronos_columns`, which you find via `chronos_tables`. The escape is hardcoding the page-0 roots and the system-table schemas — and *nothing more*. Each extra hardcoded fact is a second source of truth that can drift from the rows; people bleed by hardcoding "just one more thing" for convenience.
- **Half of "transactional DDL" is the cache.** The heap rows get atomicity for free from Tier III; the in-memory cache does not. A transaction that plans against a schema another transaction hasn't committed — or keeps using a cached entry after the abort that should have killed it — is the bug the coherence rows exist to catch, and it doesn't show up single-threaded.
- **The `CatalogManager` singleton with its own persistence path.** The gravitational pull toward "just serialize the maps to a file at shutdown" is strong and fatal: it bypasses WAL, bypasses recovery, and deletes the entire point of the phase, which is exercising your own engine as its own first customer.
- **Storage allocation vs. catalog rows.** The catalog insert is logged; is creating the table's heap storage? If yes, prove undo handles it; if no, prove recovery reaps orphans. "I didn't decide" is the answer the torture loop punishes.
- **DROP and rename.** Not in scope to build — but decide *now* whether your row formats and id allocation make them possible later, or 6.x will inherit a catalog that can only grow.

## What you hand back for review

1. Implementation + the full harness table + torture iteration count + `catalog_schema.md`.
2. One sentence per trap above: did it bite, and how you resolved it.

Review will be principal-engineer style: the interview attack here is "two sessions run CREATE TABLE t concurrently — walk me through every interleaving," and "your process dies between the heap-file allocation and the catalog insert's commit; who notices, and when?" Then we advance to Phase 5.2.

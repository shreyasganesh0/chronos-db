# Tier VI · Phase 6.1 — Network server (scaffold)

**Deliverable of this phase:** a `chronosd` server binary — io_uring connection loop reusing the Phase 0.3 engine, one session = one transaction context, a resumable message-framing state machine, and a wire protocol real clients can speak — proven by `psql -h localhost` running transactions against chronos and by a chaos-client soak whose post-mortem invariant scan reports zero orphaned transactions and zero leaked locks.
**What you'll own afterward:** the byte boundary between TCP and transactions. When a session dies mid-statement two phases from now under benchmark load, you'll know exactly which state machine was mid-flight, who owns the abort, and why no lock leaked.
**Calibration:** 🔨 (server, framing) · 🧩 (PostgreSQL simple-query protocol subset)
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is the byte boundary between TCP and transactions — not a connection pooler, and not the executor.** You are building the place where two lifetimes meet: a *connection's* lifetime (a socket that can die at any byte) and a *transaction's* lifetime (a txn context that must end in exactly one of COMMIT or ABORT, never neither). The server accepts connections via multishot accept on the 0.3 io_uring engine, reassembles a message stream out of arbitrarily fragmented reads, hands complete statements to the Phase 5.3 executor under the session's txn context, and streams results back without letting a slow client pin unbounded memory.

It is not a pooler — there is no connection multiplexing, no transaction handoff between sockets; one session maps to one connection maps to at most one open txn. It is not the executor — if you catch yourself adding query logic here, stop; statements go *through* this layer, they are not interpreted *by* it. And the framing state machine is not "read until newline": TCP hands you bytes, and your decoder must be resumable at **any** byte boundary, including mid-length-field.

One protocol decision is made in this phase, by you: implement the 🧩 PostgreSQL simple-query subset (StartupMessage, Query, RowDescription/DataRow, CommandComplete, ErrorResponse, ReadyForQuery) so real `psql` is the client — or ship a documented custom protocol and accept that the harness's interop oracle becomes a spec-derived reference client instead of `psql`. Either is acceptable; the choice and its rationale go in `docs/protocol.md` as a decision record.

You're done with the framing when you can say: the deliverable is *a session layer where connection death is a first-class abort path*, not *a socket wrapper in front of the executor*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
You'll be the kind of engineer who can reason about a protocol stall or a stuck session from first principles: which bytes arrived, where the framing state machine paused, which txn context the session holds, who fires the abort when the socket dies, and what backpressure does when the client stops reading. This is the competency behind every "the database hung" incident that turns out to live in the network layer — and behind reading a real Postgres `pg_stat_activity` with understanding rather than superstition.

### Why "from scratch" is the right call here
Put a framework (boost::asio, a gRPC layer, even a thread-per-connection `read()` loop) between TCP and your engine and the following go opaque:

- Why a message that straddles two reads is the normal case, not the edge case — and why hand-rolled protocol code that assumes "one read = one message" works in tests and dies in production.
- Why connection death must abort the open txn *concurrently* with that txn's own statement execution — the first place in chronos where two owners race for the same txn's fate.
- Why a client that stops reading can take down the server unless the result path has bounded buffering and a way to stall the producer.
- Why the 0.3 engine's completion-driven shape (you never block; completions invoke continuations) dictates the session design — a session is a state machine, not a thread.
- Why `psql` connecting is a real interoperability proof and not a vanity feature: a third-party client built from the same public spec is the strongest evidence your wire format is what you think it is.

### What carries forward to later tiers
- **Phase 6.2** kills `chronosd` mid-protocol-exchange; the session-abort path you build now is what makes those crashes recoverable rather than lock-leaking.
- **Phase 6.3** drives all benchmark load through this server — its throughput ceiling and latency tail are *your* accept loop, framing, and outbox under contention.
- **Phase 6.4**'s `demo.sh` opens with `chronosd` + `psql`; the demo's credibility starts at this layer.
- The resumable-decoder discipline is the same shape as Phase 3.1's WAL-tail parsing (a record straddling a segment boundary) — second rep, now under concurrency.
- The chaos-client invariant scan (no orphan txns, no leaked locks) is the session-level echo of Phase 4.1's lock-release discipline and Phase 3.3's abort path; this phase proves they compose.

### What good looks like
- The framing decoder is a pure function of (state, new bytes) → (state, zero or more complete messages). You can feed it one byte at a time and get identical message sequences to feeding it megabyte chunks — the harness literally does this.
- You can answer cold: *"A client sends `BEGIN; INSERT ...` then the socket dies while the INSERT is mid-execution inside the executor — who aborts the txn, on which thread/completion, and what prevents a double-abort?"*
- A client that connects and never reads its results consumes O(configured outbox bound) server memory, not O(result size); the executor stalls rather than the server OOMing.
- `psql` (or the reference client) sees correct transaction status in every ReadyForQuery: idle / in-txn / failed-txn — including after an error mid-transaction.
- After any chaos soak, a quiesced server reports zero active txns and zero held locks, every time.

### Why this is the shape of the deliverable
A server's bugs are timing bugs: they appear only when bytes fragment unkindly, clients die at the worst instant, or results outrun the socket. So the deliverable is a binary plus two adversaries — an interoperability oracle you don't control (real `psql`, or a reference client built from your written spec and nothing else) and a chaos client whose whole job is to disconnect at every phase of the request lifecycle — followed by an invariant scan, because "it didn't crash" says nothing about leaked locks.

## Exam questions this phase targets (build-proven)

1. Implement a message-framing state machine resumable at any byte boundary, such that the harness's byte-dribble row (same stream delivered 1 byte at a time vs. in bulk) produces identical message sequences.
2. Implement session-lifetime/txn-lifetime coupling such that the chaos soak — random disconnects mid-txn, mid-statement, mid-response — ends with the invariant scan reporting zero orphaned txns and zero held locks.
3. Implement enough of a wire protocol that an independent client (real `psql`, or the harness's spec-derived client) can BEGIN, modify data, COMMIT, observe an error, and read correct txn status — without your engine code linked into the client.

## Prerequisites — concepts this phase uses

Vocabulary, not the build. Recognize the role each plays; designing the server is the phase.

### PostgreSQL wire protocol (the frontend/backend protocol)
Reference: https://www.postgresql.org/docs/current/protocol.html (message formats: https://www.postgresql.org/docs/current/protocol-message-formats.html)

- **frontend/backend protocol** — PostgreSQL's client↔server wire format. Almost every message is a 1-byte type tag + a 4-byte big-endian length (which counts itself) + a payload. The *simple query* sub-protocol is the small, stateless-per-statement subset this phase targets; the *extended* protocol (parse/bind/execute, prepared statements) is explicitly out of scope.
- **StartupMessage** — the first message a client sends (no type byte — a famous irregularity): protocol version + key/value parameters (`user`, `database`). The server's reply sequence ends in ReadyForQuery. Authentication can be the trivial `AuthenticationOk` for this phase.
- **Query (`'Q'`)** — carries one SQL string (possibly multiple statements). The whole simple-query protocol is: client sends Query, server sends responses, server sends ReadyForQuery.
- **RowDescription (`'T'`) / DataRow (`'D'`)** — result-set framing: one message describing column names/types, then one message per row. Text format is sufficient for this phase.
- **CommandComplete (`'C'`)** — per-statement completion tag, e.g. `INSERT 0 1`, `SELECT 3`. `psql` parses these; wrong tags show up as confusing client output.
- **ErrorResponse (`'E'`)** — severity/code/message fields. After an error, the server still sends ReadyForQuery — errors are protocol-level responses, not connection killers.
- **ReadyForQuery (`'Z'`)** — the "your turn" message, carrying one status byte: `I` idle, `T` in a transaction, `E` in a *failed* transaction (statements rejected until ROLLBACK). This byte is the protocol-visible projection of your txn state machine — the harness reads it.

### io_uring networking (extends Phase 0.3 vocabulary)
- **multishot accept** — a single accept SQE that keeps producing one CQE per incoming connection until cancelled, instead of being re-armed per accept. See `io_uring_prep_multishot_accept(3)`. Role: the accept loop stops being a loop.
- **buffered / provided-buffer reads** — letting the ring hand completions data in kernel-selected buffers from a pre-registered pool, rather than dedicating one buffer per pending read. Role: thousands of mostly-idle connections without thousands of pinned read buffers. (Whether you use provided buffers or per-session buffers is your call; the term appears in the literature you'll read.)
- **short read / partial read** — `read` returning fewer bytes than asked. On sockets this is the *common* case; your decoder must treat every read as potentially partial.

### session & transaction lifetime
- **session** — the server-side state for one client connection: identity, protocol state, and (here) the at-most-one open txn context. In chronos: one session = one connection = one txn context.
- **orphaned transaction** — a txn whose owning session died without COMMIT/ROLLBACK. Every real engine aborts these; the chaos soak exists to prove yours does, promptly and exactly once.
- **half-close / RST** — the two flavors of peer death: orderly FIN (read returns 0) vs. reset (read/write returns `ECONNRESET`). Both must funnel into the same abort path.
- **backpressure** — bounding the producer when the consumer stalls. Here: a bounded per-session outbox; when it fills, the executor producing rows must stall, not the server allocate.

### the oracle side
- **chaos client** — a harness-owned client that behaves adversarially on purpose: connects, starts work, and disconnects at randomized points (mid-txn, mid-statement, mid-response, mid-StartupMessage). Standard technique for shaking out lifetime races.
- **invariant scan** — a post-soak check, through a documented introspection surface, that the quiesced server holds zero active txns and zero locks. The scan checks *consequences*, not code paths — that's what keeps it non-circular.

## How you know you're aligned (the cross-check)

The harness in `test/network_server/` is your continuous oracle — build a part, run it, watch rows flip SKIP→PASS. It is not circular, three ways:

1. **Interoperability oracle:** real `psql` driven by the harness (scripted via `psql -c` / stdin) — a client you didn't write, built from the public protocol spec. If you chose a custom protocol instead, the harness substitutes a reference client built from *your* `docs/protocol.md` and nothing else (no engine headers); the independence rule is the same as Phase 1.1's `pagecheck`.
2. **Byte-dribble differential:** the harness replays identical captured byte streams against your server in pathological fragmentations (1-byte writes, fragment boundaries inside length fields) and asserts response-stream equality with the bulk delivery. Your decoder's resumability is checked by behavior, not inspection.
3. **Chaos soak + invariant scan:** N seeded chaos clients run concurrently against real SQL workload, disconnecting at randomized lifecycle points; afterward the harness quiesces the server and asserts, via the introspection contract below, zero active txns / zero held locks — then runs a plain `psql` session to prove the server is still fully serviceable.

The harness sees only the public surface: the TCP port, the wire bytes, the introspection output, and the `chronosd` CLI. Threading model, session allocation, outbox structure, how the abort races are resolved — private, yours.

## The build, in parts (each gated independently by the harness)

### Part 1 — The connection loop 🔨
`chronosd` listens, multishot-accepts on the 0.3 engine, creates/destroys sessions, and survives connect/disconnect storms with no fd or memory leaks. No protocol yet — echo or discard is fine internally; the harness checks accept-under-load and clean teardown. **Harness rows:** N-connections accepted, connect/disconnect churn leaves the server serviceable, ASan-clean teardown.

### Part 2 — The framing state machine 🔨
The resumable decoder: bytes in, complete messages out, suspendable at any byte boundary; oversized/garbage frames rejected with a documented error path, never UB. **Harness rows:** byte-dribble equality, fragment-inside-length-field, garbage-frame rejection, max-message-size enforcement.

### Part 3 — The protocol, the session, the executor 🔨
Make the protocol decision; write `docs/protocol.md`. Wire complete messages to the 5.3 executor under the session's txn context; stream results back; map executor `Result<T>` errors to ErrorResponse; emit correct ReadyForQuery status (idle / in-txn / failed-txn). **Harness rows:** `psql` (or reference client) startup handshake, autocommit statement, explicit BEGIN/COMMIT round-trip, error-inside-txn leaves status `E` until ROLLBACK, multi-row SELECT framing.

### Part 4 — Death and backpressure 🔨
Connection death at any point becomes exactly one txn abort with all locks released; the per-session outbox is bounded and a non-reading client stalls its own executor, not the server. **Harness rows:** the chaos soak + invariant scan, the no-read client row (server RSS bounded while a huge result is pinned by a stalled client), double-abort absence under TSAN.

### Part 5 — The harness as a published artifact 🔨
Every row green, including the full seeded chaos soak. Write `RESULTS.md` from `templates/RESULTS.md`: the table, the protocol decision record's one-paragraph summary, and the soak parameters (clients, duration, seed). Publish.

## API contract (what the harness drives)

The harness drives `chronosd` as a black box over TCP plus the introspection surface. Engine-side code lives wherever you choose under `src/`; the only linked surface is the binary's documented behavior. `Result<T>` remains the engine-internal error discipline; on the wire, errors surface as protocol error messages.

**PUBLIC SURFACE** — the items below. **PRIVATE INTERNALS — deliberately unspecified:** threading/completion model, session memory layout, outbox data structure, abort-race resolution mechanism, buffer strategy.

### `chronosd` (server binary)

```
chronosd --port <p> --data-dir <dir> [--max-connections <n>] [--outbox-bytes <n>]
```

- **What it does:** serves the wire protocol on `<p>` against the database in `<dir>` (created/recovered via the Tier III path on startup). Runs until SIGTERM; exits 0 on clean shutdown.
- **Why it exists:** the single process the rest of Tier VI (torture, benchmarks, demo) operates on.
- **Side-effect requirement:** on SIGTERM, open txns are aborted, locks released, WAL in a state from which restart recovers cleanly. On SIGKILL (Phase 6.2's job), no requirement beyond what ARIES already guarantees.
- **Critical contract details:** `--max-connections` exceeded ⇒ documented refusal (error then close), not a hang. `--outbox-bytes` is the per-session result-buffer bound the backpressure row checks. Flag names may differ; document them in `--help` — the harness shells out to `--help` to discover spellings is *not* supported, so keep these exact spellings or update the harness contract with the spec author.

### Wire protocol

```
docs/protocol.md   — the written contract (PostgreSQL simple-query subset, or your custom protocol)
```

- **What it does:** defines every message the server sends/accepts, byte-exactly: framing, the startup sequence, statement execution, result streaming, errors, txn status reporting.
- **Why it exists:** the interop oracle is built from this document alone. If the doc and the server disagree, that's a failing row — by design, same as Phase 1.1's format spec.
- **Side-effect requirement:** after any error the connection remains protocol-usable (error → ready-for-query, not connection drop), unless the error class is documented as fatal.
- **Critical contract details:** if PostgreSQL subset: message set is at minimum StartupMessage, Query, RowDescription, DataRow, CommandComplete, ErrorResponse, ReadyForQuery, with correct length framing and ReadyForQuery status bytes `I`/`T`/`E`. If custom: the doc must be sufficient for a stranger to write the client — because the harness's author literally will.

### Introspection surface (for the invariant scan)

```
chronosd-stat <port-or-socket>   → prints: active_sessions=<n> active_txns=<n> held_locks=<n>
   (mechanism is yours: an admin SQL command, an admin socket, or SIGUSR1 dump — but the
    three counters and a machine-parseable one-line format are the contract)
```

- **What it does:** reports, at quiesce, the server's live session/txn/lock counts from the *engine's own tables* (4.1's lock table, the txn manager) — not from session-layer bookkeeping.
- **Why it exists:** "no orphaned txns, no leaked locks" must be observable to the harness without engine headers. Counting from the lock table rather than the session layer is what makes the scan catch session-layer lies.
- **Side-effect requirement:** none; read-only.
- **Critical contract details:** must be callable while the server runs. After the chaos soak quiesces, all three counters must read the harness-expected values (0 txns, 0 locks, 0 sessions once clients are gone).

## Acceptance criteria (phase-level "done")

1. `test/network_server/` — all rows PASS; Parts 1–4 rows additionally green under ASan+UBSan builds; the chaos soak and double-abort rows green under TSAN.
2. Interop proof: a scripted `psql` session (or the spec-derived client) runs BEGIN → writes → COMMIT → error-handling round-trip, output captured in RESULTS.md.
3. Chaos soak: ≥ 30 minutes seeded, randomized disconnect points covering startup/mid-txn/mid-statement/mid-response; invariant scan zero/zero/zero; server serviceable after.
4. `docs/protocol.md` published (decision record included); `RESULTS.md` published; a stranger can rerun everything.

## Principal-engineer traps (no solutions)

- **TCP gives you bytes, not messages.** Partial reads across message boundaries — including *inside* the 4-byte length field — are where hand-rolled protocol code dies. The byte-dribble row exists because "works with curl-sized writes" proves nothing.
- **Connection death is an abort path that fires concurrently with the txn's own statement execution.** This is the first real session-lifetime race in chronos: the executor is mid-INSERT on behalf of a txn whose owner just vanished. Decide who owns the abort, and what makes it happen exactly once, before you write either path.
- **Backpressure or OOM — there is no third option.** A client that stops reading must not pin unbounded result buffers. Bound the per-session outbox and stall the executor; "the kernel socket buffer will handle it" is not an answer for a multi-megabyte SELECT.
- **ReadyForQuery's status byte is a contract, not decoration.** `psql` changes behavior on `I`/`T`/`E`; an optimistic `I` after an error inside a txn will make the interop rows fail in ways that look like client bugs.
- **Multishot accept can stop being multishot.** The CQE flags tell you when the kernel disarmed it; miss that and the server silently stops accepting under load — exactly when 6.3 needs it most.

## What you hand back for review

1. Implementation + the harness table + `docs/protocol.md` + the chaos-soak parameters and invariant-scan output in RESULTS.md.
2. One sentence per trap above: did it bite you, and how did you resolve it?

Review is principal-engineer style: the interview attack ("socket dies while the executor holds a page latch on behalf of this session — walk me through the next 10 milliseconds"), any overstated claims, the next upgrade. Then Phase 6.2.

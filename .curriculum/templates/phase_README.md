# Tier _ · Phase _._ — <NAME> (scaffold)

<!-- The scaffold every phase spec in phase_specs/ follows. Section order is rigid.
     Delete these comments when filled. -->

**Deliverable of this phase:** <the one externally verifiable artifact this phase ships>
**What you'll own afterward:** <one-sentence capability unlock; expanded in "Why this phase exists" below>
**Calibration:** 🔨 / 🧩 / 📖
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

<!-- REQUIRED. The FIRST framing the owner reads. State plainly what category this
     component is (a durability primitive? a cache? an index? a recovery algorithm?
     a benchmark rig?) and explicitly what it is NOT — naming the adjacent thing
     the owner is most likely to confuse it with and build by mistake (e.g. "a
     write-ahead LOG, not a recovery system" / "a buffer pool, NOT a generic LRU
     cache"). Close with the one-sentence "right artifact" test: "you're done with
     the framing when you can say the deliverable is X, not Y." Never a one-liner. -->

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
<the underlying engineering capability — phrased as "you'll be the kind of engineer who can debug/design/reason about X". One paragraph.>

### Why "from scratch" is the right call here
<what specifically goes opaque if you use the library/DBMS version. Name the later debugging / design moments that depend on having built this yourself. 3-5 concrete future failure modes.>

### What carries forward to later tiers
<bullets, each naming a future phase and the specific thing from this phase that recurs there. Use real cross-references (Phase 2.2, Phase 3.1, etc.). This is where the curriculum's order is justified.>

### What good looks like
<bullets describing observable properties of a well-built solution WITHOUT giving the implementation: surface area, interview-attack answers you can give without running it, performance/correctness floors, invariants that hold at quiesce. 4-6 bullets.>

### Why this is the shape of the deliverable
<one paragraph: why the artifact is what it is (a library + torture rig? a standalone tool? a results table?). Connect the deliverable shape to the nature of the correctness problem — silent corruption, concurrency, crash timing.>

## Exam questions this phase targets (build-proven)
1. <build-proven question — "implement X such that harness row Y PASSes" or "measure Z and report it">
2. ...

## Prerequisites — concepts this phase uses

<!-- Define every vocabulary concept the spec, contract, or harness assumes.
     Audience: a strong C/C++ systems programmer who has never built a database.
     They should recognize each term and know what role it plays — NOT how to
     implement it. Implementing it is the build. Group by sub-area; link to
     authoritative docs/papers. -->

### <sub-area>
- **`<term>`** — <1-2 sentence definition: what role does it play? Where would a fresh systems dev encounter it?>

## How you know you're aligned (the cross-check)

<!-- REQUIRED. Foreground the validated harness as THE continuous alignment oracle,
     so the owner never builds blind and checks at the end. Build a part, run the
     suite, a row flipping SKIP→PASS is PROOF the build matches the expected
     observable shape — however it was implemented. Say explicitly that the harness
     inspects only the public surface, not the private internals. Name the
     independent oracle (shadow model / differential reference / spec-derived
     decoder / SIGKILL torture verifier / SQLite / TPC-C conditions) so the owner
     trusts it isn't circular. -->

## The build, in parts (each gated independently by the harness)
### Part 1 — <name> 🔨
<the central idea; what the harness rows check>

### Part N — The harness as a published artifact
<make every row green; write RESULTS.md from templates/RESULTS.md; publish>

## API contract (what the harness imports)

<!-- For EVERY type/function the harness links against, give all of:
     - signature (types, ownership, alignment requirements)
     - **What it computes / does** — the observable behavior (the spec)
     - **Why it exists** — the role it plays in real database engines
     - **Side-effect / durability requirement** — what must be true afterward
       (what's on disk, what's latched, what's visible), NOT how
     - Non-obvious contract details (error modes via Result<T>, thread-safety
       class, blocking behavior, alignment).
     A bare signature is not a spec. Mark PUBLIC SURFACE vs PRIVATE INTERNALS
     clearly: internals (node layout, latch placement, encoding details unless
     frozen by golden images) are deliberately unspecified. -->

### <component name>

```
<signature>
```

- **What it does:** <observable behavior>
- **Why it exists:** <role in real systems / what it unlocks>
- **Side-effect / durability requirement:** <what must hold afterward; NOT how>
- **Critical contract details:** <error modes, thread-safety, blocking, alignment>

## Acceptance criteria (phase-level "done")
1. Harness: all rows PASS <including which sanitizer builds must be green>.
2. <phase-specific gates: torture iterations, TSAN, benchmark table>
3. RESULTS.md published; stranger can rerun.

## Principal-engineer traps (no solutions)
- <the place people bleed #1>
- <#2>

## What you hand back for review
1. Implementation + harness table <+ benchmark numbers where the phase ships them>
2. One sentence per trap: did it bite, how resolved

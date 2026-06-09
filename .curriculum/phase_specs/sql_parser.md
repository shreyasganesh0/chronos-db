# Tier V · Phase 5.2 — SQL lexer & parser (scaffold)

**Deliverable of this phase:** a hand-rolled lexer + recursive-descent/Pratt parser for chronos's frozen OLTP grammar, with `Result<T>` error reporting carrying byte positions — proven by golden AST dumps, a parse/pretty fixpoint, a grammar-derived random generator, and a must-fail corpus.
**What you'll own afterward:** the text→AST seam of the engine, built under `-fno-exceptions` discipline — the proof that you can write a parser whose every error path is a value, not a `throw`, so that bad input can never abort the server you build in 6.1.
**Calibration:** 🔨 — but a **second rep**. You built a Pratt parser and a lexer in quackdb (L20–21). Move fast: the parsing is review; the new content is the error-propagation discipline and the frozen-grammar scope control. If this phase takes more than a small fraction of the time 5.1 did, you're gold-plating.
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is a text-to-AST translator, nothing else.** Input: a SQL string. Output: a structured tree, or a positioned error. It performs no name resolution (it doesn't know whether table `users` exists — that's the 5.3 binder against the 5.1 catalog), no type checking, no planning, no execution. `SELECT a FROM no_such_table` parses *successfully* here.

It is **not** a SQL-standard implementation. The grammar is the small OLTP subset and it is **frozen in a checked-in grammar spec before the first line of parser code**: CREATE TABLE, CREATE INDEX, INSERT, SELECT (WHERE, one simple two-table join, ORDER BY, LIMIT), UPDATE, DELETE, BEGIN/COMMIT/ROLLBACK, SET TRANSACTION ISOLATION LEVEL. No subqueries, no GROUP BY, no expressions-in-ORDER-BY, no string-escape exotica beyond what the grammar file says. The frozen file is a scope-creep firewall: every "while I'm in here, I'll just add…" steals days from Tier VI, where the actually-new material lives.

And it is **not** quackdb's parser with the panics left in. quackdb could `panic!` on a malformed token because a crashed analytics REPL costs nothing. chronos's parser feeds a server (6.1) where one malformed packet calling `assert` or `abort()` is a denial of service. The real lesson of the rep is threading `Result<T>` through every production without exceptions and without losing the error's byte position.

You're done with the framing when you can say: the deliverable is *a frozen grammar plus a total function from bytes to AST-or-positioned-error*, not *a SQL frontend*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building

You'll be the kind of engineer who can make deep recursive code total under `-fno-exceptions`: every production returns `Result`, every error propagates with its source position intact, and no input — adversarial, truncated, or fuzzed — can reach an abort. That discipline, not operator precedence, is what this rep adds over quackdb.

### Why "from scratch" is the right call here

It mostly already was — in quackdb. The marginal from-scratch value this time:

- **Error totality.** A generated parser or a copied one hides where the panic-shaped escape hatches are. You only trust "no input aborts the server" if you wrote every early-return yourself.
- **Position plumbing.** The 6.1 server and any future client need errors like `syntax error at byte 27: expected ')'`. Getting positions right through nested productions is fiddly value-threading you have to own.
- **The frozen-grammar muscle.** Practicing "spec first, freeze, then implement exactly that" on a component where you're fast is rehearsal for components where you're not.

### What carries forward to later tiers

- **Phase 5.3:** the binder and planner consume this AST directly; the statement shapes you define here are 5.3's input contract.
- **Phase 6.1:** the server calls `Parse` on every untrusted byte string a client sends. The no-abort guarantee proven here is a standing security property there.
- **Phase 6.2 / 6.3:** the torture rig and benchmark load generators produce statement streams through this same grammar; the random generator the harness ships here gets reused as their statement source.
- **`Result<T>` discipline everywhere:** the parser is the deepest call tree in the codebase — if the error-propagation pattern works here, it works anywhere in the engine.

### What good looks like

- The grammar file exists, was committed before parser code, and the diff history proves it.
- Every production returns `Result<…>`; `grep` finds no `throw`, no `assert(` on the input path, no `abort()`. Malformed input of any kind returns an error value with a sane byte offset.
- Precedence and associativity are table-driven or otherwise centralized — one place to read, one place to be wrong — and `a - b - c`, `NOT a = b`, `a OR b AND c` all dump the tree the grammar file says they should.
- The lexer and parser are separable: you can dump a token stream without parsing, which is your first debugging tool when a golden AST row goes red.
- You can answer cold: "what does your parser do with a 10MB statement of nested parens?" (you know your recursion-depth story) and "where does the byte offset in a deeply nested error come from?"

### Why this is the shape of the deliverable

Parser bugs are disproportionately in inputs you didn't think to write: precedence interactions, boundary tokens, near-miss keywords. So the oracle is generative and structural rather than a hand-picked list — golden dumps anchor known shapes, the fixpoint property checks self-consistency on *every* input, the grammar-derived generator covers the space mechanically, and the must-fail corpus pins the error surface (including positions) so error handling is a tested contract, not a vibe.

## Exam questions this phase targets (build-proven)

1. Implement the frozen grammar such that every statement emitted by the harness's grammar-derived random generator parses, and `parse(pretty(parse(x))) == parse(x)` holds across the whole generated corpus.
2. Make the parser total under `-fno-exceptions`: every entry in the must-fail corpus returns the asserted error byte position, and no input in a fuzz run reaches an abort — proven under ASan/UBSan.
3. Get `a - b - c` and `NOT a = b` right, and defend the precedence table at a whiteboard.

## Prerequisites — concepts this phase uses

Vocabulary only; you've built most of this once. Definitions are role-level, not recipes.

### Parsing vocabulary

- **lexer / tokenizer** — the pass that turns bytes into tokens (keyword, identifier, number, string, operator), each carrying its source byte range. Keeps the parser free of character-level concerns.
- **recursive-descent parser** — a parser where each grammar rule is a function that consumes tokens and returns a node; the call stack mirrors the parse tree. The standard hand-rolled technique.
- **Pratt / precedence-climbing parsing** — the recursive-descent extension for expressions, where binding powers attached to operators replace one-function-per-precedence-level. You built one in quackdb L20.
- **AST (abstract syntax tree)** — the structured output: statement nodes containing expression nodes, free of surface syntax like parens and whitespace. Its in-memory layout is yours (see PRIVATE INTERNALS).
- **grammar specification (EBNF-style)** — the rule file naming every production, token, precedence level, and associativity. Here it's also the contract the random generator is derived from — which is why it, not the parser, is the source of truth.

### Testing vocabulary

- **golden file testing** — checked-in expected outputs (here: canonical AST dumps) diffed against actual output; any change to parse shape is a visible diff, intentional or not.
- **fixpoint / round-trip property** — `parse(pretty(parse(x))) == parse(x)`: pretty-printing a tree and re-parsing must reproduce the same tree. Catches precedence and associativity bugs mechanically, because a mis-shaped tree pretty-prints to differently-parenthesized text.
- **grammar-derived generation** — random statement synthesis by walking the grammar rules. Derived from the grammar *file*, never from the parser code — otherwise the test inherits the parser's bugs and the oracle is circular.
- **must-fail corpus** — inputs that are required to be rejected, each with the asserted error position. Tests that error handling is precise, not merely present.

## How you know you're aligned (the cross-check)

The harness in `test/sql_parser/` is the continuous oracle: build the lexer, rows flip; build statements one production at a time, more rows flip. It sees only the public surface below — `Parse`, the dump, the pretty-printer, the error type. Node layout, token representation, and parsing technique are invisible to it.

Independence comes from the grammar file: the harness's random generator is written against `grammar.md`'s rules, sharing nothing with your parser, so agreement between generator and parser is two artifacts independently implementing one spec — not the parser grading itself. The golden dumps are reviewed by eye once against the grammar, then frozen. The fixpoint property needs no external truth at all; it's self-consistency, and it is vicious about precedence.

## The build, in parts (each gated independently by the harness)

### Part 1 — Freeze the grammar 🔨
Write `grammar.md`: every production, token class, keyword, precedence level, associativity, and the statement list — exactly the scope in the framing section, nothing more. Commit it before parser code exists. The harness generator and must-fail corpus are built from this file, so it gates everything. **Harness rows:** grammar-file-present (checks the file is committed and covers the required statement list).

### Part 2 — Lexer 🔨
Bytes → tokens with byte ranges; unterminated strings and stray bytes are positioned errors, not aborts. **Harness rows:** token-stream goldens, lexer must-fail entries with positions.

### Part 3 — Statements & expressions 🔨
The full frozen grammar, every production returning `Result`. Expressions via Pratt with the precedence table from `grammar.md`. Go fast — this is the quackdb rep. **Harness rows:** golden AST dumps per statement class; the precedence battery (`a - b - c`, `NOT a = b`, `a OR b AND c`, comparison chains); parser must-fail entries with asserted byte positions.

### Part 4 — Pretty-printer + the generative gauntlet 🔨
`Pretty` emits valid SQL from any AST; then the fixpoint property and the generator corpus run at scale (thousands of seeded statements), plus a garbage-bytes fuzz pass that must produce only error values — never a crash — under ASan/UBSan. **Harness rows:** fixpoint-corpus, generator-corpus, fuzz-no-abort.

### Part 5 — The harness as a published artifact 🔨
All rows green; `RESULTS.md` from `templates/RESULTS.md` with the table, corpus sizes/seeds, and one paragraph on what the no-exceptions discipline changed versus your quackdb parser. Publish.

## API contract (what the harness links against)

**PRIVATE INTERNALS — deliberately unspecified:** AST node layout (variant, class hierarchy sans RTTI, arena-allocated — your call, and note 0.2's arenas fit statement lifetime), token representation, the parsing technique itself. The harness compares *dump strings*, never node structure. The dump format is yours but becomes frozen the moment goldens are checked in: deterministic, one canonical rendering per tree, fully parenthesized so precedence is visible.

### `Parse`

```
Result<Statement> Parse(std::string_view sql);
```

- **What it does:** parses exactly one statement into an AST, or returns a `ParseError`. Total: any byte sequence yields one of the two — never UB, never an abort.
- **Why it exists:** the single text→AST seam; 5.3 binds its output, 6.1 calls it on every client message.
- **Side-effect requirement:** none. No global state, no I/O; safe to call from concurrent sessions on different inputs.
- **Critical contract details:** errors carry byte offset + message (see `ParseError`). Pathological nesting must hit a *graceful* depth limit (an error value), not a stack overflow — your recursion-depth strategy is a design decision the review will probe.

### `ParseError`

```
struct ParseError { size_t byte_offset; /* + your message representation */ };
```

- **What it does:** locates the failure as a byte offset into the original input, with a human-readable message.
- **Why it exists:** positioned errors are the difference between a usable frontend and `ERROR: syntax error`; the must-fail corpus asserts the offsets.
- **Critical contract details:** the offset points into the *original* input bytes (what a client/editor needs to underline). Exact message text is yours; the harness asserts offsets and error-class, not prose.

### `DumpAst` / `Pretty`

```
std::string DumpAst(const Statement&);   // canonical structural dump — golden-file format
std::string Pretty(const Statement&);    // valid SQL that re-parses to the same tree
```

- **What they do:** `DumpAst` renders the tree deterministically for golden comparison and for the fixpoint equality check; `Pretty` emits parseable SQL for the round-trip.
- **Why they exist:** they are the only window the harness has into your trees — they *are* the observable surface of the AST.
- **Side-effect requirement:** pure; identical trees produce identical strings.
- **Critical contract details:** the fixpoint is checked as `DumpAst(parse(Pretty(t))) == DumpAst(t)`. `Pretty` need not preserve the user's whitespace or paren style — only the tree.

## Acceptance criteria (phase-level "done")

1. Harness: all rows PASS — goldens, precedence battery, must-fail positions, fixpoint corpus, generator corpus, fuzz-no-abort — under ASan/UBSan builds.
2. `grammar.md` committed before implementation (history shows it) and unchanged since, or every change has a one-line justification in the file.
3. No `throw` / input-path `assert` / `abort()` anywhere in the lexer or parser; builds clean under `-fno-exceptions -fno-rtti -Wall -Wextra -Werror`.
4. `RESULTS.md` published with seeds and corpus sizes; a stranger can rerun.

## Principal-engineer traps (no solutions)

- **The hidden abort.** Without exceptions, every production hand-threads `Result` — and the temptation to `assert` "impossible" token states is constant. Each one is a server-killing input away from being possible. This phase is an error-propagation exercise wearing a parser costume; treat every assert on the input path as a bug.
- **Precedence and associativity bugs hide.** Hand-written tests check the cases you thought of; `a - b - c` and `NOT a = b` break the ones you didn't. The fixpoint + generator combination exists because it catches tree-shape errors mechanically — don't skip running them at full corpus size.
- **Scope creep through the grammar.** Subqueries, GROUP BY, `CASE`, multi-way joins all feel cheap mid-flow and each one taxes the binder, planner, and executor in 5.3 and beyond. The grammar file is frozen; additions go to a future-work list, not the parser.
- **Position drift.** It's easy to report the position of the token *after* the problem, or of the production's start. Decide the convention once, write it in `grammar.md`, and make the must-fail corpus enforce it.
- **Pretty-printer half-effort.** If `Pretty` can emit text your own parser rejects (quoting, keyword-identifier collisions), the fixpoint rows red unrelated to parsing. Budget an hour for it; it's load-bearing test infrastructure, and 6.3's load generator will reuse it.

## What you hand back for review

1. Implementation + `grammar.md` + the full harness table with corpus sizes and seeds.
2. One sentence per trap: did it bite, how resolved.

Review attack: "show me the path a malformed byte at offset 27 takes from the lexer to the error the server would write," and "defend `NOT a = b` from your precedence table without running it." Fast review expected — then Phase 5.3, where the actually-new Tier V material lives.

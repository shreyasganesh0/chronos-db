# Tier VI · Phase 6.3 — Benchmarks: YCSB-style & TPC-C-lite (scaffold)

**Deliverable of this phase:** `chronos-bench` — a coordinated-omission-safe load generator running YCSB-style workloads A–F and TPC-C-lite (NewOrder, Payment, OrderStatus over scaled warehouses) against chronos through the 6.1 server, with hand-rolled HdrHistogram-style latency capture — and a published `RESULTS.md` carrying tpmC-style numbers, p50/p99 latency distributions, ablation curves (group-commit window, buffer-pool size, isolation level, lock-vs-MVCC contention), a SQLite-WAL-mode baseline column, and TPC-C consistency conditions green after **every** run.
**What you'll own afterward:** a measurement instrument you trust — the ability to state chronos's performance with error bars and durability config attached, and to detect a correctness regression *because the benchmark's consistency checks went red*, not because a user complained.
**Calibration:** 🔨
**Ground rule:** spec + validated harness only. No solution code. You design and write every line. The harness judges only the observable surface; the internals are yours.

## What this is — and is not

**This is an instrument, not marketing numbers — and the benchmark doubles as a correctness oracle under real contention.** That second clause is the headline: TPC-C was designed with built-in consistency conditions (W_YTD must equal the sum of its districts' D_YTD; order counts must reconcile with NEW-ORDER rows) precisely so that a "fast" run that corrupted data is detectable. Your TPC-C-lite inherits that design — the consistency battery runs after every benchmark run, making 6.3 the highest-contention correctness test chronos has ever faced. Many engines' worst concurrency bugs are first seen as a benchmark's consistency check going red.

It is not a marketing exercise: a number without its durability configuration, warm-up discipline, seed, and variance is not a measurement. It is also not the torture rig — faults are 6.2's job; here the storage is honest and the adversary is *contention*. And it is not full TPC-C: three of the five transactions, no think times unless you choose them, no audit-grade scaling rules — "tpmC-style," clearly labeled as such, never "tpmC."

You're done with the framing when you can say: the deliverable is *a reproducible instrument whose every number carries its conditions and whose every run re-proves consistency*, not *a big throughput number*.

## Why this phase exists (goals, rationale, what carries forward)

### The skill you're building
You'll be the kind of engineer who can audit a benchmark claim in someone else's README in thirty seconds — *closed-loop or open? fsync on? warm or cold? how many runs? where's p99.9?* — because you've personally built a load generator that doesn't lie, and you know each specific way one can. Performance claims are the most-bluffed area in databases; this phase makes yours auditable.

### Why "from scratch" is the right call here
Use an off-the-shelf benchmark harness and the following stay opaque:

- Why a naive closed-loop client under-reports tail latency by orders of magnitude — coordinated omission is invisible until you've built the intended-start-time scheduler that fixes it.
- Why histogram *design* (bucket resolution, range, memory) determines whether p99.9 is a measurement or an artifact — and why recording latencies into a `std::vector` then sorting changes the experiment by allocating in the hot path.
- Why TPC-C's consistency conditions are checkable at all — wiring them yourself means you know exactly which invariant each transaction maintains, which is the same knowledge a concurrency-bug postmortem needs.
- Why ablations (group-commit window, pool size, isolation level) need the load held constant to mean anything — off-the-shelf rigs make varying the *system* easy and controlling the *experiment* hard.
- Why a SQLite baseline keeps you honest: an absolute number has no context; "vs. the best-engineered embedded engine on Earth, on the same hardware, same workload, same durability setting" does.

### What carries forward to later tiers
- **Phase 6.4**'s demo runs this phase's TPC-C-lite load and consistency battery as the live proof; `demo.sh`'s "benchmark resumes after recovery" step is `chronos-bench` re-attached.
- The ablation curves become ARCHITECTURE.md's quantitative spine in 6.4 — the group-commit-window curve closes the loop opened by Phase 3.1's knob, the pool-size curve by Phase 1.3's hit-rate table, the recovery-time discussion by Phase 3.2's checkpoint-interval study.
- The lock-vs-MVCC contention ablation is the empirical payoff of building both Tier IV paths — the comparison interviews always ask for, answered with your own data.
- The consistency battery joins 6.2's oracles in the capstone's composed pipeline: torture rig + benchmark workload + server, one verdict.

### What good looks like
- Every results row carries: workload, seed, client count, target rate, durability config (fsync on/off, group-commit window), warm-up discarded, run count, mean ± variance. A row missing any of these is an invalid row by your own tooling's rules.
- The load generator schedules by *intended start time*: when the system stalls, queued requests' latencies include their wait. You can state the p99 difference this makes on a stalled run from your own data.
- TPC-C consistency conditions green after every run at every isolation level you claim to support — and you can name which condition which transaction threatens.
- chronos beats SQLite-WAL on concurrent writers (the architectural bet of the whole project: real concurrency vs. a single-writer engine), and every case where it doesn't is EXPLAINed in RESULTS.md, not omitted.
- Ablation curves have ≥ 3 runs per point with variance shown; a crossed pair of curves prompts an explanation, not a re-run until it looks right.

### Why this is the shape of the deliverable
Performance claims rot instantly unless they're reproducible, and they mislead unless tail-honest and durability-honest. So the artifact is a *tool plus a results document with methodology*, not a number: seeded workloads, recorded configs, multiple runs, variance, a baseline column, and a consistency battery that makes every performance run double as a correctness run. The shape *is* the honesty.

## Exam questions this phase targets (build-proven)

1. Implement a coordinated-omission-safe load generator (intended-start-time scheduling) and demonstrate, on a deliberately stalled run, the p99 gap between CO-safe and naive measurement — both numbers in RESULTS.md.
2. Implement TPC-C-lite (NewOrder, Payment, OrderStatus; scaled warehouses) such that the harness's consistency battery — the TPC-C-derived assertions over W_YTD/D_YTD, order counts, and NEW-ORDER reconciliation — passes after every run at every supported isolation level.
3. Produce the four ablation curves (group-commit window, buffer-pool size, isolation level, lock-vs-MVCC) with ≥ 3 samples per point and variance, plus the SQLite-WAL baseline column, in a RESULTS.md a stranger can regenerate.

## Prerequisites — concepts this phase uses

Vocabulary, not the build. Recognize the role each plays; building the rig is the phase.

### workloads
- **YCSB (Yahoo! Cloud Serving Benchmark)** — the standard key-value-style workload family (Cooper et al., SoCC 2010: "Benchmarking Cloud Serving Systems with YCSB"). Workloads A–F are named mixes: A update-heavy (50/50 read/update), B read-mostly (95/5), C read-only, D read-latest, E short scans, F read-modify-write. Here they run as SQL against a chronos table — "YCSB-style," exercising point-access and scan paths under varying write pressure.
- **Zipfian distribution** — the skewed key-popularity distribution YCSB specifies; a few hot keys absorb most traffic. Role: it's what makes A/B/F *contention* tests rather than uniform-random I/O tests.
- **TPC-C** — the canonical OLTP benchmark (spec: tpc.org/tpcc): a wholesale supplier with warehouses, districts, customers, orders. Transactions are short, write-heavy, and contended (every NewOrder and Payment updates its district/warehouse rows). "Lite" here: NewOrder, Payment, OrderStatus only.
- **warehouse scaling** — TPC-C's load knob: data size *and* contention domain scale with the warehouse count W; cross-warehouse touches (a small fraction of NewOrder lines, per the spec's mix) create the cross-domain conflicts. Reporting W with every number is part of the methodology.
- **tpmC** — TPC-C's metric: NewOrder transactions completed per minute. Audited tpmC has strict mix/think-time/response-time rules; yours is "tpmC-style" — same numerator, simplified rules, labeled as such.
- **TPC-C consistency conditions** — the spec's consistency requirements (clause 3.3.2): assertions over the data that any correct execution preserves, e.g. each warehouse's W_YTD equals the sum of its districts' D_YTD; each district's D_NEXT_O_ID − 1 equals its max order id; NEW-ORDER row counts reconcile with order ranges. Role: a benchmark-integrated correctness oracle that needs no execution trace — only the final state.

### measurement
- **coordinated omission** — Gil Tene's term ("How NOT to Measure Latency") for the systematic error where a closed-loop client, blocked waiting on a slow response, *doesn't send* the requests it should have — so the slowest periods generate the fewest samples and p99 collapses into fiction.
- **intended start time / open-loop scheduling** — the fix: each request has a schedule-derived intended start; latency is measured from *intended* start, so a stalled system accrues the queueing delay it actually caused.
- **HdrHistogram** — Tene's high-dynamic-range histogram: logarithmic buckets with bounded relative error, constant memory, no hot-path allocation, mergeable across threads. You hand-roll the equivalent; the role to copy is *recording cost independent of value, quantiles accurate to a stated error*.
- **p50/p99/p99.9 (percentiles)** — distribution summaries; the tail percentiles are where databases earn or lose their keep. Means hide everything interesting.
- **closed vs. open loop** — closed: N clients, each waits for its response before sending the next (throughput self-throttles). Open: arrivals on a clock regardless of completions (backlogs form). Knowing which you ran, and saying so, is methodology.
- **warm-up** — the period before steady state (cold buffer pool, unsplit B+Tree hot paths, empty WAL segments). Measured runs discard it — and say how much was discarded.
- **ablation** — varying exactly one factor (group-commit window, pool size, isolation level) across runs with everything else fixed, to attribute effect to cause. One sample per point is an anecdote, not a curve.

### the baseline
- **SQLite WAL mode** — SQLite with `journal_mode=WAL`: readers don't block the writer, but there is *one* writer at a time. Role: the honesty baseline — superbly engineered, architecturally single-writer. chronos's bet is concurrent writers; this column is where the bet pays off or gets explained.

## How you know you're aligned (the cross-check)

The harness in `test/benchmarks/` is your continuous oracle — build a part, run it, rows flip SKIP→PASS. Its independence comes from three directions:

1. **TPC-C consistency conditions** — assertions taken from the TPC-C spec, evaluated by the harness *via plain SQL queries through the server* (it never links engine code). They were defined by the TPC two decades before chronos existed; nothing about them depends on your implementation.
2. **A reference histogram and CO-check** — the harness replays recorded (intended-start, completion) timestamp logs your generator must emit, recomputes quantiles exactly (full sort, offline), and compares to your histogram's claims within your stated error bound; it also detects coordinated omission by recomputing latency-from-intended-start independently. Your instrument is calibrated against arithmetic, not against itself.
3. **SQLite as the baseline runner** — the harness runs the same seeded workload against SQLite-WAL with the same generator, so the baseline column is produced by the *same instrument*, removing rig bias from the comparison.

The harness drives only the `chronos-bench` CLI, its emitted logs, and SQL through the server; histogram internals, generator threading, and workload plumbing are private.

## The build, in parts (each gated independently by the harness)

### Part 1 — The latency instrument 🔨
The hand-rolled histogram (bounded relative error, constant memory, no hot-path allocation, mergeable) and the timestamp log every run emits. **Harness rows:** quantile accuracy vs. the harness's exact recomputation across adversarial distributions (bimodal, heavy-tail), merge correctness, recording-cost row (no allocation per record).

### Part 2 — The CO-safe load generator 🔨
Open-loop, intended-start-time scheduling at a target rate over N connections to the 6.1 server; closed-loop available as a labeled mode. **Harness rows:** the stall test — the harness induces a server stall (it owns a pause mechanism) and asserts your reported p99 includes queueing delay (matching its independent recomputation); seeded run reproducibility; target-rate adherence on an unstalled run.

### Part 3 — YCSB-style A–F 🔨
The six mixes over a seeded Zipfian key space, as SQL. **Harness rows:** per-workload mix ratios match spec within tolerance (audited from the emitted log), seed reproducibility, results-row completeness (every required field present or the row is invalid).

### Part 4 — TPC-C-lite + the consistency battery 🔨
Schema, population (scaled by W), NewOrder/Payment/OrderStatus with the spec's input distributions and cross-warehouse fractions; tpmC-style reporting; the consistency battery wired to run after every run. **Harness rows:** the consistency conditions green after a contended run at each supported isolation level; population conforms to scale rules; the battery itself *flags* a harness-injected inconsistency (the battery is tested, not trusted).

### Part 5 — The harness as a published artifact 🔨
Ablations (group-commit window, pool size, isolation level, lock-vs-MVCC) and the SQLite baseline column; ≥ 3 runs per point, variance reported. Write `RESULTS.md` from `templates/RESULTS.md`: methodology, tables, curves, the CO demonstration pair, and the EXPLAIN paragraph for any SQLite loss. Publish.

## API contract (what the harness drives)

The harness drives the `chronos-bench` CLI and reads its emitted artifacts; it issues consistency-check SQL through the 6.1 server. **PUBLIC SURFACE** below; **PRIVATE INTERNALS — deliberately unspecified:** histogram bucket scheme, generator threading model, connection management, workload-driver structure.

### `chronos-bench` (CLI)

```
chronos-bench --workload ycsb-a..ycsb-f|tpcc --host <h> --port <p>
              --clients <n> --rate <ops/s>|--closed-loop --duration <s>
              --seed <s> --warmup <s> [--warehouses <W>] [--scale <ycsb-rows>]
   emits: results row (machine-parseable: workload, seed, clients, rate/mode,
          durability config echoed from the server, warmup, duration, throughput,
          p50/p90/p99/p99.9, run id)
        + timestamp log: (op-type, intended-start, actual-start, completion) per op
   exit 0 ⇔ run completed AND (tpcc) consistency battery green
```

- **What it does:** runs one seeded workload at the requested load shape against a running `chronosd` (or SQLite, in baseline mode — `--engine sqlite --db <file>`), recording every operation's latency from intended start, then reports.
- **Why it exists:** the instrument all numbers flow from; the same binary produces chronos and baseline columns, which is what makes the comparison fair.
- **Side-effect requirement:** for `tpcc`, the consistency battery executes against the final database state after every run, and a red battery makes the run a *failure* regardless of how good the numbers were.
- **Critical contract details:** identical (seed, flags) ⇒ identical generated operation stream (timings vary; the *requests* don't). The durability config in each results row must be read from the server's reported configuration, not asserted by the bench's flags — a row that can't establish it is marked `durability=UNKNOWN` and the harness rejects it. Warm-up ops are logged but excluded from reported quantiles, and the row says how many were excluded.

### TPC-C-lite consistency battery

```
chronos-bench --check-consistency --host <h> --port <p> [--warehouses <W>]
   evaluates the TPC-C-derived conditions (W_YTD/D_YTD sums, D_NEXT_O_ID vs max order id,
   NEW-ORDER count reconciliation, order-line count vs O_OL_CNT) via plain SQL
   exit 0 ⇔ all conditions hold; violations printed one per line with the failing ids
```

- **What it does:** interrogates the final state with aggregate SQL and asserts the spec-derived invariants.
- **Why it exists:** the correctness half of the headline framing — every performance run re-proves that contention didn't corrupt the bookkeeping. Also the capstone's post-recovery green light (Phase 6.4).
- **Side-effect requirement:** read-only; runnable any number of times with the same verdict on a quiesced database.
- **Critical contract details:** conditions are evaluated through ordinary SQL via the server — same path a user would take — so a visibility or isolation bug can't hide behind a privileged code path. Must run green at every isolation level the benchmark claims to have run under.

## Acceptance criteria (phase-level "done")

1. `test/benchmarks/` — all rows PASS: histogram calibration, the CO stall row, workload-mix audits, consistency battery (including the injected-inconsistency detection row).
2. Full matrix run: YCSB A–F and TPC-C-lite (state your W), each ≥ 3 runs, fsync-on durability, consistency battery green after every TPC-C run.
3. Ablation curves published (group-commit window, pool size, isolation level, lock-vs-MVCC), ≥ 3 samples per point, variance shown; SQLite-WAL baseline column produced by the same instrument; any chronos loss EXPLAINed.
4. `RESULTS.md` published with full methodology (seeds, configs, warm-up, hardware); a stranger can regenerate the tables.

## Principal-engineer traps (no solutions)

- **Coordinated omission is the trap the whole phase is named for.** Measuring only the requests you managed to send under-reports p99 by orders of magnitude — the stalls that matter most generate the fewest samples. The harness's stall row exists because this bug produces *beautiful* numbers.
- **Benchmarking with fsync off — or on a lying filesystem — produces marketing, not engineering.** Record the durability config in every results row, from the server's mouth. The single most common dishonest database benchmark is a durable engine compared against someone else's non-durable config.
- **Measure after warm-up and say so.** Cold-cache numbers answer a different question; mixing them into steady-state quantiles answers no question. The discarded portion is part of the methodology line.
- **An ablation with one sample per point is an anecdote.** Variance across runs is the difference between a curve and a scatter of noise that happens to slope the way you hoped.
- **TPC-C without contention is not TPC-C.** Padding W until conflicts vanish makes throughput climb and the benchmark meaningless — the district hot rows *are* the test. Report W; resist the urge to scale your way out of contention.
- **The bench can bottleneck before the server does.** A generator that saturates its own histogram lock or connection pool measures itself; prove the rig's ceiling exceeds the server's before attributing a plateau to chronos.

## What you hand back for review

1. Implementation + the harness table + RESULTS.md (full matrix, ablations, baseline column, CO demonstration pair).
2. One sentence per trap above: did it bite you, and how did you resolve it?

Review is principal-engineer style: the interview attack ("your p99 doubled between A and F — which subsystem, and what's your evidence?"; "defend your SQLite loss on workload E"), any overstated claims, the next upgrade. Then Phase 6.4.

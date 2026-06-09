# STATUS — source of truth for where the build is

**Current phase:** Tier 0 · Phase 0.1 — The durability contract (`phase_specs/durability_contract.md`)
**State:** not started — repo was reset on 2026-06-09 for the from-scratch rebuild; curriculum authored same day.
**Harness:** not yet written (produced when the maintainer says "scaffold 0.1", per CONTRACT.md).

## Banked knowledge (pre-reset history)
- io_uring fundamentals: SQ/CQ rings, SQE/CQE field layout, raw-syscall setup + mmap of the rings (a partial `cat` clone). Lives in `git log` before the reset commit. Slots into Phase 0.3's prerequisites; does not skip 0.1/0.2.
- From quackdb (../quackdb): full OLAP stack — columnar layout, compression, vectorized execution, SQL parsing, planning, CBO basics, coarse MVCC/WAL. See the overlap map in `build_plan.md`.

## Phase log
| Phase | State | RESULTS |
|---|---|---|
| 0.1 durability_contract | not started | — |

(Add a row when a phase starts; fill RESULTS link when it ships.)

# RESULTS — Tier _ · Phase _._

**Date:** <when>  ·  **Hardware:** <CPU / disk (matters here: NVMe? fs? mount opts?)>  ·  **Env:** <kernel version / compiler / liburing version if used>

## Harness output
```
<paste the PASS/FAIL/SKIP table here>
```
- Sanitizer builds: <ASan/UBSan/TSAN — which ran, all green?>
- Torture iterations: <N seeded crash/fault schedules, 0 violations> (durability phases)

## The key result of this phase
<one paragraph: the load-bearing thing you built and what the harness proved
about it. e.g. for 3.3: recovery replays history and undoes losers such that,
across N randomized SIGKILL points, every acked-committed txn was present and
every uncommitted txn absent — including double-crash-during-undo runs — judged
by a shadow model that never saw the engine's code.>

## Numbers that matter
<phase-specific: syscalls/op, hit rates, commits/sec vs group-commit window,
recovery time vs checkpoint interval, p99 latency. Seeds and run counts stated.>

## Traps that bit me
- <trap>: <how it manifested, how I found it, the fix>

## Reproduce
```
<the exact commands a stranger runs to rerun this — build flags included>
```

## Links
- Implementation: <commit>
- Harness: <commit> (validated per CONTRACT.md five gates)

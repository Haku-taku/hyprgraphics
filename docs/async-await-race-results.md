# Await race — comparison results

Investigation: [`async-await-race.md`](async-await-race.md) · Patch: [`async-await-race.diff`](async-await-race.diff)

## Environment

| | |
|---|---|
| Base | `upstream/main` @ `ade4ef0` (VERSION 0.6.0) |
| Branch | `fix/async-await-race` |
| Compiler | g++ (GCC) 16.2.1 |
| CMake | 4.4.5 |
| Build | `Release` (`-O3`) |
| Host | Linux 7.3.1 (CachyOS), 24 hardware threads |

## Harness

`tests/await_race.cpp` — three scenarios, each a separate invariant:

| Scenario | Invariant |
|---|---|
| `completion-before-await` | A worker that finished before `await()` is called still wakes the caller (R1). |
| `await-returns-published` | 3000 × `enqueue()` + `await()`; every return sees `m_ready` and the rendered payload (R4). |
| `concurrent-waiters` | 8 threads `await()` the same resource; all wake and see the payload (R5). |

A scenario that deadlocks is killed by an external timeout, so the exit code is 124.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -R AwaitRace --output-on-failure
# single scenario (useful when one of them hangs):
./build/hyprgraphics_await_race completion|published|concurrent
```

## Summary

| Scenario | Unpatched (`ade4ef0`) | Patched | Runs (unpatched / patched) |
|---|---|---|---|
| `completion-before-await` | **hang**, exit 124 | PASS | 3 / 5 |
| `await-returns-published` | **FAIL**, exit 1 | PASS | 5 / 5 |
| `concurrent-waiters` | **hang**, exit 124 | PASS | 3 / 5 |

Patched: 5 full runs × 3 scenarios = 15/15 PASS, exit 0. Full run takes ~1.06 s.

## Raw results

### Unpatched

| Scenario | Run | Exit | Observed |
|---|---|---|---|
| `completion-before-await` | 1 | 124 | killed after 10 s, no output |
| `completion-before-await` | 2 | 124 | killed after 10 s, no output |
| `completion-before-await` | 3 | 124 | killed after 10 s, no output |
| `await-returns-published` | 1 | 1 | `await-returns-published FAIL` |
| `await-returns-published` | 2 | 1 | `await-returns-published FAIL` |
| `await-returns-published` | 3 | 1 | `await-returns-published FAIL` |
| `await-returns-published` | 4 | 1 | `await-returns-published FAIL` |
| `await-returns-published` | 5 | 1 | `await-returns-published FAIL` |
| `concurrent-waiters` | 1 | 124 | killed after 10 s, no output |
| `concurrent-waiters` | 2 | 124 | killed after 10 s, no output |
| `concurrent-waiters` | 3 | 124 | killed after 10 s, no output |

`await-returns-published` fails with `m_ready == false` (and the payload flag unset) after
`await()` returned — the waiter woke on the notify that the worker sends *before* setting
`m_ready`.

### Patched

| Scenario | Runs | PASS | FAIL | Hang |
|---|---|---|---|---|
| `completion-before-await` | 5 | 5 | 0 | 0 |
| `await-returns-published` | 5 | 5 | 0 | 0 |
| `concurrent-waiters` | 5 | 5 | 0 | 0 |

```
completion-before-await            PASS
await-returns-published            PASS
concurrent-waiters                 PASS
```

## Corroborating run (older base)

Before the test was moved into the tree it ran as a standalone harness against the
installed `libhyprgraphics` 0.5.1 with the same code paths. Same three failures:

| Mode | Unpatched (system 0.5.1) | Patched build |
|---|---|---|
| completion before await | hang, `timeout 5` → exit 124 | PASS |
| 3000 × enqueue + await | early return at iteration 1901 / 1770 / 1265 (3 runs) | PASS |
| 8 concurrent waiters | hang, `timeout 20` → exit 124 | PASS |
| 8 threads × 500 loops | passed (window not hit) | PASS |

Patched build: 4 modes × 10 repetitions = 0 failures.

## Notes on the numbers

- The early-return scenario is timing dependent; 500 iterations was not enough to hit the
  window reliably, 3000 iterations failed 5/5 on the unpatched build. The in-tree test
  uses 3000.
- The hangs are deterministic: the worker finishes before `await()` installs its CV, so
  there is no notify at all.
- `8 threads × 500 loops` passing on the unpatched code is expected — it shares the
  gatherer but not a resource, so it does not exercise R5.

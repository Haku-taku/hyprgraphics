# `CAsyncResourceGatherer::await()` races with the worker thread

Base: `upstream/main` @ `ade4ef0` (VERSION 0.6.0).
Patch: [`async-await-race.diff`](async-await-race.diff) · Results: [`async-await-race-results.md`](async-await-race-results.md)

## Summary

`await()` creates a fresh `std::condition_variable` per call and the worker thread only
notifies when that pointer already exists. That produces three observable failures:

1. **Deadlock** — the worker finishes before `await()` installs the CV, so nobody ever
   notifies and the caller waits forever.
2. **Early return** — the predicate is written outside the mutex and `m_ready` is set
   *after* `notify_all()`, so a waiter can return before the resource is published.
3. **Deadlock with concurrent waiters** — the single `awaitingCv` slot is overwritten by
   every waiter.

The fix keeps one condition variable for the resource's lifetime, publishes the payload
state under `awaitingMtx`, and notifies after unlocking.

## Impact

The only in-tree caller is `IHyprRenderer::renderText()` in Hyprland, which does
`enqueue()` immediately followed by `await()` on the render thread:

```cpp
auto res = makeAtomicShared<Hyprgraphics::CTextResource>(std::move(data));
g_pAsyncResourceGatherer->enqueue(res);
g_pAsyncResourceGatherer->await(res);   // may block forever
```

A deadlock here freezes the compositor. An early return reads `res->m_asset` without a
happens-before edge to `render()`, i.e. a data race on the cairo surface.

Backgrounds (`CImageResource`) are unaffected in practice because they use the
non-blocking pattern (`m_ready` + `m_events.finished`), never `await()`.

## Root cause

Old code, `src/resource/AsyncResourceGatherer.cpp`:

```cpp
// :32
void CAsyncResourceGatherer::await(CAtomicSharedPointer<IAsyncResource> resource) {
    resource->m_impl->awaitingCv = makeUnique<std::condition_variable>();   // (1)
    std::unique_lock<std::mutex> lk(resource->m_impl->awaitingMtx);
    resource->m_impl->awaitingCv->wait(lk, [&] { return resource->m_impl->awaitingEvent; });
    resource->m_impl->awaitingCv.reset();                                   // (4)
}

// :64, worker
r->render();
if (r->m_impl->awaitingCv) {                                                // (2)
    r->m_impl->awaitingEvent = true;                                        // (3)
    r->m_impl->awaitingCv->notify_all();
}
r->m_ready = true;                                                          // (5)
```

| # | Defect | Failure |
|---|---|---|
| R1 | `await()` allocates the CV before taking the lock; the worker checks `if (awaitingCv)` | Worker completes first → no CV → no notify → waiter sleeps forever |
| R2 | `awaitingCv` is read by the worker and written/reset by the caller without synchronisation | Data race on the pointer itself |
| R3 | `await()` resets the CV right after waking, while the worker's `notify_all()` does not hold the mutex | Use-after-free of a heap `condition_variable` |
| R4 | `awaitingEvent` and `m_ready` are written outside `awaitingMtx`, and `m_ready` after the notify | No happens-before edge; waiter can return before publication |
| R5 | One CV slot per resource, overwritten by every caller | Concurrent waiters overwrite each other and never wake |

## Fix

`src/resource/resources/AsyncResource.hpp`:

```cpp
struct SAsyncResourceImpl {
    std::mutex              awaitingMtx;
    std::condition_variable awaitingCv;          // lives as long as the resource
    bool                    awaitingEvent = false; // guarded by awaitingMtx
};
```

`src/resource/AsyncResourceGatherer.cpp`:

```cpp
void CAsyncResourceGatherer::await(CAtomicSharedPointer<IAsyncResource> resource) {
    std::unique_lock<std::mutex> lk(resource->m_impl->awaitingMtx);
    resource->m_impl->awaitingCv.wait(lk, [&] { return resource->m_impl->awaitingEvent; });
}

// worker, after r->render()
{
    std::scoped_lock lg(r->m_impl->awaitingMtx);
    r->m_ready               = true;
    r->m_impl->awaitingEvent = true;
}
r->m_impl->awaitingCv.notify_all();
```

Why this is enough:

- A CV that outlives every call removes R1 (no "installed too late" window), R3 (nothing is
  freed while the worker may still notify) and R5 (all waiters share one CV and are all
  woken by `notify_all`).
- Writing the predicate and `m_ready` under the same mutex that the waiter holds gives the
  publication a happens-before edge, removing R4.
- The notify stays outside the lock, so a woken waiter does not immediately block on a
  mutex the worker still holds.

## Verification

`tests/await_race.cpp` (registered with CTest as `AwaitRace`, timeout 60 s) pins the three
invariants. Run against the unpatched tree it hangs or fails; against the patched tree it
passes. Full numbers and commands: [`async-await-race-results.md`](async-await-race-results.md).

| Scenario | Unpatched | Patched |
|---|---|---|
| `completion-before-await` | hang (3/3) | PASS (5/5) |
| `await-returns-published` | FAIL (5/5) | PASS (5/5) |
| `concurrent-waiters` | hang (3/3) | PASS (5/5) |

## Notes

- **No public ABI change.** `SAsyncResourceImpl` is opaque outside the library; only its
  heap allocation inside `IAsyncResource` changes. Consumers do not need a rebuild.
- **The event is sticky.** `awaitingEvent` stays `true` after completion, so a later
  `await()` on the same resource returns immediately instead of blocking.
- **`enqueue()` twice is still a caller error.** Re-enqueueing an already completed
  resource re-renders it, but the sticky flag makes a second `await()` return before that
  re-render. The gatherer never promised anything else; deduplicate at the call site.
- **This does not make `enqueue()` + `await()` faster.** The calling thread still blocks
  for the whole render. Real overlap requires the non-blocking pattern
  (`m_ready` + `m_events.finished` → main loop → damage → build the texture next frame),
  which is what `CBackground` already does.

## Suggested commit message

```
resource: fix a race in CAsyncResourceGatherer::await()

await() allocated a condition variable per call and the worker only
notified if that pointer already existed. A worker that finished before
await() installed the CV never woke the caller, concurrent waiters
overwrote each other's CV, and the caller could return before m_ready /
m_asset were published because the predicate was written outside the
mutex and m_ready after the notify.

Keep one condition variable per resource, publish m_ready and the
predicate under awaitingMtx, and notify after unlocking. Add
tests/await_race.cpp, which deadlocks or fails on the old code.
```

# Locking, re-entrancy and shutdown discipline

Copyright 2026 Summon Software Labs. SPDX-License-Identifier: Apache-2.0

This document is the authoritative statement of the concurrency contract. It
was written by inspecting every lock acquisition in the runtime, not by
inferring behaviour from tests.

## Lock inventory

| Lock | Owns | Never held across |
|---|---|---|
| `Governor::mutex_` | snapshots, publisher registry, epoch/boot authority, durable state, engine and tracker | the worker pool, the coordinator mutex, the store's own file operations that could re-enter the governor, or any callback |
| `Coordinator::mutex_` | listener state, connection list, progress counters | any call into the governor |
| `WorkerPool::mutex_` | queue, counters, lifecycle flags | execution of a task |
| `Connection::stopping` (atomic) | per-connection cancellation | - |

## Global lock order

```
Governor::mutex_  ->  Store (single-threaded file I/O, no locks)
Coordinator::mutex_  ->  (never calls into the Governor while held)
WorkerPool::mutex_  ->  (never held while a task runs)
```

The coordinator mutex and the governor mutex are **never nested**: every call
into `Governor` from a connection thread happens with no coordinator lock held,
and every counter update happens after the governor call returns. There is
therefore no lock cycle between the two components, in either direction.

## Audited hazards

1. **Read-lock to write-lock re-entry on the same lock.** Not present. The
   runtime uses `std::mutex` only; there is no `shared_mutex` anywhere, so a
   read-then-write upgrade cannot occur.
2. **Write lock held across callbacks.** Not present. The runtime has no
   user-supplied callbacks: decisions are *returned* from `Governor::evaluate`,
   never emitted from inside the lock.
3. **Mutex re-entry through callbacks.** Not present for the same reason. A
   worker task may call `WorkerPool::submit`; `submit` takes the pool mutex
   while the task runs with no pool lock held, and it never blocks on a full
   queue (it returns `QueueFull`), so it cannot deadlock against itself.
4. **Event emission while internal locks are held.** Not present. Nothing in
   the runtime emits events. The coordinator's `progress_` condition variable
   is signalled after the coordinator mutex is released or immediately before
   it is released by `std::lock_guard` scope exit, and the waiters only read
   counters.
5. **Worker shutdown while holding locks workers need.** `WorkerPool::*
   _and_stop` releases the pool mutex before joining, and
   `Governor::stop_workers` moves the pool out of the governor under the lock
   and then joins with the governor lock released. A worker that needs the
   governor to finish can always acquire it.
6. **Joining a thread while holding state that thread requires.**
   `Coordinator::stop` swaps the connection list out under the mutex, releases
   it, then shuts the sockets down and joins. `Coordinator::run` waits on
   `progress_` and is woken by `stop` after `running_` is cleared under the
   lock.
7. **Reversed lock ordering on cancellation paths.** Cancellation uses
   `Connection::stopping` (an atomic) plus `Socket::shutdown_both`, neither of
   which takes a lock. The cancellation path therefore acquires nothing and
   cannot invert an ordering.
8. **Progress callbacks that re-enter mutable state.** Not present; there are
   no progress callbacks. `Coordinator::wait_for_publishers` and
   `wait_for_admissions` block on a condition variable and only read counters.
9. **Shutdown paths that prevent work completing while waiting for it.**
   `WorkerPool::drain_and_stop` sets `stopping_` and lets workers run the queue
   to exhaustion before exiting; `cancel_and_stop` clears the queue first and
   then waits for *active* tasks only. Neither waits for work it has already
   cancelled.
10. **Nested resource acquisition with inconsistent global ordering.** The
    only nesting is `Governor::mutex_` -> internal `std::unique_ptr<Store>`
    calls. The store holds no lock of its own and never calls back into the
    governor, so the nesting is a strict chain with a single order.

## Cancellation semantics

* Cancelled work never reports success. `WorkerPool` accounts dropped tasks
  separately from completed and failed tasks, and a task that was dropped is
  never executed.
* `Governor` has no asynchronous work in flight: `admit` and `evaluate`
  either fully commit or leave state untouched. A rejected frame never mutates
  authoritative state, because every snapshot is decoded into a local and
  installed only after the integrity, epoch, incarnation, sequence, generation
  and structural checks all pass.
* The coordinator closes a session when the epoch or incarnation is no longer
  current, so a fenced publisher cannot keep a stale session alive.

## Shutdown order

1. `Coordinator::stop`: stop accepting, close the listener, join the accept
   thread, signal every connection, join every connection thread.
2. `Governor::stop_workers`: move the pool out under the lock, then drain and
   join with no lock held.
3. `Governor` destruction: `stop_workers` is idempotent.
4. `Store::close`: close the journal. Durable state is committed explicitly by
   `Governor::commit_durable`, never implicitly at shutdown.

# Hotspot Governor

Open-source, vendor-neutral C++20 runtime for generation-bound identification
and mitigation intent for localized path, link, switch, queue, or resource
saturation inside a larger fabric.

Version 1.0.0. Apache License 2.0.

## The question this answers

> Given authoritative utilization, queue, buffer, capacity, topology, path, and
> traffic evidence, is congestion localized to a specific fabric region or
> resource, what is the hotspot scope and cause, which traffic contributes, and
> what bounded mitigation is authorized?

## Systems boundary

Hotspot Governor owns **localized saturation identification, scope,
contribution attribution, and bounded mitigation intent**.

It does **not** own global congestion state, topology truth, route computation,
path legality, flow placement, rate enforcement, queue or buffer implementation,
pacing, or recovery sequencing. It never programs a switch, never writes a
route, never enforces a rate and never moves a flow. It emits intent plus the
authority vector that justifies it, and hands that to the systems that own those
decisions.

## What is implemented

* **Strong identities and generations.** Typed identities for resources,
  regions, links, paths, flows, hotspots, evidence sets, capacity snapshots,
  path sets, traffic sets, policies and interventions; generations for topology,
  capacity, signals, paths, traffic and policy; coordinator epochs, boot
  incarnations and publisher incarnations. Every stream generation is
  authoritative and monotonic per publisher and per stream.
* **Generation-bound evidence.** Capacity binds a topology generation; signals
  bind topology and capacity generations; paths bind a topology generation;
  traffic binds a path generation. Detection refuses any snapshot whose binding
  does not match the live upstream generation.
* **Structural verification.** Every admitted snapshot is checked once against
  the live topology generation: evidence that names a resource the topology does
  not define is refused, and the previous snapshot stays installed.
* **Localized saturation detection.** Per-resource utilization, queue and buffer
  pressure from capacity and signal evidence; connected components over the
  saturated set using exact topology adjacency; scope classification into
  `localized`, `regional`, `global`, `none` and `unknown`; cause
  classification into capacity exhaustion, queue buildup, buffer exhaustion,
  microburst, fan-in contention and structural convergence.
* **Persistence and severity.** A saturation tracker measures *continuous*
  saturation across evaluations. A resource that stops being saturated loses its
  history immediately, and an evaluation that cannot admit the evidence breaks
  the series rather than carrying it forward. A hotspot is only confirmed once
  it has held for the policy persistence window and sample count.
* **Contribution attribution.** Paths traversing the hotspot are joined with
  per-flow demand evidence, aggregated per flow, ranked, and reported with
  shares and an attribution confidence. Stale contributor evidence is excluded
  and lowers that confidence; attribution-dependent mitigation is withheld when
  the confidence is below the policy floor.
* **Bounded mitigation intent.** Flow relocation, path rebalancing, affected-
  scope admission reduction, rate/pacing change, degraded-resource isolation and
  escalation to global congestion ownership. Every intent is checked against the
  per-intent and whole-plan budgets, the affected scope share, the live topology
  generation and the policy's kind allow-list. Refusals are recorded, never
  silent. Local mitigation cannot silently become global policy: when the
  observed scope exceeds what local mitigation can cover, the plan carries an
  explicit escalation intent instead.
* **Authority vector.** Every decision states, per authority domain, whether it
  was granted or denied, why, and the exact generation binding. A refusal in any
  entry for a domain denies that domain, so a later finding always overrides an
  earlier one.
* **Bounded explanation.** Scope, binding, coverage, hotspot resources, healthy
  neighbour comparison, contributors, binding thresholds, authorized and
  suppressed mitigation, escalation and the authority vector, rendered within
  hard line and byte bounds.
* **Real transport.** A fixed 88-byte frame header with separate header and
  payload CRC-64, over real OS sockets (WinSock or POSIX). Frames are checked
  for magic, protocol version, declared size bound, header CRC and payload CRC
  before any field is trusted.
* **Coordinator and publisher processes.** `hgm_coordinator` owns the epoch,
  accepts real connections, admits verified frames and evaluates;
  `hgm_publisher` generates synthetic fabric evidence and publishes it over the
  framed protocol.
* **Durable state.** A CRC-protected append-only journal plus an atomically
  replaced snapshot, committed in the order journal -> fsync -> snapshot ->
  atomic replace -> journal reset. Recovery restores committed history only:
  publisher authority, evidence freshness, leases and worker authority are never
  restored, and a restart always advances the coordinator epoch.
* **Bounded everything.** Compile-time bounds on resources, regions, links,
  paths, hops, signals, traffic, hotspots, contributors, intents, affected sets,
  frame size, publishers, connections, worker threads, queue depth, journal
  records, durable bytes, explanation lines and explanation bytes. Checked
  integer arithmetic for every externally influenced size, capacity, rate,
  counter and time unit.

## UNKNOWN is preserved

`CongestionScope::Unknown` is returned whenever the answer cannot be
established: missing or stale topology, missing capacity, a capacity or signal
generation mismatch, capacity or admissible telemetry coverage below the policy
floor, contradictory samples (occupancy pressure with no load behind it, or
offered load with no observable effect anywhere), or saturation that has no
healthy, evidence-backed neighbour to compare against. `none` is only ever
returned on evidence that passed every admissibility test. Missing or stale
evidence never becomes a positive answer.

## Requirements

* CMake 3.20 or newer.
* A C++20 compiler. Validated with MSVC 19.44 (Visual Studio 2022 17.14).
* Threads. On Windows, WinSock is linked automatically.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Options:

| Option | Default | Meaning |
|---|---|---|
| `HGM_BUILD_TESTS` | `ON` | Build the in-process and multiprocess test suites |
| `HGM_BUILD_TOOLS` | `ON` | Build `hgm_cli`, `hgm_coordinator`, `hgm_publisher` |
| `HGM_BUILD_BENCH` | `ON` | Build the synthetic benchmark harness |
| `HGM_WARNINGS_AS_ERRORS` | `ON` | `/WX` on MSVC, `-Werror` elsewhere |
| `HGM_ENABLE_ASAN` | `OFF` | AddressSanitizer/UndefinedBehaviorSanitizer |
| `HGM_ENABLE_MSVC_ANALYZE` | `OFF` | MSVC `/analyze` |
| `HGM_BUILD_MULTIPROCESS_TESTS` | `ON` | Build the real-child-process suite |

Release and Debug both compile clean under `/W4 /WX /permissive-`.

## Install and consume

```
cmake --install build --prefix /your/prefix
```

The install exports the package `HotspotGovernor` with the target
`HotspotGovernor::hotspot_governor`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyConsumer LANGUAGES CXX)
find_package(HotspotGovernor 1.0 REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE HotspotGovernor::hotspot_governor)
```

```cpp
#include <hgm/engine.hpp>
#include <hgm/synthetic.hpp>

// Build generation-bound evidence, then ask the core question.
hgm::SyntheticFabric fabric;         // SYNTHETIC population
fabric.rebuild(now, generation);
hgm::Engine engine;
hgm::DecisionInput input;
input.topology = &fabric.topology();
input.capacity = &fabric.capacity();
input.signals  = &fabric.signals();
input.paths    = &fabric.paths();
input.traffic  = &fabric.traffic();
input.policy   = &policy;            // a live PolicySnapshot
input.now = now;
input.epoch = epoch;
input.boot = boot;
input.tracker = &engine.tracker();
hgm::Decision decision = engine.decide(input);
// decision.assessment.scope, .hotspots, .plan.intents, .explanation
```

## Tools

```
hgm_cli version
hgm_cli selfcheck
hgm_cli demo --regions 4 --resources 8 --hotspots 1 --fan-in 4 [--global] [--stale]

hgm_coordinator --port 0 [--state-dir DIR] [--exit-after-evals N] [--max-connections N]
hgm_publisher  --host 127.0.0.1 --port P --name NAME --incarnation N --boot N
               [--rounds N] [--mode localized|global] [--evidence fresh|stale]
               [--force-epoch N] [--regions N] [--resources N] [--hotspots N] [--fan-in N]
```

## Verification

### REAL

* Unit, integration, property, adversarial, concurrency and hardening suites
  running the actual runtime: **163 in-process tests**.
* **5 multiprocess tests** that start real OS processes which talk over real
  loopback TCP with the framed protocol, and hard-kill a publisher with
  `TerminateProcess` (SIGKILL on POSIX) mid-stream.
* Hard process kill, publisher loss detection, fresh incarnation fencing, stale
  incarnation refusal, unknown-epoch refusal, coordinator restart with epoch
  advancement and stale-epoch refusal after restart.
* Durability: journal replay, torn-tail truncation, snapshot corruption,
  truncation, unsupported durable version, leftover temporary snapshot,
  crash-safe ordering, journal growth bounds.
* `AddressSanitizer` on the **x86 (Win32)** configuration: all 17 CTest entries
  pass. The x64 ASan runtime is **not installed** in this environment and no
  GCC/Clang toolchain is present, so **no x64 sanitizer run was performed**.
* MSVC `/analyze` over the library: **no first-party findings**. The only two
  warnings are `C6101` inside the Windows SDK header `ws2tcpip.h`.
* Release and Debug builds compile clean under `/W4 /WX /permissive-`.
* An independent downstream CMake consumer that knows nothing about this source
  tree resolves `find_package(HotspotGovernor 1.0 REQUIRED)`, links, runs, and
  drives detection, framing and transport.

### SYNTHETIC

* The benchmark harness and `hgm_cli` operate on generated fabric populations.
  These are synthetic: no switch, NIC, link, queue, RDMA, NVLink, optical or DPU
  hardware is involved, and no result from them is a physical-network
  measurement.
* Because detection needs continuously admissible evidence across evaluations,
  a hotspot is confirmed only after the persistence window is met.

### UNSUPPORTED / NOT VALIDATED

* Multi-node, multi-switch, RDMA, NVLink, optical, NIC and DPU behaviour.
* Physical fabric telemetry of any kind.
* POSIX sockets: the code path is implemented, but this validation run used
  Windows only.
* x64 AddressSanitizer and any GCC/Clang sanitizer build.
* Performance claims beyond the synthetic benchmark below.

### Benchmark (SYNTHETIC)

Measured quantity: completed work through the real runtime path -- framed
admission into a governor, then one evaluation per round. Synthetic fabric
generation is excluded and reported separately. Environment: Windows, MSVC
19.44, Release, `x86_64`, single host, `hgm_bench` at the 1.0.0 tag.

| case | resources | paths | evals | hotspots | intents | ms/eval | ns per resource-eval |
|---|---|---|---|---|---|---|---|
| graph-1k | 1,024 | 4,096 | 4 | 1 | 1 | 6.49 | 6,333 |
| graph-8k | 8,192 | 32,768 | 4 | 1 | 1 | 56.53 | 6,901 |
| graph-64k | 65,536 | 262,144 | 4 | 1 | 1 | 541.84 | 8,268 |
| hotspots-8 | 4,096 | 16,384 | 4 | 8 | 8 | 26.79 | 6,541 |
| hotspots-64 | 4,096 | 16,384 | 4 | 64 | 8 | 26.85 | 6,554 |
| hotspots-256 | 4,096 | 16,384 | 4 | 256 | 8 | 27.65 | 6,750 |
| fanin-1 | 2,048 | 2,048 | 4 | 4 | 4 | 7.00 | 3,417 |
| fanin-16 | 2,048 | 32,768 | 4 | 4 | 4 | 37.07 | 18,098 |
| fanin-64 | 2,048 | 131,072 | 4 | 4 | 4 | 123.78 | 60,437 |
| density-100 | 2,048 | 8,192 | 4 | 4 | 4 | 12.33 | 6,019 |
| density-70 | 2,048 | 8,192 | 4 | 3 | 3 | 12.45 | 6,078 |
| density-40 | 2,048 | 8,192 | 4 | 0 | 0 | 11.56 | 5,646 |
| global-8k | 8,192 | 32,768 | 4 | 1 | 1 | 65.94 | 8,049 |

Observations from this run: cost is linear in resource count (6.3-8.3
microseconds per resource-evaluation across a 64x graph-size sweep); the
per-plan intent count stays bounded at the policy's `max_intents_per_plan`
regardless of hotspot count; reducing evidence density below the policy floor
turns the verdict into `unknown` instead of guessing; and global congestion
produces exactly one escalation intent.

## Design notes

* `docs/ARCHITECTURE.md` -- boundary, data flow, generation binding, durability
  ordering, repository layout.
* `docs/LOCKING.md` -- the concurrency contract, the global lock order, and the
  results of the deadlock and lock re-entrancy audit.

## Reliability properties

* A frame is decoded into a local and installed only after integrity, protocol
  version, size bound, epoch, incarnation, sequence, generation and structural
  checks all pass. A rejected frame never mutates authoritative state.
* Duplicate and out-of-order sequences are refused; duplicate generations are
  refused; generation regressions are refused per stream.
* A publisher incarnation that dies loses its authority immediately and cannot
  resume it; a strictly higher incarnation fences the previous one; the same
  incarnation claiming a different boot id is refused.
* Advancing the coordinator epoch fences every registered publisher, and the new
  epoch is durable before it becomes live.
* A connection whose thread retires itself is detached, and one that is still
  live at shutdown is joined; exactly one of the two paths owns the thread
  handle.
* Detection never consults evidence from a different topology, capacity, path or
  traffic generation than the one it is bound to.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

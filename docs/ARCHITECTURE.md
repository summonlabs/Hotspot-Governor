# Architecture and systems boundary

Copyright 2026 Summon Software Labs. SPDX-License-Identifier: Apache-2.0

## What Hotspot Governor owns

Given authoritative utilisation, queue, buffer, capacity, topology, path and
traffic evidence, Hotspot Governor answers four questions and nothing else:

1. **Is congestion localized** to a specific fabric region or resource?
2. **What is the hotspot scope and cause?**
3. **Which traffic contributes?**
4. **What bounded mitigation is authorized?**

It owns: localized saturation identification, scope, contributing-traffic
attribution, persistence and severity, and *bounded mitigation intent*.

## What it does not own

Global congestion state, topology truth, route computation, path legality, flow
placement, rate enforcement, queue or buffer implementation, pacing, and
recovery sequencing. The runtime never programs a switch, never writes a route,
never enforces a rate and never moves a flow. It produces intent plus the
authority vector that justifies it, and hands that to the systems that own
those decisions.

## Data flow

```
publisher processes                     coordinator process
  |  framed evidence over TCP             |
  +-------------------------------------->+  Coordinator
                                          |    epoch authority, sessions
                                          v
                                       Governor
                                          |  integrity -> epoch -> incarnation
                                          |  -> sequence -> generation -> structure
                                          v
                                    generation-bound snapshots
                                          |
                                          v
                                       Engine
                                          |  detect -> classify -> attribute -> plan
                                          v
                                    Decision (assessment + plan + explanation)
                                          |
                                          v
                                       Store  (journal -> snapshot -> journal reset)
```

## Generation binding

Every authoritative stream carries its own monotonic generation:

| Stream | Generation | Binds |
|---|---|---|
| topology | `TopologyGeneration` | - |
| capacity | `CapacityGeneration` | `TopologyGeneration` |
| signals | `EvidenceGeneration` | `TopologyGeneration`, `CapacityGeneration` |
| paths | `PathGeneration` | `TopologyGeneration` |
| traffic | `TrafficGeneration` | `PathGeneration` |
| policy | `PolicyGeneration` | - |

Detection refuses any snapshot whose binding does not match the live upstream
generation. This is what makes "hotspot membership uses exact topology and path
generations" a property of the runtime rather than a convention.

## UNKNOWN preservation

`CongestionScope::Unknown` is returned whenever the answer cannot be
established: missing or stale topology, missing capacity, capacity or signal
generation mismatch, telemetry coverage below the policy floor, admissible
coverage below the floor, contradictory samples, or saturation that cannot be
compared against a healthy neighbour. `CongestionScope::None` is only ever
returned on evidence that passed every admissibility test. Missing or stale
evidence never becomes a positive answer.

## Authority

Every decision carries an `AuthorityVector`: one entry per domain
(coordinator epoch, boot incarnation, publisher incarnation, topology,
capacity, signals, paths, traffic, policy, attribution, persistence, mitigation
budget) stating granted or denied with a reason and the exact binding. A denial
in any entry for a domain makes the domain denied, so a later finding always
overrides an earlier one.

## Durability

`Store` commits in the order: journal records -> fsync -> snapshot written to a
temporary file -> fsync -> atomic replace -> journal reset. A crash at any point
recovers either the previous snapshot plus the complete journal, or the new
snapshot. Recovery restores durable configuration and committed history only;
publisher authority, evidence freshness, leases and worker authority are never
restored and must be re-established by live processes. A restart always advances
the coordinator epoch.

## Repository layout

```
include/hgm/    public headers: model, detection, planning, runtime, durability
src/            implementation
tools/          hgm_cli, hgm_coordinator, hgm_publisher
bench/          synthetic benchmark harness
tests/          in-process suites plus the real multiprocess suite
docs/           this document and LOCKING.md
```

# Heterogeneous Accelerator Federation

Open-source, vendor-neutral C++20 runtime for federating heterogeneous accelerator fleets
through capability negotiation, compatibility, placement eligibility, migration, and workload
portability.

**Federation is not homogenization.** Two accelerators belonging to one federation does not
imply that they execute the same binaries, expose the same ISA, support the same numeric
modes, provide equivalent memory behaviour, support identical kernels or communication
primitives, expose the same device features, support migration of live execution state,
provide equivalent performance, or expose the same runtime API. Compatibility is established
explicitly, from evidence, against workload requirements. Missing evidence is `UNKNOWN`, and
`UNKNOWN` fails closed.

---

## Contents

- [The central question](#the-central-question)
- [Systems boundary](#systems-boundary)
- [Adjacent-runtime separation](#adjacent-runtime-separation)
- [Architecture](#architecture)
- [Federation authority model](#federation-authority-model)
- [Capability semantics](#capability-semantics)
- [Workload requirements](#workload-requirements)
- [Compatibility decisions](#compatibility-decisions)
- [Portability taxonomy](#portability-taxonomy)
- [Migration semantics](#migration-semantics)
- [Evidence model](#evidence-model)
- [Persistence and recovery](#persistence-and-recovery)
- [Control plane](#control-plane)
- [Build](#build)
- [Test](#test)
- [Install and consume](#install-and-consume)
- [Examples](#examples)
- [CLI](#cli)
- [Benchmarks](#benchmarks)
- [Real hardware validation](#real-hardware-validation)
- [REAL, SYNTHETIC, UNSUPPORTED](#real-synthetic-unsupported)
- [Known limitations](#known-limitations)
- [Repository layout](#repository-layout)
- [License](#license)

---

## The central question

> How can CUDA, ROCm, and other accelerator fleets participate in one authoritative execution
> federation without erasing the capability, compatibility, portability, and authority
> differences that determine whether work can actually run or move between them?

The runtime answers that question with four explicit, inspectable artefacts:

1. **Capability evidence** — what a device was observed to do, by which adapter, with what
   provenance, and how fresh it is.
2. **Compatibility decisions** — a typed `ELIGIBLE` / `INELIGIBLE` / `UNKNOWN` outcome for one
   workload against one accelerator, with a deterministic, generation-bound explanation.
3. **Portability classification** — exactly what must happen before work valid on accelerator A
   is valid on accelerator B.
4. **Migration plans** — the required transformations, bound to the authority that would commit
   them.

## Systems boundary

Given a set of independently reported accelerators and workload requirements, the runtime
determines:

- which accelerator resources are genuinely compatible with a workload;
- what portability relationship exists between two accelerators;
- which execution targets are eligible;
- when workload state may migrate or be reconstructed elsewhere;
- which federation authority is currently allowed to make those claims.

It **does not** run workloads, schedule them, compile kernels, virtualize devices, manage
memory pools, or own any physical topology.

## Adjacent-runtime separation

| Adjacent system | Owned by that system | Owned here |
| --- | --- | --- |
| Rack / cluster fabric | Physical and system membership, topology | References into topology only, never topology itself |
| Resource broker | Resource acquisition and allocation | Eligibility, never reservation of physical resources |
| Scheduler | When and where runnable work executes | Deterministic ranking **only** among already-eligible federation targets, for demonstration and migration planning |
| Hardware capability registry | Durable canonical capability knowledge | Observed capability evidence for the current federation, with freshness and provenance |
| Accelerator virtualization | Virtual device identity, tenancy, multiplexing | Partition information **only as observed capability**, never partition lifecycle |

This repository is not a scheduler, a broker, a cluster manager, a compiler, a driver, a vendor
runtime replacement, a container orchestrator, a model router, a coherence protocol, a memory
pooling runtime, or a workload execution framework.

## Architecture

```
include/haf/
  core/         strongly typed identities, generations, typed status, semantic versions,
                canonical SHA-256 / FNV-1a hashing, bounded byte codec, time
  model/        capability registry and values, evidence, accelerator descriptor,
                workload requirements, policy, compatibility decisions, portability,
                migration plans, membership lifecycle
  engine/       portability classifier, compatibility engine, deterministic ranking,
                migration planner, invariant audit
  federation/   the authoritative runtime: state, authority validation, events, snapshots
  persist/      integrity-protected atomic file store
  net/          framing, typed messages, TCP sockets, server, client, control plane
  adapters/     vendor adapter interface, synthetic profiles, real CUDA adapter
```

Layers, from the bottom up:

| Target | Contents |
| --- | --- |
| `haf_core` | Vendor-neutral federation runtime. No network, no vendor SDK, no CUDA. |
| `haf_net` | Framed TCP control plane built on `haf_core`. |
| `haf_adapters` | Adapter interface plus deterministic synthetic profiles. |
| `haf_adapter_cuda` | Optional real CUDA adapter (built only when a CUDA toolkit is found). |
| `haf_coordinator`, `haf_agent`, `haf_cli` | The distributed control plane and inspection surface. |

The core library compiles and operates with no CUDA and no ROCm present.

### Repository layout

```
include/haf/     public headers (installed)
src/             implementation
apps/            haf_coordinator, haf_agent, haf_cli
tests/           framework + support + 8 suites
examples/        eight runnable examples
benchmarks/      federation benchmarks
cmake/           compiler/warning policy and package config
```

## Federation authority model

Authority is a first-class system property. **No mutation is accepted solely because an object
identity exists.** Every mutation presents an `AuthorityClaim` naming the exact generations the
caller believes are current, and the federation compares that claim against its own state:

| Field | Rejection when stale |
| --- | --- |
| `FederationGeneration` | `StaleFederationGeneration` |
| `CoordinatorEpoch` | `StaleEpoch` |
| `AgentId` | `StaleAgent` |
| `AgentBootId` | `StaleBoot` |
| `DeviceGeneration` | `StaleDeviceGeneration` |
| `CapabilityGeneration` | `StaleCapabilityGeneration` |
| `PolicyGeneration` | `StalePolicy` |
| `WorkloadRevision` | `StaleMigration` / stale decision detection |
| `DecisionGeneration` / `MigrationGeneration` | `StaleDecision` / `StaleMigration` |

Every error is a stable machine-readable `ErrorCode`; human text supplements it and never
replaces it.

### Membership lifecycle

```
Discovered -> Observed -> Admitted -> Active
                              |          |
                              v          v
                          Degraded <-----+
                              |   \        \
                              v    v        v
                          Draining  Fenced  Retired (terminal)
                              \       |
                               \      v
                                +-> Retired / Observed (via reopen)
```

Legal transitions are an explicit table (`is_legal_transition`); anything not listed is refused
with `InvalidTransition`. Retired is terminal: a retired incarnation cannot be revived, and its
evidence is dead.

A restarted agent produces a new `AgentBootId`, therefore a new `AcceleratorId`; the previous
incarnation of the same physical device is retired explicitly rather than silently inherited.
An agent session that ends fences every member it owned, so a dead or half-open session can
never keep a device eligible for new work.

## Capability semantics

Capabilities are identified by canonical dotted names from a closed registry (see
`include/haf/model/capability_key.hpp`). Four states are used:

| State | Meaning |
| --- | --- |
| `SUPPORTED` | The advertiser positively claims the capability. |
| `UNSUPPORTED` | The advertiser positively claims the absence. |
| `DEGRADED` | The capability exists but is currently impaired. Fails hard requirements unless policy permits degraded capabilities. |
| `UNKNOWN` | No evidence. This is the state of every capability not advertised inside a namespace the advertiser declared **closed**. |

An advertisement declares the namespaces it speaks for authoritatively. Absence inside a closed
namespace is positive evidence of absence; absence anywhere else is `UNKNOWN`. This is what
makes "the device did not mention it" different from "the device does not have it".

Values are typed (`Presence`, `Version`, `Integer`, `Scalar`, `Enumeration`, `TokenSet`),
canonicalized before hashing or comparison (tokens lowercased, sorted, deduplicated; `-0.0`
normalized; non-finite scalars rejected), and content-addressed. Vendor-specific data lives in
adapter-owned extension keys of the form `x.<namespace>.<name>`, which can never satisfy a core
requirement.

## Workload requirements

A workload profile is a set of typed capability requirements plus execution-level constraints:

| Constraint | Values |
| --- | --- |
| `execution_mode` | `ExactBinary`, `EquivalentAllowed`, `AnyPortability` |
| `minimum_portability` | Any portability class |
| `migration_need` | `None`, `RestartOnly`, `CheckpointRestore`, `LiveStateTransfer` |
| `reconstruction_allowed` | Whether rebuilding state elsewhere is acceptable |
| `required_policy_tags` | Policy tags the destination must provide |

Requirements are `Hard` or `Soft`. **Hard requirements determine eligibility; soft preferences
may only influence deterministic ranking and can never rescue an ineligible accelerator.**

## Compatibility decisions

For every (workload, accelerator) pair the engine produces a typed outcome and an ordered,
deterministic explanation:

- `ELIGIBLE` — every hard requirement is positively satisfied;
- `INELIGIBLE` — a hard requirement is positively violated;
- `UNKNOWN` — evidence needed to decide a hard requirement is missing.

Each decision binds the generations of the workload requirements, capability evidence, policy,
device incarnation, and coordinator authority, and carries a SHA-256 fingerprint over exactly
those inputs. Identical inputs always produce identical fingerprints; any relevant generation
change makes the decision stale by construction.

Rejection codes are specific where the vocabulary allows: `VendorMismatch`,
`ArchitectureMismatch`, `RuntimeMismatch`, `RuntimeVersionUnsupported`, `IsaIncompatible`,
`InsufficientMemory`, `NumericModeUnsupported`, `UnsupportedCapability`, `UnknownCapability`,
`PolicyMismatch`, `PortabilityConstraintViolated`, `MigrationUnsupported`.

### Hard eligibility precedes ranking

`rank_candidates` exists only to order **already-eligible** candidates deterministically. It
returns eligible candidates separately from excluded ones; an ineligible or unknown candidate
is never ranked alongside an eligible one. Ordering uses integer comparisons and a stable
identity tie-break — never container iteration order, pointer values, thread timing, or vendor
enumeration order.

## Portability taxonomy

| Class | Meaning |
| --- | --- |
| `NATIVE` | Same code object target, same architecture, same vendor. |
| `BINARY_COMPATIBLE` | A code object the source already produces is directly loadable. |
| `RECOMPILE_REQUIRED` | Source must be rebuilt for the destination ISA. |
| `REPACKAGE_REQUIRED` | A code object exists but runtime packaging must be rebuilt. |
| `STATE_RECONSTRUCTION_REQUIRED` | State cannot move; the workload must be reconstructed. |
| `CHECKPOINT_RESTORE_SUPPORTED` | State can be exported and restored, but not while the source runs. |
| `LIVE_MIGRATION_SUPPORTED` | State can transfer while the source is live, with one authority. |
| `UNSUPPORTED` | No known path. |
| `UNKNOWN` | Evidence is missing. |

The classifier fails closed: if any input needed to justify a class is `UNKNOWN`, the result is
`UNKNOWN`, never a weaker concrete class. Live migration is never claimed across vendors unless
both accelerators explicitly support cross-vendor state transfer **and** policy permits it.

## Migration semantics

A plan binds source and destination (with device and capability generations), the workload
revision, the referenced compatibility decisions, the portability class, the policy generation,
the destination evidence provenance, a deterministic cost estimate, and the required
transformation steps.

Outcomes: `MOVE_NATIVE_STATE`, `RESTORE_CHECKPOINT`, `RECOMPILE_THEN_RESTORE`,
`REPACKAGE_AND_RESTART`, `RECONSTRUCT`, `RESTART`, `UNSUPPORTED`, `UNKNOWN`.

Lifecycle: `Planned -> Validated -> Prepared -> Transferring -> Verified -> Committed`, with
`Aborted` reachable from any pre-commit state and `Refused` as the terminal outcome of a
rejected plan. **Authority commits only at `Committed`**; a failure before that point cannot
create two authoritative executions. Committing moves authority: the source is put into
`Draining` and stops accepting new work.

A plan's identity covers what must happen and under whose authority. The lifecycle position and
the refusal explanations are deliberately excluded, so a plan keeps one identity while it
advances and while explanations are attached to it. Currency is defined by the generations the
plan actually depends on: coordinator epoch, active policy, both endpoint incarnations and
capability sets, and the workload revision.

## Evidence model

Every claim is backed by an `EvidenceRecord` carrying adapter identity, source description,
subject, payload digest, runtime and driver versions, observation time, sequence, freshness
budget, and a provenance class of `REAL`, `SYNTHETIC`, or `UNSUPPORTED`.

`SYNTHETIC` evidence never silently becomes `REAL`: the class is part of the record, part of
the capability-set digest, part of the decision fingerprint, and part of every rendering.

Freshness is decided from a process-local monotonic marker that is deliberately **not
persisted**. A record loaded from disk has no live monotonic reference, is marked
`requires_revalidation`, and can never be treated as current until a live adapter observation
replaces it. Freshness-changing capabilities (`health.*`, `memory.free_bytes`,
`queue.concurrent_streams`) are the only ones governed by the freshness budget; static device
properties are not aged out.

## Persistence and recovery

The store is a versioned container with a magic value, format version, payload length, SHA-256
payload digest, header CRC, footer magic, and a repeated length. Every field is validated
before the payload reaches a decoder: corruption, truncation, trailing garbage, and unsupported
versions are all refused with typed errors. Replacement is atomic (write to a sibling file,
flush, rename, with a backup fallback on platforms that cannot rename over an existing file).

On restart the coordinator:

- **advances the coordinator epoch** and the federation generation;
- restores durable membership conservatively — members that were live become `DEGRADED` and
  therefore accept no new work until a live agent revalidates them;
- marks every restored evidence record as requiring revalidation;
- drops decisions, which were bound to the previous epoch and generation;
- aborts every in-flight migration plan with a typed reason;
- keeps retired members retired.

Mutate-then-persist happens under the exclusive lock, so a reader can never observe state that
has not been durably published.

## Control plane

`haf_coordinator` owns federation authority. `haf_agent` represents an accelerator-bearing node
and advertises observed devices through adapters. `haf_cli` inspects and exercises the
federation. They are independent OS processes communicating over TCP loopback.

Wire format (little-endian):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `HAF1` |
| 4 | 2 | protocol version |
| 6 | 2 | message type |
| 8 | 2 | flags |
| 10 | 2 | reserved (must be zero) |
| 12 | 8 | sequence |
| 20 | 4 | payload length |
| 24 | 4 | CRC-32 (IEEE) over bytes 0..23 |
| 28 | N | payload |
| 28+N | 8 | FNV-1a 64 over bytes 0..27+N |

Messages: `HELLO`, `HELLO_ACK`, `ADVERTISE`, `ADVERTISE_ACK`, `QUERY`, `QUERY_RESPONSE`,
`EVALUATE`, `EVALUATE_RESPONSE`, `PLAN_MIGRATION`, `PLAN_MIGRATION_RESPONSE`, `TRANSITION`,
`TRANSITION_RESPONSE`, `REGISTER_WORKLOAD`, `REGISTER_WORKLOAD_ACK`, `HEARTBEAT`,
`HEARTBEAT_ACK`, `GOODBYE`, `ERROR`.

The control plane safely refuses truncated frames, oversized frames (declared or actual),
invalid enum values, malformed lengths, non-monotonic (replayed) sequences, unknown message
types, corrupted payloads, requests before the handshake, duplicate live boots, and stale
epochs. Framing faults close the connection rather than leaving it half-open.

## Build

Requirements: CMake 3.20+, a C++20 compiler, and (optionally) a CUDA toolkit.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Useful options:

| Option | Default | Effect |
| --- | --- | --- |
| `HAF_BUILD_TESTS` | `ON` | Build the eight test suites |
| `HAF_BUILD_EXAMPLES` | `ON` | Build the examples |
| `HAF_BUILD_BENCHMARKS` | `ON` | Build the benchmarks |
| `HAF_BUILD_APPS` | `ON` | Build coordinator, agent, and CLI |
| `HAF_ENABLE_CUDA` | `ON` | Build the real CUDA adapter when a toolkit is found |
| `HAF_CUDA_ARCHITECTURES` | `` (auto) | CUDA architectures to target. Empty queries the local GPU and falls back to the toolkit default. |
| `HAF_WARNINGS_AS_ERRORS` | `ON` | Treat first-party warnings as errors |
| `HAF_ENABLE_ASAN` | `OFF` | Build with AddressSanitizer |

Both `Release` and `Debug` build with **zero first-party warnings** under `/W4 /permissive-`
(MSVC) or `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` and friends (other
compilers).

## Test

```sh
ctest --test-dir build --output-on-failure     # or run the suites directly
build/tests/haf_unit_tests
build/tests/haf_multiprocess_tests --case=multiprocess::full_federation_lifecycle_proof
build/tests/haf_unit_tests --list              # stable suite::case names
```

Eight suites, each case addressable as `suite::case`, each emitting flushed progress markers
(`BEGIN`, `SETUP`, `CONNECT`, `NEGOTIATE`, `ADVERTISE`, `EVALUATE`, `PLAN`, `KILL`,
`RESTART`, `RECOVER`, `VERIFY`, `SHUTDOWN`, `PASS`). No test uses a timeout as a substitute for
correctness.

| Suite | Scope |
| --- | --- |
| `haf_unit_tests` | Core primitives, models, engines |
| `haf_property_tests` | Randomized capability sets, requirements, policies, generations, serialization, ordering (seeded, seed reported) |
| `haf_concurrency_tests` | Concurrent queries, capability updates, policy updates during reads, add/remove, invalidation, snapshot, shutdown, persistence visibility |
| `haf_adversarial_tests` | Malformed advertisements, impossible quantities, non-finite values, duplicate identities and live boots, stale authority of every kind, invalid transitions, frame and persistence corruption, huge counts, reconnect storms, device disappearance, tampered decisions |
| `haf_persistence_tests` | Save, reload, conservative recovery, revalidation, retirements, plan abortion, atomic replacement, foreign identity rejection |
| `haf_integration_tests` | End-to-end library flows, policy invalidation, migration commit, capability staleness, repeated recovery cycles |
| `haf_multiprocess_tests` | Real OS processes over real TCP: full lifecycle proof, kill/restart, stale-boot refusal, coordinator restart, protocol faults |
| `haf_cuda_tests` | Real CUDA discovery and real kernel execution where CUDA is present |

### Multiprocess proof

`multiprocess::full_federation_lifecycle_proof` performs, with genuine processes:

1. start the coordinator; 2. start Agent A; 3. Agent A advertises accelerators;
4. start Agent B with a different capability profile; 5. both are admitted and active;
6. submit workload requirements over the control plane; 7. verify hard eligibility filtering;
8. verify deterministic compatibility explanation and reproducible fingerprints across separate
CLI processes; 9. verify portability classification and migration planning;
10. kill Agent A's process; 11. prove its old incarnation no longer accepts work;
12. restart it with a fresh boot identity; 13. prove stale-boot traffic is refused with
`StaleBoot`; 14. restart the coordinator; 15. prove the epoch advances;
16. prove pre-restart authority is refused with `StaleEpoch`; 17. prove durable membership
recovers conservatively; 18. prove dynamic evidence requires refresh;
19. revalidate and restore active federation state; 20. shut down cleanly with no leaked child
processes and an independently verifiable store.

## Install and consume

```sh
cmake --install build --prefix /path/to/prefix
```

The install exports `HeterogeneousAcceleratorFederationConfig.cmake`, a version file, and
namespaced targets. A downstream consumer needs only:

```cmake
find_package(HeterogeneousAcceleratorFederation 1.0 REQUIRED)

add_executable(my_consumer main.cpp)
target_link_libraries(my_consumer PRIVATE
  HeterogeneousAcceleratorFederation::haf_core
  HeterogeneousAcceleratorFederation::haf_adapters)
```

```cpp
#include <haf/adapters/synthetic.hpp>
#include <haf/federation/federation.hpp>

int main() {
    haf::FederationConfig config;
    config.name = "consumer";
    config.persist = false;
    auto federation = haf::Federation::open(config);
    if (!federation.ok()) {
        return 1;
    }
    haf::adapters::AdapterContext context;
    context.node_token = "consumer-node";
    context.node = haf::node_id_from_token(context.node_token);
    auto descriptor = haf::adapters::build_descriptor(
        haf::adapters::cuda_class_profile(), context, "consumer-adapter", 0);
    if (!descriptor.ok()) {
        return 1;
    }
    auto joined = (*federation)->observe(*descriptor, (*federation)->epoch_claim("consumer"));
    return joined.ok() ? 0 : 1;
}
```

## Examples

| Example | Demonstrates |
| --- | --- |
| `example_federation_lifecycle` | Observe, admit, activate, and stale-authority refusal |
| `example_compatibility_query` | Eligibility, structured explanations, and `UNKNOWN` for unmodelled requirements |
| `example_capability_matrix` | Deterministic workload x accelerator matrix |
| `example_portability` | Every portability class with its reasons |
| `example_policy_invalidation` | Policy generation advance and decision invalidation |
| `example_migration_planning` | Plan outcomes, lifecycle transitions, stale refusal |
| `example_persistence_recovery` | Save, reopen, conservative recovery, revalidation |
| `example_cuda_member` | Real CUDA discovery and execution proof, or an explicitly labelled fallback |

## CLI

```sh
haf_coordinator --store federation.store --port 0 --seed 41
haf_agent --connect 127.0.0.1:PORT --profile all --seed 42
haf_cli --connect 127.0.0.1:PORT member list
```

| Command | Purpose |
| --- | --- |
| `federation show` | Identity, generations, store |
| `member list` / `member show ID` / `member explain ID` | Membership, record, full explanation with evidence and lifecycles |
| `capability list` | The federation capability vocabulary |
| `policy show` | The active policy |
| `workload list` / `workload evaluate ID --accelerator ID [--from ID]` | Registered profiles and eligibility |
| `compatibility matrix [--workload ID]` | Deterministic matrix with blocking requirements and generations |
| `portability explain --from ID --to ID` | Portability class with reasons |
| `migration plan ID --from ID --to ID` | Migration planning |
| `plan list` / `adapters` / `recovery` / `audit` | Plans, adapter availability, recovery report, invariant audit |
| `snapshot verify --store PATH` | Verify a durable store without a coordinator |
| `demo` | Self-contained federation demonstration |

`haf_cli demo` needs no coordinator and exercises the same public API a downstream consumer
would use.

## Benchmarks

`build/benchmarks/haf_bench` measures completed work at scales of 10, 100, 1,000, and 10,000.
Nothing asynchronous is reported as completed throughput.

Measured on the validation host (Release, single thread):

| Operation | 10 | 100 | 1,000 | 10,000 |
| --- | --- | --- | --- | --- |
| Admission (observe + admit + activate) | 0.11 ms | 1.07 ms | 13.2 ms | 135.7 ms |
| Capability normalization + digest | 0.35 ms | 3.3 ms | 33.4 ms | 350.5 ms |
| Compatibility sweep (4 workloads, O(NxM)) | 0.21 ms | 2.8 ms | 26.2 ms | 298.5 ms |
| Deterministic ranking | 0.03 ms | 0.10 ms | 0.71 ms | 4.9 ms |
| Evaluate + record decision | 0.07 ms | 0.81 ms | 9.9 ms | 98.6 ms |
| Snapshot creation | 0.04 ms | 0.84 ms | 10.4 ms | 102.6 ms |
| Invariant audit | 0.09 ms | 1.29 ms | 23.8 ms | 227.7 ms |
| Member list rendering | 0.05 ms | 0.84 ms | 11.0 ms | 133.5 ms |

Persistence is measured as one atomic replacement of the whole federation: save 6.2 ms / 45.2 ms
/ 476.7 ms and load+verify 4.8 ms / 13.4 ms / 124.1 ms at 100 / 1,000 / 10,000 members, with
recovery open at 13.9 ms / 141.0 ms / 1540.0 ms.

**Honest algorithmic note.** The default durability contract writes the whole federation before
every mutation becomes visible. That contract costs **O(N^2) persisted bytes** for N admissions:
measured at 56 ms for 10, 1.05 s for 100, and 20.9 s for 400 members. Bulk loaders can set
`FederationConfig::persist_on_mutation = false` and call `persist()` explicitly, which is what
the persistence benchmark does. The benchmark reports both so the trade-off is visible rather
than hidden. All other measured paths scale linearly in the dimensions shown.

## Real hardware validation

**What was physically present on the validation host:**

| Item | Value |
| --- | --- |
| Host | Windows, x64 |
| Toolchain | MSVC 19.44 (Visual Studio 2022 Build Tools 17.14.25), CMake 4.3.2, Ninja |
| Accelerator | NVIDIA GeForce RTX 5090, 34,162,016,256 bytes reported total memory, compute capability 12.0 |
| CUDA toolkit | 12.9 (nvcc V12.9.86) |
| CUDA runtime version reported by the runtime | 12.9.0 |
| CUDA driver API version reported by the runtime | 13.4.0 |
| AMD / ROCm hardware | none |
| Intel accelerator hardware | none |
| Second physical machine | none |

**REAL** — genuinely exercised on this host:

- Windows host and process behaviour, including real process kill and restart;
- TCP loopback control plane between independent OS processes;
- RTX 5090 CUDA discovery through the CUDA runtime on physical hardware;
- a real CUDA kernel executing on the device, verified against an independently computed CPU
  reference, with device memory released afterwards;
- durable persistence, integrity verification, atomic replacement, and conservative recovery;
- coordinator restart with epoch advancement and stale-epoch rejection.

**SYNTHETIC** — deterministic models, always labelled, never presented as hardware:

- the ROCm/AMD-class member profile (no AMD accelerator or ROCm runtime is present);
- the Intel/Level-Zero-class member profile (no Intel accelerator is present);
- the CUDA-class synthetic profile, used only when no real CUDA device is available;
- multi-node topology (only one machine is available; node identity is a reference, not a
  second host);
- cross-vendor portability and migration scenarios that cannot be executed without a second
  vendor's hardware.

**UNSUPPORTED** — cannot be truthfully implemented or proven here:

- real ROCm execution;
- physical multi-vendor accelerator coexistence;
- multi-GPU peer/NVLink behaviour beyond what a single device reports;
- RDMA and fabric-level collective execution;
- MIG or other partition lifecycle (partition mode is recorded only as observed capability);
- any cross-vendor live state migration. CUDA advertises
  `migration.live_state_transfer` as `UNSUPPORTED` from positive evidence of absence, so a
  workload requiring live transfer is ineligible rather than mis-placed.

## REAL, SYNTHETIC, UNSUPPORTED

The provenance class is never cosmetic. It appears in the evidence record, in the
capability-set digest, in the decision fingerprint, in `member list` and `member explain`,
in the adapter availability report, and in every example's output. Policy can reject synthetic
evidence outright, or require it for diagnostic federations, and a synthetic member is never
reported as `NATIVE`.

## Known limitations

- **Per-mutation durability is quadratic.** See the benchmark note above. The default is
  chosen for safety; deferred persistence is available and explicit.
- **The control plane is unauthenticated.** It is designed for loopback or a trusted network.
  Authority is bound to generations, not to cryptographic identities. Do not expose it to an
  untrusted network.
- **One coordinator per store.** There is no consensus protocol and no leader election; the
  coordinator is a single authority by construction.
- **Migration is planned, not executed.** The runtime produces and commits plans and moves
  authority. It does not transfer bytes: it has no access to workload state.
- **Ranking is deliberately narrow.** It orders eligible federation targets by portability,
  transformation cost, evidence provenance, and an optional caller-supplied locality measure.
  It is not a scheduler.
- **Freshness is bounded by the policy budget**, not by device-side health polling. A device
  that stops reporting is fenced by its session ending, not by an independent health check.
- **LeakSanitizer is unavailable with the MSVC AddressSanitizer**, so the sanitizer run covers
  memory errors but not leak detection.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

// Benchmarks for the CPU-side federation operations.
//
// Every measurement reports completed work: a result that has been fully
// computed and, where relevant, verified. Nothing is reported as throughput
// merely because it was submitted. Scales run 10 / 100 / 1,000 / 10,000 so that
// algorithmic behaviour is visible instead of inferred.
//
// The compatibility sweep is honestly O(N x M): it evaluates every workload
// against every accelerator. The benchmark states the dimensions rather than
// hiding them behind a "per second" figure.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "haf/adapters/synthetic.hpp"
#include "haf/engine/compatibility_engine.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/engine/ranking.hpp"
#include "haf/federation/federation.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/persist/file_store.hpp"

using namespace haf;

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
    std::string operation;
    std::string dimensions;
    double millis{0.0};
    std::uint64_t items{0};
    double items_per_second{0.0};
    std::string notes;
};

std::vector<Measurement> g_measurements;
bool g_checks_passed = true;

void record(const std::string& operation, const std::string& dimensions, double millis, std::uint64_t items,
            const std::string& notes) {
    Measurement measurement;
    measurement.operation = operation;
    measurement.dimensions = dimensions;
    measurement.millis = millis;
    measurement.items = items;
    measurement.items_per_second = millis > 0.0 ? (static_cast<double>(items) * 1000.0 / millis) : 0.0;
    measurement.notes = notes;
    // Print before the value is moved into the results table; printing a
    // moved-from object would silently lose the labels.
    std::printf("%-38s %-26s %10.3f ms %12llu items %14.1f items/s  %s\n", measurement.operation.c_str(),
                measurement.dimensions.c_str(), measurement.millis,
                static_cast<unsigned long long>(measurement.items), measurement.items_per_second,
                measurement.notes.c_str());
    std::fflush(stdout);
    g_measurements.push_back(std::move(measurement));
}

void check(bool condition, const std::string& what) {
    if (!condition) {
        g_checks_passed = false;
        std::printf("CHECK FAILED: %s\n", what.c_str());
    }
}

adapters::AdapterContext context(std::uint64_t seed) {
    adapters::AdapterContext result;
    IdGenerator generator = make_deterministic_id_generator(seed);
    result.agent = generator.next_id<AgentIdTag>();
    result.agent_boot = generator.next_id<AgentBootIdTag>();
    result.node_token = "bench-node";
    result.node = node_id_from_token(result.node_token);
    result.seed = seed;
    return result;
}

AcceleratorDescriptor make_device(std::uint64_t index, const adapters::DeviceProfile& base) {
    adapters::DeviceProfile profile = base;
    profile.memory_total_bytes = static_cast<std::int64_t>(8 + (index % 32)) * 1024LL * 1024LL * 1024LL;
    profile.memory_free_bytes = profile.memory_total_bytes / 2;
    profile.device_index = static_cast<std::int64_t>(index);
    const Result<AcceleratorDescriptor> built =
        adapters::build_descriptor(profile, context(1000 + index), "bench", static_cast<std::int64_t>(index));
    if (!built.ok()) {
        throw std::runtime_error("benchmark device construction failed: " + built.status().describe());
    }
    return *built;
}

WorkloadProfile make_workload(std::uint64_t index) {
    std::vector<CapabilityRequirement> requirements;
    Result<CapabilityRequirement> vendor =
        require_equals("vendor.id", index % 2 == 0 ? "nvidia" : "amd", RequirementStrength::Hard);
    Result<CapabilityRequirement> format = require_present("numeric.fp32", RequirementStrength::Hard);
    Result<CapabilityRequirement> isa =
        require_tokens_superset("isa.code_object_targets", {index % 2 == 0 ? "sm-120" : "gfx-942"},
                                RequirementStrength::Hard);
    Result<CapabilityRequirement> memory =
        require_at_least("memory.total_bytes", static_cast<std::int64_t>(4 + (index % 16)) * 1024LL * 1024LL * 1024LL,
                         RequirementStrength::Hard);
    Result<CapabilityRequirement> preference = require_present("memory.ecc_enabled", RequirementStrength::Soft);
    for (const Result<CapabilityRequirement>& requirement : {vendor, format, isa, memory, preference}) {
        if (!requirement.ok()) {
            throw std::runtime_error("benchmark requirement construction failed: " + requirement.status().describe());
        }
    }
    preference->weight = 0.5;
    WorkloadProfile profile;
    profile.name = "bench-workload-" + std::to_string(index);
    profile.class_id = workload_class_id_from_token(profile.name);
    profile.requirements = {*vendor, *format, *isa, *memory, *preference};
    profile.requirement_id = derive_requirement_id(profile);
    profile.revision = derive_workload_revision(profile);
    return profile;
}

/// Admit one accelerator through the full lifecycle: observe, admit, activate.
[[nodiscard]] bool admit_accelerator(Federation& federation, const AcceleratorDescriptor& device) {
    const AuthorityClaim claim = federation.epoch_claim("benchmark");
    Result<MemberRecord> observed = federation.observe(device, claim);
    if (!observed.ok()) {
        return false;
    }
    if (observed->state == MemberState::Observed) {
        Result<MemberRecord> admitted = federation.admit(device.id, claim);
        if (!admitted.ok()) {
            return false;
        }
        observed = admitted;
    }
    if (observed->state == MemberState::Admitted) {
        Result<MemberRecord> activated = federation.activate(device.id, claim);
        if (!activated.ok()) {
            return false;
        }
        observed = activated;
    }
    return observed->state == MemberState::Active;
}

EvaluationContext context_for(const AcceleratorDescriptor& target, const FederationPolicy& policy) {
    EvaluationContext evaluation;
    evaluation.federation = FederationId::from_raw(derive_identity("haf.bench.federation", "b"));
    evaluation.federation_generation = FederationGeneration(1);
    evaluation.epoch = CoordinatorEpoch(1);
    evaluation.policy_id = policy.id;
    evaluation.policy_generation = policy.generation;
    evaluation.policy = &policy;
    evaluation.workload_revision = WorkloadRevision(1);
    evaluation.now = now_monotonic();
    evaluation.target.accelerator = target.id;
    evaluation.target.device_generation = target.generation;
    evaluation.target.capability_generation = target.capabilities.generation();
    evaluation.target.evidence_generation = EvidenceGeneration(1);
    evaluation.target.support_level = target.support_level;
    evaluation.target.provenance = target.provenance;
    evaluation.target.capabilities = &target.capabilities;
    evaluation.target.evidence_fresh = true;
    evaluation.target.accepts_new_work = true;
    return evaluation;
}

std::vector<std::uint64_t> scales() { return {10, 100, 1000, 10000}; }

void benchmark_admission(std::uint64_t count) {
    FederationConfig config;
    config.name = "bench-admission";
    config.id_seed = 7;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    check(federation.ok(), "federation open");
    if (!federation.ok()) {
        return;
    }
    std::vector<AcceleratorDescriptor> devices;
    devices.reserve(count);
    for (std::uint64_t index = 0; index < count; ++index) {
        devices.push_back(make_device(index, adapters::cuda_class_profile()));
    }
    const auto start = Clock::now();
    std::uint64_t admitted = 0;
    for (const AcceleratorDescriptor& device : devices) {
        if (admit_accelerator(**federation, device)) {
            ++admitted;
        }
    }
    const auto finish = Clock::now();
    const double millis = std::chrono::duration<double, std::milli>(finish - start).count();
    record("admission (observe+activate)", "accelerators=" + std::to_string(count), millis, admitted,
           "completed admissions");
    check(admitted == count, "every accelerator was admitted");
    check((*federation)->member_count() == count, "member count matches");
}

void benchmark_capability_normalization(std::uint64_t count) {
    std::vector<CapabilitySet> sets;
    sets.reserve(count);
    for (std::uint64_t index = 0; index < count; ++index) {
        const AcceleratorDescriptor device = make_device(index, adapters::rocm_class_profile());
        sets.push_back(device.capabilities);
    }
    const auto start = Clock::now();
    std::uint64_t normalized = 0;
    std::uint64_t digest_accumulator = 0;
    for (std::size_t index = 0; index < sets.size(); ++index) {
        CapabilitySet working = sets[index];
        std::vector<CapabilityRecord> shuffled = working.records();
        std::reverse(shuffled.begin(), shuffled.end());
        if (working.set_records(shuffled).ok()) {
            // The digest is computed only after normalization completes, so the
            // measurement covers fully finished work.
            digest_accumulator += working.digest()[0];
            ++normalized;
        }
    }
    const auto finish = Clock::now();
    const double millis = std::chrono::duration<double, std::milli>(finish - start).count();
    record("capability normalization+digest", "capability_sets=" + std::to_string(count), millis, normalized,
           "normalized and hashed");
    check(normalized == count, "every capability set normalized");
    check(digest_accumulator != 0, "digests were computed");
}

void benchmark_compatibility(std::uint64_t devices, std::uint64_t workloads) {
    FederationPolicy policy = FederationPolicy::permissive_default();
    std::vector<AcceleratorDescriptor> fleet;
    fleet.reserve(devices);
    for (std::uint64_t index = 0; index < devices; ++index) {
        fleet.push_back(make_device(index, index % 2 == 0 ? adapters::cuda_class_profile()
                                                          : adapters::rocm_class_profile()));
    }
    std::vector<WorkloadProfile> profiles;
    profiles.reserve(workloads);
    for (std::uint64_t index = 0; index < workloads; ++index) {
        profiles.push_back(make_workload(index));
    }
    const auto start = Clock::now();
    std::uint64_t evaluated = 0;
    std::uint64_t eligible = 0;
    std::uint64_t fingerprint_accumulator = 0;
    for (const WorkloadProfile& workload : profiles) {
        for (const AcceleratorDescriptor& device : fleet) {
            const Result<CompatibilityDecision> decision =
                evaluate_compatibility(workload, context_for(device, policy));
            if (!decision.ok()) {
                continue;
            }
            ++evaluated;
            if (decision->outcome == CompatibilityOutcome::Eligible) {
                ++eligible;
            }
            fingerprint_accumulator += decision->fingerprint()[0];
        }
    }
    const auto finish = Clock::now();
    const double millis = std::chrono::duration<double, std::milli>(finish - start).count();
    record("compatibility sweep O(N*M)",
           "devices=" + std::to_string(devices) + " workloads=" + std::to_string(workloads), millis, evaluated,
           "eligible=" + std::to_string(eligible));
    check(evaluated == devices * workloads, "every pair evaluated");
    check(fingerprint_accumulator != 0, "fingerprints computed");
}

void benchmark_ranking(std::uint64_t devices) {
    FederationPolicy policy = FederationPolicy::permissive_default();
    std::vector<AcceleratorDescriptor> fleet;
    fleet.reserve(devices);
    for (std::uint64_t index = 0; index < devices; ++index) {
        fleet.push_back(make_device(index, adapters::cuda_class_profile()));
    }
    const WorkloadProfile workload = make_workload(0);
    std::vector<RankingCandidate> candidates;
    candidates.reserve(devices);
    for (const AcceleratorDescriptor& device : fleet) {
        const Result<CompatibilityDecision> decision =
            evaluate_compatibility(workload, context_for(device, policy));
        if (!decision.ok()) {
            continue;
        }
        RankingCandidate candidate;
        candidate.accelerator = device.id;
        candidate.decision = *decision;
        candidate.topology_distance = static_cast<std::uint32_t>(device.generation.value());
        candidates.push_back(std::move(candidate));
    }
    RankingWeights weights;
    weights.use_topology_distance = true;
    const auto start = Clock::now();
    const Result<RankingResult> ranked = rank_candidates(candidates, weights);
    const auto finish = Clock::now();
    const double millis = std::chrono::duration<double, std::milli>(finish - start).count();
    check(ranked.ok(), "ranking succeeded");
    if (!ranked.ok()) {
        return;
    }
    record("deterministic ranking", "candidates=" + std::to_string(candidates.size()), millis,
           ranked->ranked.size(), "ranked=" + std::to_string(ranked->ranked.size()));
    check(ranked->ranked.size() + ranked->excluded.size() == candidates.size(), "every candidate accounted for");
}

void benchmark_decisions_and_snapshot(std::uint64_t count) {
    FederationConfig config;
    config.name = "bench-decisions";
    config.id_seed = 8;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    check(federation.ok(), "federation open for decision benchmark");
    if (!federation.ok()) {
        return;
    }
    std::vector<AcceleratorId> ids;
    for (std::uint64_t index = 0; index < count; ++index) {
        AcceleratorDescriptor device = make_device(index, adapters::cuda_class_profile());
        if (admit_accelerator(**federation, device)) {
            ids.push_back(device.id);
        }
    }
    const WorkloadProfile workload = make_workload(0);
    if (!(*federation)
             ->register_workload(workload, (*federation)->epoch_claim("bench"))
             .ok()) {
        check(false, "workload registration");
        return;
    }
    {
        const auto start = Clock::now();
        std::uint64_t recorded = 0;
        for (const AcceleratorId& id : ids) {
            if ((*federation)->evaluate(workload.class_id, id).ok()) {
                ++recorded;
            }
        }
        const auto finish = Clock::now();
        record("evaluate + record decision", "accelerators=" + std::to_string(ids.size()),
               std::chrono::duration<double, std::milli>(finish - start).count(), recorded, "recorded decisions");
        check(recorded == ids.size(), "every decision recorded");
    }
    {
        const auto start = Clock::now();
        const std::size_t removed = (*federation)->invalidate_stale_decisions();
        const auto finish = Clock::now();
        record("stale-decision invalidation", "members=" + std::to_string(ids.size()),
               std::chrono::duration<double, std::milli>(finish - start).count(), ids.size(),
               "removed=" + std::to_string(removed));
    }
    {
        const auto start = Clock::now();
        const FederationSnapshot snapshot = (*federation)->snapshot();
        const auto finish = Clock::now();
        record("snapshot creation", "members=" + std::to_string(snapshot.members.size()),
               std::chrono::duration<double, std::milli>(finish - start).count(), snapshot.members.size(),
               "digest=" + snapshot.digest_hex().substr(0, 16));
        check(!snapshot.federation.is_nil(), "snapshot has an identity");
    }
    {
        const auto start = Clock::now();
        const AuditReport report = (*federation)->audit();
        const auto finish = Clock::now();
        record("invariant audit", "members=" + std::to_string(ids.size()),
               std::chrono::duration<double, std::milli>(finish - start).count(), report.checks_run,
               "violations=" + std::to_string(report.violations.size()));
        check(report.ok(), "audit is clean");
    }
}

void benchmark_persistence_and_recovery(std::uint64_t count) {
    const std::filesystem::path store =
        std::filesystem::temp_directory_path() / ("haf-bench-" + std::to_string(count) + ".store");
    std::error_code error;
    std::filesystem::remove(store, error);

    double save_millis = 0.0;
    double load_millis = 0.0;
    double recovery_millis = 0.0;
    std::size_t members = 0;
    {
        FederationConfig config;
        config.name = "bench-persistence";
        config.id_seed = 9;
        config.store_path = store;
        // Bulk load with an explicit durability period, then measure one save of
        // the whole federation. Per-mutation persistence is measured separately
        // and honestly reported as O(N^2) in persisted bytes.
        config.persist_on_mutation = false;
        Result<std::unique_ptr<Federation>> federation = Federation::open(config);
        check(federation.ok(), "persistent federation open");
        if (!federation.ok()) {
            return;
        }
        for (std::uint64_t index = 0; index < count; ++index) {
            AcceleratorDescriptor device = make_device(index, adapters::rocm_class_profile());
            static_cast<void>(admit_accelerator(**federation, device));
        }
        members = (*federation)->member_count();
        const auto start = Clock::now();
        const VoidResult persisted = (*federation)->persist();
        const auto finish = Clock::now();
        save_millis = std::chrono::duration<double, std::milli>(finish - start).count();
        check(persisted.ok(), "explicit persist succeeded");
    }
    {
        const auto start = Clock::now();
        const Result<ByteBuffer> payload = FileStore(store).read();
        const auto finish = Clock::now();
        load_millis = std::chrono::duration<double, std::milli>(finish - start).count();
        check(payload.ok(), "store read succeeded");
        if (payload.ok()) {
            const Result<FederationSnapshot> decoded = decode_snapshot(*payload);
            check(decoded.ok(), "store decoded");
            if (decoded.ok()) {
                check(decoded->members.size() == members, "decoded member count matches");
            }
        }
    }
    {
        FederationConfig config;
        config.name = "bench-persistence";
        config.id_seed = 9;
        config.store_path = store;
        const auto start = Clock::now();
        Result<std::unique_ptr<Federation>> recovered = Federation::open(config);
        const auto finish = Clock::now();
        recovery_millis = std::chrono::duration<double, std::milli>(finish - start).count();
        check(recovered.ok(), "recovery succeeded");
        if (recovered.ok()) {
            check((*recovered)->recovery_report().recovered, "recovery was reported");
            check((*recovered)->audit().ok(), "recovered federation audits clean");
        }
    }
    record("persistence save", "members=" + std::to_string(members), save_millis, members,
           "one atomic replace of the whole federation");
    record("persistence load+verify", "members=" + std::to_string(members), load_millis, members,
           "integrity checked");
    record("recovery open", "members=" + std::to_string(members), recovery_millis, members,
           "authority advanced");
    std::filesystem::remove(store, error);
}

/// Measures the default durability contract: every accepted mutation is on
/// disk before it is observable. The cost of that contract is quadratic in the
/// number of mutations because each one rewrites the whole federation, which is
/// exactly why the benchmark reports it rather than hiding it.
void benchmark_durable_per_mutation(std::uint64_t count) {
    const std::filesystem::path store =
        std::filesystem::temp_directory_path() / ("haf-bench-durable-" + std::to_string(count) + ".store");
    std::error_code error;
    std::filesystem::remove(store, error);
    FederationConfig config;
    config.name = "bench-durable";
    config.id_seed = 11;
    config.store_path = store;
    config.persist_on_mutation = true;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    check(federation.ok(), "durable federation open");
    if (!federation.ok()) {
        return;
    }
    const auto start = Clock::now();
    std::uint64_t admitted = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        AcceleratorDescriptor device = make_device(index, adapters::cuda_class_profile());
        if (admit_accelerator(**federation, device)) {
            ++admitted;
        }
    }
    const auto finish = Clock::now();
    record("durable admission (per mutation)", "accelerators=" + std::to_string(count),
           std::chrono::duration<double, std::milli>(finish - start).count(), admitted,
           "every mutation durable, O(N^2) bytes");
    check(admitted == count, "every accelerator was durably admitted");
    std::filesystem::remove(store, error);
}

void benchmark_inspection(std::uint64_t count) {
    FederationConfig config;
    config.name = "bench-inspection";
    config.id_seed = 10;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    check(federation.ok(), "inspection federation open");
    if (!federation.ok()) {
        return;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        AcceleratorDescriptor device = make_device(index, adapters::intel_class_profile());
        static_cast<void>(admit_accelerator(**federation, device));
    }
    std::string rendering;
    const std::size_t before = rendering.size();
    static_cast<void>(before);
    const auto start = Clock::now();
    const Result<std::string> text = (*federation)->render_members();
    const auto finish = Clock::now();
    check(text.ok(), "member rendering succeeded");
    if (text.ok()) {
        record("member list rendering", "members=" + std::to_string(count),
               std::chrono::duration<double, std::milli>(finish - start).count(), count,
               "bytes=" + std::to_string(text->size()));
        check(text->size() > count, "rendering contains one line per member");
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--quick") {
            quick = true;
        }
    }
    std::printf("Heterogeneous Accelerator Federation benchmarks\n");
    std::printf("All figures are completed work, measured on the calling thread.\n\n");

    const std::vector<std::uint64_t> sizes = quick ? std::vector<std::uint64_t>{10, 100}
                                                   : std::vector<std::uint64_t>{10, 100, 1000, 10000};
    for (const std::uint64_t size : sizes) {
        benchmark_admission(size);
    }
    for (const std::uint64_t size : sizes) {
        benchmark_capability_normalization(size);
    }
    for (const std::uint64_t size : sizes) {
        benchmark_compatibility(size, 4);
    }
    for (const std::uint64_t size : sizes) {
        benchmark_ranking(size);
    }
    for (const std::uint64_t size : sizes) {
        benchmark_decisions_and_snapshot(size);
    }
    for (const std::uint64_t size : (quick ? std::vector<std::uint64_t>{100}
                                           : std::vector<std::uint64_t>{100, 1000, 10000})) {
        benchmark_persistence_and_recovery(size);
    }
    for (const std::uint64_t size : (quick ? std::vector<std::uint64_t>{10, 100}
                                           : std::vector<std::uint64_t>{10, 100, 400})) {
        benchmark_durable_per_mutation(size);
    }
    for (const std::uint64_t size : sizes) {
        benchmark_inspection(size);
    }

    std::printf("\n%s\n", g_checks_passed ? "BENCHMARK CHECKS: PASS" : "BENCHMARK CHECKS: FAIL");
    return g_checks_passed ? 0 : 1;
}

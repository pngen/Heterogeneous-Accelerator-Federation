// Concurrency contracts. These cases exercise the documented threading model:
// every public Federation operation is safe to call concurrently, no callback
// runs under the internal lock, and a mutate-then-persist operation is atomic
// with respect to readers.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/federation/federation.hpp"
#include "haf/model/taxonomy.hpp"
#include "support/environment.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

namespace {

constexpr int kThreads = 8;
constexpr int kIterations = 60;

}  // namespace

HAF_TEST(concurrency, concurrent_queries_observe_a_consistent_federation) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(100, permissive_policy());
    HAF_REQUIRE_OK(federation);
    std::vector<AcceleratorDescriptor> devices;
    for (int index = 0; index < 8; ++index) {
        AcceleratorDescriptor device =
            make_device(adapters::cuda_class_profile(), 200, "concurrent", index);
        HAF_REQUIRE_OK(join(**federation, device));
        devices.push_back(std::move(device));
    }
    const WorkloadProfile workload = make_workload("concurrent-query", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("test")));

    std::atomic<int> failures{0};
    std::atomic<int> success{0};
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&, thread_index]() {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const AcceleratorDescriptor& device =
                    devices[static_cast<std::size_t>((thread_index + iteration) % devices.size())];
                const Result<CompatibilityDecision> decision =
                    (*federation)->evaluate_transient(workload.class_id, device.id);
                if (!decision.ok() || decision->outcome != CompatibilityOutcome::Eligible) {
                    failures.fetch_add(1);
                } else {
                    success.fetch_add(1);
                }
                const AuditReport report = (*federation)->audit();
                if (!report.ok()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    HAF_EQ(failures.load(), 0);
    HAF_EQ(success.load(), kThreads * kIterations);
}

HAF_TEST(concurrency, concurrent_capability_updates_serialize_correctly) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(101, permissive_policy());
    HAF_REQUIRE_OK(federation);
    AcceleratorDescriptor device = make_cuda_like(300);
    HAF_REQUIRE_OK(join(**federation, device));

    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&, thread_index]() {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                AcceleratorDescriptor updated = device;
                adapters::DeviceProfile profile = adapters::cuda_class_profile();
                profile.memory_free_bytes = (thread_index + 1) * 1024LL * 1024LL * (iteration + 1);
                const Result<CapabilitySet> capabilities = adapters::build_capability_set(profile);
                if (!capabilities.ok()) {
                    rejected.fetch_add(1);
                    continue;
                }
                const Result<MemberRecord> current = (*federation)->member(device.id);
                if (!current.ok()) {
                    rejected.fetch_add(1);
                    continue;
                }
                const AuthorityClaim claim = (*federation)->epoch_claim("concurrent capability update");
                EvidenceRecord evidence = make_evidence("concurrent", "thread update", "1.0.0", device.id,
                                                        device.physical_device, EvidenceProvenance::Synthetic,
                                                        EvidenceKind::Declaration, capabilities->digest(),
                                                        RuntimeVersion{1, 0, 0}, RuntimeVersion{1, 0, 0}, 0, false);
                evidence.capability_generation = current->capability_generation;
                const Result<MemberRecord> updated_member =
                    (*federation)->update_capabilities(device.id, *capabilities, evidence, claim);
                if (updated_member.ok()) {
                    accepted.fetch_add(1);
                } else {
                    rejected.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    // Every update either succeeded or was rejected with a typed error; the
    // federation never observed a partially applied update.
    HAF_EQ(accepted.load() + rejected.load(), kThreads * kIterations);
    HAF_CHECK(accepted.load() > 0);
    const AuditReport report = (*federation)->audit();
    HAF_CHECK(report.ok());
}

HAF_TEST(concurrency, policy_updates_during_reads_never_expose_a_partial_policy) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(102, permissive_policy());
    HAF_REQUIRE_OK(federation);
    HAF_REQUIRE_OK(join(**federation, make_cuda_like(400)));
    const WorkloadProfile workload = make_workload("policy-race", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("test")));

    std::atomic<bool> stop{false};
    std::atomic<int> read_failures{0};
    std::vector<std::thread> readers;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                // Each call returns a complete, internally consistent value. The
                // runtime does not promise that two separate calls observe the
                // same generation; callers that need that take a snapshot.
                const FederationPolicy policy = (*federation)->policy();
                if (!policy.validate().ok()) {
                    read_failures.fetch_add(1);
                }
                FederationPolicy rederived = policy;
                rederived.refresh_identity();
                if (rederived.id != policy.id || rederived.generation != policy.generation) {
                    read_failures.fetch_add(1);
                }
                const FederationSnapshot snapshot = (*federation)->snapshot();
                if (snapshot.policy != snapshot.policy_data.id ||
                    snapshot.policy_generation != snapshot.policy_data.generation) {
                    read_failures.fetch_add(1);
                }
            }
        });
    }
    for (int iteration = 0; iteration < 40; ++iteration) {
        FederationPolicy policy = permissive_policy();
        policy.name = "policy-" + std::to_string(iteration);
        policy.minimum_memory_bytes = static_cast<std::uint64_t>(iteration) * 1024;
        policy.refresh_identity();
        const Result<PolicyGeneration> applied =
            (*federation)->set_policy(policy, (*federation)->epoch_claim("policy race"));
        if (!applied.ok()) {
            read_failures.fetch_add(1);
        }
    }
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }
    HAF_EQ(read_failures.load(), 0);
}

HAF_TEST(concurrency, member_add_and_remove_is_safe_under_readers) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(103, permissive_policy());
    HAF_REQUIRE_OK(federation);
    std::atomic<bool> stop{false};
    std::atomic<int> reader_failures{0};
    std::vector<std::thread> readers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                const std::vector<MemberRecord> members = (*federation)->members();
                for (const MemberRecord& record : members) {
                    if (record.accelerator.is_nil()) {
                        reader_failures.fetch_add(1);
                    }
                }
                static_cast<void>((*federation)->snapshot());
            }
        });
    }
    for (int index = 0; index < 24; ++index) {
        AcceleratorDescriptor device = make_device(adapters::rocm_class_profile(), 500, "cycle", index);
        const Result<MemberRecord> joined = join(**federation, device);
        if (!joined.ok()) {
            reader_failures.fetch_add(1);
            continue;
        }
        const Result<MemberRecord> retired =
            (*federation)->retire(device.id, (*federation)->epoch_claim("cycle retire"));
        if (!retired.ok()) {
            reader_failures.fetch_add(1);
        }
    }
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }
    HAF_EQ(reader_failures.load(), 0);
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(concurrency, decision_invalidation_is_safe_under_concurrent_evaluation) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(104, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(600);
    HAF_REQUIRE_OK(join(**federation, device));
    const WorkloadProfile workload = make_workload("invalidate", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("test")));

    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::thread evaluator([&]() {
        while (!stop.load()) {
            if (!(*federation)->evaluate(workload.class_id, device.id).ok()) {
                failures.fetch_add(1);
            }
        }
    });
    std::thread invalidator([&]() {
        for (int iteration = 0; iteration < 200; ++iteration) {
            static_cast<void>((*federation)->invalidate_stale_decisions());
            static_cast<void>((*federation)->decisions().size());
        }
    });
    invalidator.join();
    stop.store(true);
    evaluator.join();
    HAF_EQ(failures.load(), 0);
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(concurrency, event_sink_is_invoked_without_holding_the_internal_lock) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(105, permissive_policy());
    HAF_REQUIRE_OK(federation);
    std::atomic<int> events{0};
    std::atomic<int> reentrant_failures{0};
    (*federation)->set_event_sink([&](const FederationEvent& event) {
        events.fetch_add(1);
        // Re-entering the federation from a sink must not deadlock. This is the
        // property that forbids dispatching events while the lock is held.
        if (!(*federation)->members().empty() && event.kind == EventKind::MemberObserved) {
            if (!(*federation)->audit().ok()) {
                reentrant_failures.fetch_add(1);
            }
        }
    });
    HAF_REQUIRE_OK(join(**federation, make_cuda_like(700)));
    HAF_CHECK(events.load() > 0);
    HAF_EQ(reentrant_failures.load(), 0);
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(concurrency, event_sink_may_replace_itself_and_may_throw) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(107, permissive_policy());
    HAF_REQUIRE_OK(federation);
    std::atomic<int> delivered{0};
    std::atomic<int> after_replacement{0};
    // A sink that installs a different sink and then throws: the runtime must
    // contain the exception, keep the federation usable, and never hold a lock
    // across the callback.
    (*federation)->set_event_sink([&](const FederationEvent&) {
        delivered.fetch_add(1);
        (*federation)->set_event_sink([&](const FederationEvent&) { after_replacement.fetch_add(1); });
        throw std::runtime_error("sink failure");
    });
    const Result<MemberRecord> first = join(**federation, make_cuda_like(1200));
    HAF_CHECK(first.ok());
    HAF_CHECK(delivered.load() >= 1);
    // The replacement sink is now active without holding any lock.
    const Result<MemberRecord> second = join(**federation, make_rocm_like(1201));
    HAF_CHECK(second.ok());
    HAF_CHECK(after_replacement.load() >= 1);
    HAF_CHECK((*federation)->audit().ok());

    // Clearing the sink from inside a callback is also safe.
    (*federation)->set_event_sink([&](const FederationEvent&) { (*federation)->set_event_sink(nullptr); });
    HAF_CHECK(join(**federation, make_intel_like(1202)).ok());
    HAF_CHECK((*federation)->audit().ok());
    HAF_CHECK((*federation)->shutdown().ok());
}

HAF_TEST(concurrency, shutdown_is_idempotent_and_revokes_authority) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(106, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(800);
    HAF_REQUIRE_OK(join(**federation, device));

    std::vector<std::thread> workers;
    std::atomic<int> rejected{0};
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        workers.emplace_back([&]() {
            for (int iteration = 0; iteration < 20; ++iteration) {
                const Result<MemberRecord> fenced =
                    (*federation)->fence(device.id, (*federation)->epoch_claim("shutdown race"));
                if (!fenced.ok()) {
                    rejected.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    HAF_CHECK((*federation)->shutdown().ok());
    HAF_CHECK((*federation)->shutdown().ok());
    const Result<MemberRecord> after = (*federation)->fence(device.id, (*federation)->epoch_claim("after shutdown"));
    HAF_CHECK(!after.ok());
    HAF_EQ(static_cast<int>(after.status().code()), static_cast<int>(ErrorCode::ShutdownInProgress));
    // Reads still work after shutdown and the state is left auditable.
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(concurrency, persistence_is_atomic_with_respect_to_readers) {
    ScratchDirectory scratch("concurrency-persistence");
    const std::filesystem::path store = scratch.file("federation.store");
    FederationConfig config;
    config.name = "persistent";
    config.id_seed = 107;
    config.store_path = store;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    HAF_REQUIRE_OK(federation);

    std::atomic<bool> stop{false};
    std::atomic<int> reader_failures{0};
    std::thread reader([&]() {
        while (!stop.load()) {
            const FederationSnapshot snapshot = (*federation)->snapshot();
            if (snapshot.federation.is_nil()) {
                reader_failures.fetch_add(1);
            }
        }
    });
    for (int index = 0; index < 16; ++index) {
        const AcceleratorDescriptor device = make_device(adapters::cuda_class_profile(), 900, "persist", index);
        const Result<MemberRecord> joined = join(**federation, device);
        if (!joined.ok()) {
            reader_failures.fetch_add(1);
        }
    }
    stop.store(true);
    reader.join();
    HAF_EQ(reader_failures.load(), 0);
    HAF_CHECK((*federation)->audit().ok());
}

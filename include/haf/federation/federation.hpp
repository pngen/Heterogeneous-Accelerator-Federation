// Heterogeneous Accelerator Federation - the federation runtime.
//
// Ownership and threading model
// ----------------------------
//  * A Federation instance owns all authoritative state.
//  * All public methods are safe to call concurrently from any thread.
//  * Internally a single shared_mutex guards the whole authoritative state.
//    This is deliberate: federation state is small, mutations are rare, and a
//    single lock removes every lock-ordering hazard. Callers that need
//    read-modify-write atomicity use the explicit transaction helpers rather
//    than composing public calls.
//  * No lock is ever held while a callback runs. Events are collected under the
//    lock and dispatched after it is released.
//  * Mutate-then-persist is performed under the exclusive lock, so a reader can
//    never observe state that has not been durably published.
//  * The destructor never joins a thread that needs the lock: the runtime owns
//    no worker threads.

#ifndef HAF_FEDERATION_FEDERATION_HPP
#define HAF_FEDERATION_FEDERATION_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "haf/core/idgen.hpp"
#include "haf/core/limits.hpp"
#include "haf/engine/audit.hpp"
#include "haf/engine/compatibility_engine.hpp"
#include "haf/engine/migration_planner.hpp"
#include "haf/engine/ranking.hpp"
#include "haf/federation/authority.hpp"
#include "haf/federation/events.hpp"
#include "haf/federation/snapshot.hpp"
#include "haf/persist/store.hpp"

namespace haf {

struct FederationConfig {
    /// Nil means "generate a fresh federation identity".
    FederationId id{};
    std::string name{"federation"};
    FederationPolicy policy{};
    /// Empty means the federation is process-local and non-durable.
    std::filesystem::path store_path{};
    /// Zero means "seed the identity generator from OS entropy".
    std::uint64_t id_seed{0};
    /// When false the federation never writes to the store.
    bool persist{true};
    /// When true (the default) every accepted mutation is written to the store
    /// before it becomes visible, which makes durability-per-mutation the
    /// default contract. Bulk loaders may set this to false and call persist()
    /// explicitly; the trade-off is that a crash between mutations loses them,
    /// so a durable-period contract replaces the per-mutation one.
    bool persist_on_mutation{true};
    /// Bound on the number of recorded decisions retained in memory and on disk.
    std::size_t max_recorded_decisions{100000};
};

/// Options for workload evaluation.
struct EvaluationOptions {
    /// Optional origin accelerator. When set, portability is classified and the
    /// workload's portability/migration requirements are enforced.
    std::optional<AcceleratorId> source;
    /// Ranking weights used by rank().
    RankingWeights ranking{};
};

/// Outcome of a conservative recovery.
struct RecoveryReport {
    bool recovered{false};
    CoordinatorEpoch previous_epoch{};
    CoordinatorEpoch new_epoch{};
    FederationGeneration new_generation{};
    std::size_t members_restored{0};
    std::size_t members_requiring_revalidation{0};
    std::size_t decisions_invalidated{0};
    std::size_t plans_aborted{0};
    std::size_t retired_members{0};
    std::string detail;
};

class Federation {
public:
    struct MemberEntry {
        MemberRecord record;
        AcceleratorDescriptor descriptor;
    };

    /// Open (or create) a federation. When a store path is configured and the
    /// store already holds a valid snapshot for the same federation identity,
    /// the state is conservatively recovered and authority is advanced.
    [[nodiscard]] static Result<std::unique_ptr<Federation>> open(const FederationConfig& config);

    ~Federation();
    Federation(const Federation&) = delete;
    Federation& operator=(const Federation&) = delete;

    // --- Identity and authority inspection --------------------------------
    [[nodiscard]] FederationId id() const;
    [[nodiscard]] FederationGeneration generation() const;
    [[nodiscard]] CoordinatorEpoch epoch() const;
    [[nodiscard]] std::string name() const;
    [[nodiscard]] PolicyGeneration policy_generation() const;
    [[nodiscard]] FederationPolicy policy() const;
    [[nodiscard]] AuthorityClaim epoch_claim(std::string purpose) const;
    [[nodiscard]] std::string store_location() const;
    [[nodiscard]] const RecoveryReport& recovery_report() const noexcept { return recovery_; }

    // --- Negotiation and membership ---------------------------------------
    /// Validate an advertisement and create or refresh the member. Rejects
    /// malformed advertisements, duplicate live identities, stale boots, and
    /// devices whose incarnation identity does not match their content.
    [[nodiscard]] Result<MemberRecord> observe(const AcceleratorDescriptor& descriptor,
                                               const AuthorityClaim& claim);
    [[nodiscard]] Result<MemberRecord> admit(const AcceleratorId& accelerator, const AuthorityClaim& claim);
    [[nodiscard]] Result<MemberRecord> activate(const AcceleratorId& accelerator, const AuthorityClaim& claim);
    [[nodiscard]] Result<MemberRecord> transition(const AcceleratorId& accelerator, MemberState target,
                                                  const AuthorityClaim& claim);
    [[nodiscard]] Result<MemberRecord> update_capabilities(const AcceleratorId& accelerator,
                                                           const CapabilitySet& capabilities,
                                                           const EvidenceRecord& evidence,
                                                           const AuthorityClaim& claim);
    [[nodiscard]] Result<MemberRecord> fence(const AcceleratorId& accelerator, const AuthorityClaim& claim);
    [[nodiscard]] Result<MemberRecord> reopen(const AcceleratorId& accelerator, const AuthorityClaim& claim);
    [[nodiscard]] Result<MemberRecord> retire(const AcceleratorId& accelerator, const AuthorityClaim& claim);

    // --- Queries -----------------------------------------------------------
    [[nodiscard]] Result<MemberRecord> member(const AcceleratorId& accelerator) const;
    [[nodiscard]] Result<AcceleratorDescriptor> descriptor(const AcceleratorId& accelerator) const;
    [[nodiscard]] std::vector<MemberRecord> members() const;
    [[nodiscard]] std::vector<AcceleratorId> member_ids() const;
    [[nodiscard]] std::size_t member_count() const;

    // --- Policy ------------------------------------------------------------
    [[nodiscard]] Result<PolicyGeneration> set_policy(const FederationPolicy& policy, const AuthorityClaim& claim);

    // --- Workloads ---------------------------------------------------------
    [[nodiscard]] Result<WorkloadRevision> register_workload(const WorkloadProfile& profile,
                                                             const AuthorityClaim& claim);
    [[nodiscard]] Result<WorkloadProfile> workload(const WorkloadClassId& workload) const;
    [[nodiscard]] std::vector<WorkloadProfile> workloads() const;
    [[nodiscard]] std::vector<WorkloadClassId> workload_ids() const;

    // --- Decisions ---------------------------------------------------------
    /// Evaluate one workload against one accelerator and record the decision.
    [[nodiscard]] Result<CompatibilityDecision> evaluate(const WorkloadClassId& workload,
                                                         const AcceleratorId& accelerator,
                                                         const EvaluationOptions& options = {});
    /// Evaluate without recording. Used by matrix and benchmark paths so that a
    /// large sweep cannot grow durable state.
    [[nodiscard]] Result<CompatibilityDecision> evaluate_transient(const WorkloadClassId& workload,
                                                                   const AcceleratorId& accelerator,
                                                                   const EvaluationOptions& options = {}) const;
    [[nodiscard]] Result<FleetEvaluation> evaluate_fleet(const WorkloadClassId& workload,
                                                         const EvaluationOptions& options = {}) const;
    [[nodiscard]] Result<RankingResult> rank(const WorkloadClassId& workload,
                                             const RankingWeights& weights = {}) const;
    [[nodiscard]] Result<CompatibilityDecision> decision(const DecisionId& id) const;
    [[nodiscard]] std::vector<CompatibilityDecision> decisions() const;
    [[nodiscard]] std::size_t invalidate_stale_decisions();
    [[nodiscard]] Result<bool> verify_decision(const DecisionId& id) const;
    /// True when the recorded decision is still bound to current generations.
    [[nodiscard]] Result<bool> decision_is_current(const DecisionId& id) const;

    // --- Migration ---------------------------------------------------------
    [[nodiscard]] Result<MigrationPlan> plan_migration(const WorkloadClassId& workload,
                                                       const AcceleratorId& source,
                                                       const AcceleratorId& destination,
                                                       const AuthorityClaim& claim);
    [[nodiscard]] Result<MigrationPlan> migration_plan(const MigrationPlanId& id) const;
    [[nodiscard]] std::vector<MigrationPlan> migration_plans() const;
    [[nodiscard]] Result<MigrationPlan> advance_migration(const MigrationPlanId& id, MigrationState target,
                                                          const AuthorityClaim& claim);
    [[nodiscard]] Result<MigrationPlan> commit_migration(const MigrationPlanId& id, const AuthorityClaim& claim);
    [[nodiscard]] Result<MigrationPlan> abort_migration(const MigrationPlanId& id, std::string reason,
                                                        const AuthorityClaim& claim);
    [[nodiscard]] Result<bool> migration_plan_is_current(const MigrationPlanId& id) const;

    // --- Snapshot, persistence, audit -------------------------------------
    [[nodiscard]] FederationSnapshot snapshot() const;
    [[nodiscard]] VoidResult persist();
    [[nodiscard]] AuditReport audit() const;
    [[nodiscard]] Result<std::string> render_members() const;
    [[nodiscard]] Result<std::string> explain_member(const AcceleratorId& accelerator) const;

    /// Install an event sink. Replaces any previous sink.
    void set_event_sink(EventSink sink);

    /// Revoke authority: after shutdown no mutation is accepted, reads still
    /// work, and the durable state is left valid. Idempotent.
    [[nodiscard]] VoidResult shutdown();

private:
    explicit Federation(FederationConfig config);

    [[nodiscard]] Status validate_authority_locked(const AuthorityClaim& claim) const;
    [[nodiscard]] Status validate_authority_locked(const AuthorityClaim& claim, const MemberEntry& entry,
                                                   const AcceleratorId& accelerator) const;
    [[nodiscard]] Status restore_locked();
    [[nodiscard]] VoidResult persist_locked();
    [[nodiscard]] FederationSnapshot snapshot_locked() const;
    [[nodiscard]] Result<CompatibilityDecision> evaluate_locked(const WorkloadProfile& profile,
                                                               const AcceleratorId& accelerator,
                                                               const EvaluationOptions& options,
                                                               bool source_must_exist) const;
    void dispatch(const std::vector<FederationEvent>& events);
    [[nodiscard]] FederationEvent make_event(EventKind kind, std::string detail,
                                             const AcceleratorId& accelerator = {});

    mutable std::shared_mutex mutex_;
    FederationId id_{};
    std::string name_;
    FederationGeneration generation_{};
    CoordinatorEpoch epoch_{};
    FederationPolicy policy_{};
    bool shutdown_{false};
    bool persist_enabled_{true};
    bool persist_on_mutation_{true};
    /// Set only by persist(), so that an explicit save publishes even in
    /// deferred mode without a second code path.
    bool force_persist_{false};
    std::size_t max_recorded_decisions_{100000};
    std::map<AcceleratorId, MemberEntry> members_;
    /// Incarnations grouped by physical device. A device normally has one live
    /// incarnation plus any number of superseded ones, so this index keeps the
    /// duplicate-incarnation and supersede checks logarithmic instead of
    /// scanning the whole federation on every advertisement.
    std::map<PhysicalDeviceId, std::vector<AcceleratorId>> physical_index_;
    std::map<WorkloadClassId, WorkloadProfile> workloads_;
    std::map<DecisionId, CompatibilityDecision> decisions_;
    std::map<MigrationPlanId, MigrationPlan> plans_;
    std::unique_ptr<FederationStore> store_;
    IdGenerator id_generator_;
    EventSink event_sink_;
    RecoveryReport recovery_;
};

}  // namespace haf

#endif  // HAF_FEDERATION_FEDERATION_HPP

#include "haf/federation/federation.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/hash.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/persist/file_store.hpp"

namespace haf {
namespace {

[[nodiscard]] bool is_live_state(MemberState state) {
    return state == MemberState::Admitted || state == MemberState::Active || state == MemberState::Degraded ||
           state == MemberState::Draining;
}

/// States in which a member genuinely holds federation authority. Only these
/// block a new incarnation of the same physical device. A member that is
/// fenced, degraded, draining-complete, or merely observed does not hold
/// authority that a fresh boot could conflict with, so a new incarnation
/// supersedes it (and retires it) instead of being rejected.
[[nodiscard]] bool holds_competing_authority(MemberState state) {
    return state == MemberState::Admitted || state == MemberState::Active || state == MemberState::Draining;
}

[[nodiscard]] std::string transition_text(MemberState from, MemberState to) {
    return std::string(to_string(from)) + "->" + std::string(to_string(to));
}

[[nodiscard]] std::string migration_transition_text(MigrationState from, MigrationState to) {
    return std::string(to_string(from)) + "->" + std::string(to_string(to));
}

}  // namespace

Federation::Federation(FederationConfig config)
    : name_(std::move(config.name)),
      max_recorded_decisions_(config.max_recorded_decisions == 0 ? 1 : config.max_recorded_decisions),
      id_generator_(config.id_seed == 0 ? make_entropy_id_generator()
                                        : make_deterministic_id_generator(config.id_seed)) {
    id_ = config.id.is_nil() ? id_generator_.next_id<FederationIdTag>() : config.id;
    policy_ = config.policy;
    if (policy_.id.is_nil()) {
        policy_ = FederationPolicy::permissive_default();
    }
    // Epoch and generation start at one so that the initial value is never
    // mistaken for "no authority asserted".
    generation_ = FederationGeneration(1);
    epoch_ = CoordinatorEpoch(1);
    persist_enabled_ = config.persist;
    persist_on_mutation_ = config.persist_on_mutation;
    if (!config.store_path.empty()) {
        store_ = std::make_unique<FileStore>(config.store_path);
    } else {
        store_ = std::make_unique<MemoryStore>();
    }
}

Federation::~Federation() = default;

Result<std::unique_ptr<Federation>> Federation::open(const FederationConfig& config) {
    std::unique_ptr<Federation> instance(new Federation(config));
    {
        std::unique_lock<std::shared_mutex> lock(instance->mutex_);
        const Status restored = instance->restore_locked();
        if (!restored.ok()) {
            return restored;
        }
    }
    std::vector<FederationEvent> events;
    if (instance->recovery_.recovered) {
        events.push_back(instance->make_event(EventKind::FederationRecovered, instance->recovery_.detail));
    } else {
        events.push_back(instance->make_event(EventKind::FederationOpened, "federation created"));
    }
    instance->dispatch(events);
    return std::unique_ptr<Federation>(std::move(instance));
}

Status Federation::restore_locked() {
    recovery_ = RecoveryReport();
    const Result<ByteBuffer> payload = store_->read();
    if (!payload.ok()) {
        if (payload.status().code() == ErrorCode::NotFound) {
            // Fresh federation: publish the initial durable state so that a
            // subsequent restart has something authoritative to recover.
            recovery_.new_epoch = epoch_;
            recovery_.new_generation = generation_;
            recovery_.detail = "fresh federation; no durable state was found";
            return persist_locked().status();
        }
        return payload.status();
    }
    const Result<FederationSnapshot> decoded = decode_snapshot(*payload);
    if (!decoded.ok()) {
        return decoded.status();
    }
    const FederationSnapshot& snapshot = *decoded;
    if (snapshot.federation != id_) {
        return Status(ErrorCode::IntegrityFailure,
                      "durable store belongs to a different federation identity");
    }

    recovery_.recovered = true;
    recovery_.previous_epoch = snapshot.epoch;
    recovery_.new_epoch = snapshot.epoch.next();
    recovery_.new_generation = snapshot.generation.next();
    epoch_ = recovery_.new_epoch;
    generation_ = recovery_.new_generation;
    name_ = snapshot.name.empty() ? name_ : snapshot.name;
    policy_ = snapshot.policy_data;

    for (const MemberRecord& stored : snapshot.members) {
        const AcceleratorDescriptor* descriptor = snapshot.find_descriptor(stored.accelerator);
        if (descriptor == nullptr) {
            // A member without evidence cannot be restored conservatively.
            ++recovery_.decisions_invalidated;
            continue;
        }
        MemberEntry entry;
        entry.record = stored;
        entry.descriptor = *descriptor;

        // Authority from the previous epoch is dead. Members that were live are
        // restored as DEGRADED, which still records durable membership but does
        // not accept new work until a live agent revalidates them.
        switch (stored.state) {
            case MemberState::Active:
            case MemberState::Admitted:
            case MemberState::Degraded:
            case MemberState::Draining:
                entry.record.state = MemberState::Degraded;
                ++recovery_.members_requiring_revalidation;
                break;
            case MemberState::Retired:
                ++recovery_.retired_members;
                break;
            default:
                ++recovery_.members_requiring_revalidation;
                break;
        }
        entry.record.epoch = epoch_;
        entry.record.federation_generation = generation_;
        entry.record.state_generation = entry.record.state_generation.next();
        entry.record.history.push_back("RECOVERED " + std::string(to_string(entry.record.state)) +
                                       " epoch=" + std::to_string(epoch_.value()));
        std::sort(entry.record.history.begin(), entry.record.history.end());
        if (entry.record.history.size() > 64U) {
            entry.record.history.resize(64U);
        }
        entry.record.updated_at = now_timestamp();

        // Dynamic evidence never becomes current merely because it exists on
        // disk: every restored observation requires revalidation and loses its
        // process-local monotonic reference.
        for (EvidenceRecord& evidence : entry.descriptor.evidence) {
            evidence.requires_revalidation = true;
            evidence.observed_monotonic_marker = MonotonicTime();
            evidence.generation = evidence.generation.next();
        }
        physical_index_[entry.record.physical_device].push_back(entry.record.accelerator);
        members_.emplace(entry.record.accelerator, std::move(entry));
        ++recovery_.members_restored;
    }
    for (const WorkloadProfile& profile : snapshot.workloads) {
        workloads_.emplace(profile.class_id, profile);
    }
    // Decisions and migration plans were bound to the previous epoch and
    // generation: they are invalid by construction and are not restored.
    recovery_.decisions_invalidated += snapshot.decisions.size();
    for (const MigrationPlan& plan : snapshot.plans) {
        if (!is_terminal_migration_state(plan.state)) {
            MigrationPlan aborted = plan;
            aborted.state = MigrationState::Aborted;
            aborted.refusal_reasons.push_back(CompatibilityReason{
                ErrorCode::StaleEpoch, "recovery",
                "coordinator authority advanced while this plan was in flight; the plan was aborted", 
                RequirementStrength::Hard, CapabilityState::Unknown});
            sort_reasons(aborted.refusal_reasons);
            plans_.emplace(aborted.id, std::move(aborted));
        } else {
            plans_.emplace(plan.id, plan);
        }
        ++recovery_.plans_aborted;
    }
    recovery_.detail = "epoch " + std::to_string(recovery_.previous_epoch.value()) + " -> " +
                       std::to_string(recovery_.new_epoch.value()) + ", generation " +
                       std::to_string(recovery_.new_generation.value()) + ", members restored " +
                       std::to_string(recovery_.members_restored) + ", requiring revalidation " +
                       std::to_string(recovery_.members_requiring_revalidation);
    return persist_locked().status();
}

FederationSnapshot Federation::snapshot_locked() const {
    FederationSnapshot snapshot;
    snapshot.federation = id_;
    snapshot.generation = generation_;
    snapshot.epoch = epoch_;
    snapshot.name = name_;
    snapshot.policy = policy_.id;
    snapshot.policy_generation = policy_.generation;
    snapshot.policy_data = policy_;
    snapshot.members.reserve(members_.size());
    snapshot.descriptors.reserve(members_.size());
    for (const auto& entry : members_) {
        snapshot.members.push_back(entry.second.record);
        snapshot.descriptors.push_back(entry.second.descriptor);
    }
    snapshot.workloads.reserve(workloads_.size());
    for (const auto& entry : workloads_) {
        snapshot.workloads.push_back(entry.second);
    }
    snapshot.decisions.reserve(decisions_.size());
    for (const auto& entry : decisions_) {
        snapshot.decisions.push_back(entry.second);
    }
    snapshot.plans.reserve(plans_.size());
    for (const auto& entry : plans_) {
        snapshot.plans.push_back(entry.second);
    }
    snapshot.created_at = now_timestamp();
    snapshot.canonicalize();
    return snapshot;
}

VoidResult Federation::persist_locked() {
    if (!persist_enabled_ || store_ == nullptr) {
        return VoidResult();
    }
    if (!persist_on_mutation_ && !force_persist_) {
        // Deferred mode: the caller owns the durability period.
        return VoidResult();
    }
    const FederationSnapshot snapshot = snapshot_locked();
    const ByteBuffer payload = encode_snapshot(snapshot);
    return store_->write(payload);
}

FederationEvent Federation::make_event(EventKind kind, std::string detail, const AcceleratorId& accelerator) {
    FederationEvent event;
    event.id = id_generator_.next_id<EventIdTag>();
    event.kind = kind;
    event.federation = id_;
    event.generation = generation_;
    event.epoch = epoch_;
    event.accelerator = accelerator;
    event.detail = std::move(detail);
    event.at = now_timestamp();
    return event;
}

void Federation::dispatch(const std::vector<FederationEvent>& events) {
    if (events.empty()) {
        return;
    }
    EventSink sink;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        sink = event_sink_;
    }
    if (!sink) {
        return;
    }
    for (const FederationEvent& event : events) {
        try {
            sink(event);
        } catch (...) {
            // A misbehaving sink must never corrupt federation state. The
            // exception is contained here and the remaining events still run.
        }
    }
}

void Federation::set_event_sink(EventSink sink) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    event_sink_ = std::move(sink);
}

Status Federation::validate_authority_locked(const AuthorityClaim& claim) const {
    if (shutdown_) {
        return Status(ErrorCode::ShutdownInProgress, "federation authority is revoked");
    }
    if (claim.check_federation_generation && claim.federation_generation != generation_) {
        return Status(ErrorCode::StaleFederationGeneration,
                      "claim asserts federation generation " +
                          std::to_string(claim.federation_generation.value()) + " but the current generation is " +
                          std::to_string(generation_.value()));
    }
    if (claim.check_epoch && claim.epoch != epoch_) {
        return Status(ErrorCode::StaleEpoch,
                      "claim asserts coordinator epoch " + std::to_string(claim.epoch.value()) +
                          " but the current epoch is " + std::to_string(epoch_.value()));
    }
    return Status::success();
}

Status Federation::validate_authority_locked(const AuthorityClaim& claim, const MemberEntry& entry,
                                             const AcceleratorId& accelerator) const {
    const Status base = validate_authority_locked(claim);
    if (!base.ok()) {
        return base;
    }
    if (claim.check_agent && claim.agent != entry.record.agent) {
        return Status(ErrorCode::StaleAgent, "claim is scoped to a different agent for accelerator " +
                                                 accelerator.to_string());
    }
    if (claim.check_agent_boot && claim.agent_boot != entry.record.agent_boot) {
        return Status(ErrorCode::StaleBoot, "claim presents boot identity " + claim.agent_boot.to_string() +
                                                " but accelerator " + accelerator.to_string() +
                                                " is owned by boot " + entry.record.agent_boot.to_string());
    }
    if (claim.check_device_generation && claim.device_generation != entry.record.device_generation) {
        return Status(ErrorCode::StaleDeviceGeneration,
                      "claim asserts device generation " + std::to_string(claim.device_generation.value()) +
                          " but accelerator " + accelerator.to_string() + " is at generation " +
                          std::to_string(entry.record.device_generation.value()));
    }
    if (claim.check_capability_generation && claim.capability_generation != entry.record.capability_generation) {
        return Status(ErrorCode::StaleCapabilityGeneration,
                      "claim asserts capability generation " + std::to_string(claim.capability_generation.value()) +
                          " but accelerator " + accelerator.to_string() + " is at generation " +
                          std::to_string(entry.record.capability_generation.value()));
    }
    if (claim.check_policy_generation && claim.policy_generation != policy_.generation) {
        return Status(ErrorCode::StalePolicy,
                      "claim asserts policy generation " + std::to_string(claim.policy_generation.value()) +
                          " but the active policy generation is " + std::to_string(policy_.generation.value()));
    }
    return Status::success();
}

FederationId Federation::id() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return id_;
}

FederationGeneration Federation::generation() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return generation_;
}

CoordinatorEpoch Federation::epoch() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return epoch_;
}

std::string Federation::name() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return name_;
}

PolicyGeneration Federation::policy_generation() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return policy_.generation;
}

FederationPolicy Federation::policy() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return policy_;
}

AuthorityClaim Federation::epoch_claim(std::string purpose) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return epoch_only_claim(epoch_, std::move(purpose));
}

std::string Federation::store_location() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return store_ == nullptr ? std::string() : store_->location();
}

Result<MemberRecord> Federation::observe(const AcceleratorDescriptor& descriptor, const AuthorityClaim& claim) {
    std::vector<FederationEvent> events;
    Result<MemberRecord> result = Status(ErrorCode::InternalInvariantViolation, "observe produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const Status authority = validate_authority_locked(claim);
        if (!authority.ok()) {
            events.push_back(make_event(EventKind::AuthorityRejected, authority.describe(), descriptor.id));
            result = authority;
        } else {
            const Status shape = descriptor.validate();
            const AcceleratorId derived = derive_accelerator_id(descriptor.physical_device, descriptor.agent,
                                                                descriptor.agent_boot, descriptor.generation);
            if (!shape.ok()) {
                result = shape;
            } else if (derived != descriptor.id) {
                result = Status(ErrorCode::IntegrityFailure,
                                "advertised accelerator identity does not match its physical device, agent boot, "
                                "and incarnation");
            } else if (descriptor.agent_boot != claim.agent_boot && claim.check_agent_boot) {
                result = Status(ErrorCode::StaleBoot,
                                "advertisement presents a boot identity that does not match the claim");
            } else {
                // Duplicate live boot: the same physical device claimed by two
                // different live boots is a contradictory federation state.
                // Incarnations of one device are found through the physical
                // index, so this check does not scan the whole federation.
                std::vector<AcceleratorId> incarnations;
                {
                    const auto found_physical = physical_index_.find(descriptor.physical_device);
                    if (found_physical != physical_index_.end()) {
                        incarnations = found_physical->second;
                    }
                }
                bool duplicate_live = false;
                for (const AcceleratorId& candidate : incarnations) {
                    if (candidate == descriptor.id) {
                        continue;
                    }
                    const auto entry = members_.find(candidate);
                    if (entry == members_.end()) {
                        continue;
                    }
                    if (holds_competing_authority(entry->second.record.state) &&
                        entry->second.record.agent_boot != descriptor.agent_boot) {
                        duplicate_live = true;
                        result = Status(ErrorCode::DuplicateLiveBoot,
                                        "physical device " + descriptor.physical_device.to_string() +
                                            " is already claimed by live boot " +
                                            entry->second.record.agent_boot.to_string());
                        break;
                    }
                }
                if (!duplicate_live) {
                    auto found = members_.find(descriptor.id);
                    if (found == members_.end()) {
                        // A restarted agent produces a new incarnation identity.
                        // The previous incarnation of the same physical device is
                        // retired explicitly rather than silently inherited.
                        for (const AcceleratorId& candidate : incarnations) {
                            auto entry = members_.find(candidate);
                            if (entry == members_.end() || entry->first == descriptor.id) {
                                continue;
                            }
                            if (entry->second.record.agent == descriptor.agent &&
                                !is_terminal_state(entry->second.record.state)) {
                                entry->second.record.state = MemberState::Retired;
                                entry->second.record.state_generation =
                                    entry->second.record.state_generation.next();
                                entry->second.record.updated_at = now_timestamp();
                                entry->second.record.history.push_back(std::string("RETIRED superseded-by ") +
                                                                       descriptor.id.to_string());
                                std::sort(entry->second.record.history.begin(), entry->second.record.history.end());
                                if (entry->second.record.history.size() > 64U) {
                                    entry->second.record.history.resize(64U);
                                }
                                events.push_back(make_event(EventKind::MemberRetired,
                                                            "superseded by a new incarnation", entry->first));
                            }
                        }
                        MemberEntry entry;
                        entry.descriptor = descriptor;
                        entry.record.federation = id_;
                        entry.record.federation_generation = generation_;
                        entry.record.epoch = epoch_;
                        entry.record.agent = descriptor.agent;
                        entry.record.agent_boot = descriptor.agent_boot;
                        entry.record.agent_generation = AgentGeneration(1);
                        entry.record.accelerator = descriptor.id;
                        entry.record.physical_device = descriptor.physical_device;
                        entry.record.device_generation = descriptor.generation;
                        entry.record.capability_generation = descriptor.capabilities.generation();
                        entry.record.evidence_generation = EvidenceGeneration(1);
                        entry.record.policy_generation = policy_.generation;
                        entry.record.state = MemberState::Observed;
                        entry.record.state_generation = DecisionGeneration(1);
                        entry.record.support_level = descriptor.support_level;
                        entry.record.provenance = descriptor.provenance;
                        entry.record.admitted_at = now_timestamp();
                        entry.record.updated_at = entry.record.admitted_at;
                        entry.record.history.push_back(std::string("DISCOVERED"));
                        entry.record.history.push_back(std::string("OBSERVED"));
                        std::sort(entry.record.history.begin(), entry.record.history.end());
                        generation_ = generation_.next();
                        entry.record.federation_generation = generation_;
                        MemberRecord stored = entry.record;
                        physical_index_[descriptor.physical_device].push_back(descriptor.id);
                        members_.emplace(descriptor.id, std::move(entry));
                        events.push_back(make_event(EventKind::MemberObserved, "advertisement accepted", descriptor.id));
                        result = stored;
                    } else {
                        MemberEntry& entry = found->second;
                        if (is_terminal_state(entry.record.state)) {
                            result = Status(ErrorCode::Retired,
                                            "accelerator incarnation is retired and cannot be revived");
                        } else if (entry.record.state == MemberState::Fenced) {
                            result = Status(ErrorCode::Fenced,
                                            "accelerator is fenced; an explicit reopen is required before it can "
                                            "accept advertisements");
                        } else if (entry.record.agent_boot != descriptor.agent_boot) {
                            result = Status(ErrorCode::StaleBoot,
                                            "advertisement presents a different boot identity for a live incarnation");
                        } else {
                            const bool capabilities_changed =
                                entry.descriptor.capabilities.digest() != descriptor.capabilities.digest();
                            if (capabilities_changed) {
                                CapabilityGeneration next = entry.record.capability_generation.next();
                                entry.descriptor = descriptor;
                                entry.descriptor.capabilities.set_generation(next);
                                entry.record.capability_generation = next;
                                entry.record.evidence_generation = entry.record.evidence_generation.next();
                                entry.record.state_generation = entry.record.state_generation.next();
                                entry.record.support_level = descriptor.support_level;
                                entry.record.provenance = descriptor.provenance;
                                entry.record.policy_generation = policy_.generation;
                                entry.record.updated_at = now_timestamp();
                                entry.record.history.push_back(std::string("CAPABILITY_UPDATE ") +
                                                               std::to_string(entry.record.capability_generation.value()));
                                std::sort(entry.record.history.begin(), entry.record.history.end());
                                if (entry.record.history.size() > 64U) {
                                    entry.record.history.resize(64U);
                                }
                                events.push_back(make_event(EventKind::CapabilityUpdated,
                                                            "capability generation advanced to " +
                                                                std::to_string(entry.record.capability_generation.value()),
                                                            descriptor.id));
                            } else {
                                entry.descriptor = descriptor;
                                entry.record.evidence_generation = entry.record.evidence_generation.next();
                                entry.record.support_level = descriptor.support_level;
                                entry.record.provenance = descriptor.provenance;
                                entry.record.policy_generation = policy_.generation;
                                entry.record.updated_at = now_timestamp();
                                events.push_back(make_event(EventKind::MemberObserved,
                                                            "evidence refreshed without capability change",
                                                            descriptor.id));
                            }
                            generation_ = generation_.next();
                            entry.record.federation_generation = generation_;
                            entry.record.epoch = epoch_;
                            result = entry.record;
                        }
                    }
                }
            }
            if (result.ok()) {
                const VoidResult persisted = persist_locked();
                if (!persisted.ok()) {
                    result = persisted.status();
                } else {
                    events.push_back(make_event(EventKind::StorePersisted, store_->location(), descriptor.id));
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<MemberRecord> Federation::admit(const AcceleratorId& accelerator, const AuthorityClaim& claim) {
    return transition(accelerator, MemberState::Admitted, claim);
}

Result<MemberRecord> Federation::activate(const AcceleratorId& accelerator, const AuthorityClaim& claim) {
    return transition(accelerator, MemberState::Active, claim);
}

Result<MemberRecord> Federation::transition(const AcceleratorId& accelerator, MemberState target,
                                            const AuthorityClaim& claim) {
    std::vector<FederationEvent> events;
    Result<MemberRecord> result = Status(ErrorCode::InternalInvariantViolation, "transition produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const Status authority = validate_authority_locked(claim);
        if (!authority.ok()) {
            events.push_back(make_event(EventKind::AuthorityRejected, authority.describe(), accelerator));
            result = authority;
        } else {
            auto found = members_.find(accelerator);
            if (found == members_.end()) {
                result = Status(ErrorCode::UnknownAccelerator,
                                "no federation member for accelerator " + accelerator.to_string());
            } else {
                MemberEntry& entry = found->second;
                const Status scoped = validate_authority_locked(claim, entry, accelerator);
                if (!scoped.ok()) {
                    events.push_back(make_event(EventKind::AuthorityRejected, scoped.describe(), accelerator));
                    result = scoped;
                } else if (entry.record.state == target) {
                    // Idempotent: re-asserting the current state is not an error
                    // and does not advance any generation.
                    result = entry.record;
                } else if (!is_legal_transition(entry.record.state, target)) {
                    result = Status(ErrorCode::InvalidTransition,
                                    "illegal member transition " +
                                        transition_text(entry.record.state, target) + " for accelerator " +
                                        accelerator.to_string());
                } else {
                    const MemberState previous = entry.record.state;
                    entry.record.state = target;
                    entry.record.state_generation = entry.record.state_generation.next();
                    entry.record.policy_generation = policy_.generation;
                    entry.record.updated_at = now_timestamp();
                    entry.record.history.push_back(transition_text(previous, target));
                    std::sort(entry.record.history.begin(), entry.record.history.end());
                    if (entry.record.history.size() > 64U) {
                        entry.record.history.resize(64U);
                    }
                    generation_ = generation_.next();
                    entry.record.federation_generation = generation_;
                    entry.record.epoch = epoch_;
                    EventKind kind = EventKind::MemberAdmitted;
                    switch (target) {
                        case MemberState::Admitted: kind = EventKind::MemberAdmitted; break;
                        case MemberState::Active: kind = EventKind::MemberActivated; break;
                        case MemberState::Degraded: kind = EventKind::MemberDegraded; break;
                        case MemberState::Draining: kind = EventKind::MemberDraining; break;
                        case MemberState::Fenced: kind = EventKind::MemberFenced; break;
                        case MemberState::Retired: kind = EventKind::MemberRetired; break;
                        default: kind = EventKind::MemberObserved; break;
                    }
                    events.push_back(make_event(kind, transition_text(previous, target), accelerator));
                    result = entry.record;
                    const VoidResult persisted = persist_locked();
                    if (!persisted.ok()) {
                        result = persisted.status();
                    } else {
                        events.push_back(make_event(EventKind::StorePersisted, store_->location(), accelerator));
                    }
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<MemberRecord> Federation::update_capabilities(const AcceleratorId& accelerator,
                                                     const CapabilitySet& capabilities,
                                                     const EvidenceRecord& evidence,
                                                     const AuthorityClaim& claim) {
    std::vector<FederationEvent> events;
    Result<MemberRecord> result = Status(ErrorCode::InternalInvariantViolation, "update produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const Status authority = validate_authority_locked(claim);
        if (!authority.ok()) {
            events.push_back(make_event(EventKind::AuthorityRejected, authority.describe(), accelerator));
            result = authority;
        } else {
            auto found = members_.find(accelerator);
            if (found == members_.end()) {
                result = Status(ErrorCode::UnknownAccelerator,
                                "no federation member for accelerator " + accelerator.to_string());
            } else {
                MemberEntry& entry = found->second;
                const Status scoped = validate_authority_locked(claim, entry, accelerator);
                const Status shape = capabilities.validate();
                const Status evidence_shape = evidence.validate();
                if (!scoped.ok()) {
                    events.push_back(make_event(EventKind::AuthorityRejected, scoped.describe(), accelerator));
                    result = scoped;
                } else if (entry.record.state == MemberState::Fenced || is_terminal_state(entry.record.state)) {
                    result = Status(entry.record.state == MemberState::Fenced ? ErrorCode::Fenced : ErrorCode::Retired,
                                    "accelerator cannot accept capability updates in state " +
                                        std::string(to_string(entry.record.state)));
                } else if (!shape.ok()) {
                    result = shape;
                } else if (!evidence_shape.ok()) {
                    result = evidence_shape;
                } else if (evidence.subject != accelerator) {
                    result = Status(ErrorCode::IntegrityFailure,
                                    "evidence record is bound to a different accelerator");
                } else if (evidence.provenance != entry.record.provenance) {
                    result = Status(ErrorCode::IntegrityFailure,
                                    "evidence provenance does not match the member provenance; provenance cannot be "
                                    "upgraded through a capability update");
                } else if (evidence.payload_digest != capabilities.digest()) {
                    result = Status(ErrorCode::IntegrityFailure,
                                    "evidence payload digest does not match the advertised capability set");
                } else {
                    // Every accepted capability update advances the capability
                    // generation, even when the payload is byte-identical: the
                    // observation itself is new evidence, and treating an
                    // update as a no-op would let a caller silently re-assert
                    // stale authority.
                    const CapabilityGeneration next = entry.record.capability_generation.next();
                    entry.descriptor.capabilities = capabilities;
                    entry.descriptor.capabilities.set_generation(next);
                    entry.record.capability_generation = next;
                    entry.record.evidence_generation = entry.record.evidence_generation.next();
                    entry.record.state_generation = entry.record.state_generation.next();
                    entry.record.policy_generation = policy_.generation;
                    entry.record.updated_at = now_timestamp();
                    entry.descriptor.evidence.clear();
                    entry.descriptor.evidence.push_back(evidence);
                    entry.record.history.push_back(std::string("CAPABILITY_UPDATE ") + std::to_string(next.value()));
                    std::sort(entry.record.history.begin(), entry.record.history.end());
                    if (entry.record.history.size() > 64U) {
                        entry.record.history.resize(64U);
                    }
                    generation_ = generation_.next();
                    entry.record.federation_generation = generation_;
                    entry.record.epoch = epoch_;
                    events.push_back(make_event(EventKind::CapabilityUpdated,
                                                "capability generation advanced to " + std::to_string(next.value()),
                                                accelerator));
                    result = entry.record;
                    const VoidResult persisted = persist_locked();
                    if (!persisted.ok()) {
                        result = persisted.status();
                    } else {
                        events.push_back(make_event(EventKind::StorePersisted, store_->location(), accelerator));
                    }
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<MemberRecord> Federation::fence(const AcceleratorId& accelerator, const AuthorityClaim& claim) {
    return transition(accelerator, MemberState::Fenced, claim);
}

Result<MemberRecord> Federation::reopen(const AcceleratorId& accelerator, const AuthorityClaim& claim) {
    return transition(accelerator, MemberState::Observed, claim);
}

Result<MemberRecord> Federation::retire(const AcceleratorId& accelerator, const AuthorityClaim& claim) {
    return transition(accelerator, MemberState::Retired, claim);
}

Result<MemberRecord> Federation::member(const AcceleratorId& accelerator) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = members_.find(accelerator);
    if (found == members_.end()) {
        return Status(ErrorCode::UnknownAccelerator, "no federation member for accelerator " + accelerator.to_string());
    }
    return found->second.record;
}

Result<AcceleratorDescriptor> Federation::descriptor(const AcceleratorId& accelerator) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = members_.find(accelerator);
    if (found == members_.end()) {
        return Status(ErrorCode::UnknownAccelerator, "no federation member for accelerator " + accelerator.to_string());
    }
    return found->second.descriptor;
}

std::vector<MemberRecord> Federation::members() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<MemberRecord> out;
    out.reserve(members_.size());
    for (const auto& entry : members_) {
        out.push_back(entry.second.record);
    }
    return out;
}

std::vector<AcceleratorId> Federation::member_ids() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<AcceleratorId> out;
    out.reserve(members_.size());
    for (const auto& entry : members_) {
        out.push_back(entry.first);
    }
    return out;
}

std::size_t Federation::member_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return members_.size();
}

Result<PolicyGeneration> Federation::set_policy(const FederationPolicy& policy, const AuthorityClaim& claim) {
    std::vector<FederationEvent> events;
    Result<PolicyGeneration> result = Status(ErrorCode::InternalInvariantViolation, "policy update produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const Status authority = validate_authority_locked(claim);
        if (!authority.ok()) {
            events.push_back(make_event(EventKind::AuthorityRejected, authority.describe()));
            result = authority;
        } else {
            FederationPolicy updated = policy;
            updated.refresh_identity();
            const Status valid = updated.validate();
            if (!valid.ok()) {
                result = valid;
            } else {
                const PolicyGeneration previous = policy_.generation;
                policy_ = updated;
                // A policy change invalidates every decision derived from the
                // previous generation, and every in-flight migration plan that
                // was authorised under it.
                const std::size_t invalidated = decisions_.size();
                decisions_.clear();
                std::size_t aborted = 0;
                for (auto& entry : plans_) {
                    if (!is_terminal_migration_state(entry.second.state)) {
                        entry.second.state = MigrationState::Aborted;
                        entry.second.refusal_reasons.push_back(
                            CompatibilityReason{ErrorCode::StalePolicy, "policy",
                                                "policy generation advanced while this plan was in flight",
                                                RequirementStrength::Hard, CapabilityState::Unknown});
                        sort_reasons(entry.second.refusal_reasons);
                        ++aborted;
                    }
                }
                for (auto& entry : members_) {
                    entry.second.record.policy_generation = policy_.generation;
                }
                generation_ = generation_.next();
                events.push_back(make_event(EventKind::PolicyUpdated,
                                            "policy generation " + std::to_string(previous.value()) + " -> " +
                                                std::to_string(policy_.generation.value()) + ", decisions invalidated " +
                                                std::to_string(invalidated) + ", plans aborted " +
                                                std::to_string(aborted)));
                if (invalidated != 0) {
                    events.push_back(make_event(EventKind::DecisionInvalidated,
                                                std::to_string(invalidated) + " decisions invalidated by policy change"));
                }
                result = policy_.generation;
                const VoidResult persisted = persist_locked();
                if (!persisted.ok()) {
                    result = persisted.status();
                } else {
                    events.push_back(make_event(EventKind::StorePersisted, store_->location()));
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<WorkloadRevision> Federation::register_workload(const WorkloadProfile& profile, const AuthorityClaim& claim) {
    std::vector<FederationEvent> events;
    Result<WorkloadRevision> result = Status(ErrorCode::InternalInvariantViolation, "workload registration produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const Status authority = validate_authority_locked(claim);
        if (!authority.ok()) {
            events.push_back(make_event(EventKind::AuthorityRejected, authority.describe()));
            result = authority;
        } else {
            WorkloadProfile stored = profile;
            if (stored.class_id.is_nil()) {
                stored.class_id = workload_class_id_from_token(stored.name);
            }
            if (stored.requirement_id.is_nil()) {
                stored.requirement_id = derive_requirement_id(stored);
            }
            stored.revision = derive_workload_revision(stored);
            const Status valid = stored.validate();
            if (!valid.ok()) {
                result = valid;
            } else {
                workloads_[stored.class_id] = stored;
                generation_ = generation_.next();
                result = stored.revision;
                const VoidResult persisted = persist_locked();
                if (!persisted.ok()) {
                    result = persisted.status();
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<WorkloadProfile> Federation::workload(const WorkloadClassId& workload) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = workloads_.find(workload);
    if (found == workloads_.end()) {
        return Status(ErrorCode::NotFound, "no registered workload for class " + workload.to_string());
    }
    return found->second;
}

std::vector<WorkloadProfile> Federation::workloads() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<WorkloadProfile> out;
    out.reserve(workloads_.size());
    for (const auto& entry : workloads_) {
        out.push_back(entry.second);
    }
    return out;
}

std::vector<WorkloadClassId> Federation::workload_ids() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<WorkloadClassId> out;
    out.reserve(workloads_.size());
    for (const auto& entry : workloads_) {
        out.push_back(entry.first);
    }
    return out;
}

Result<CompatibilityDecision> Federation::evaluate_locked(const WorkloadProfile& profile,
                                                          const AcceleratorId& accelerator,
                                                          const EvaluationOptions& options,
                                                          bool source_must_exist) const {
    const auto found = members_.find(accelerator);
    if (found == members_.end()) {
        return Status(ErrorCode::UnknownAccelerator, "no federation member for accelerator " + accelerator.to_string());
    }
    const MemberEntry& entry = found->second;

    EvaluationContext context;
    context.federation = id_;
    context.federation_generation = generation_;
    context.epoch = epoch_;
    context.policy_id = policy_.id;
    context.policy_generation = policy_.generation;
    context.policy = &policy_;
    context.workload_revision = profile.revision;
    context.now = now_monotonic();
    context.target.accelerator = entry.record.accelerator;
    context.target.device_generation = entry.record.device_generation;
    context.target.capability_generation = entry.record.capability_generation;
    context.target.evidence_generation = entry.record.evidence_generation;
    context.target.support_level = entry.record.support_level;
    context.target.provenance = entry.record.provenance;
    context.target.capabilities = &entry.descriptor.capabilities;
    context.target.accepts_new_work = entry.record.accepts_new_work();

    bool evidence_fresh = true;
    for (const EvidenceRecord& evidence : entry.descriptor.evidence) {
        if (evidence.requires_revalidation) {
            evidence_fresh = false;
            break;
        }
        if (evidence.dynamic && policy_.required_evidence_freshness_nanos != 0 &&
            !evidence.is_fresh(context.now, policy_.required_evidence_freshness_nanos)) {
            evidence_fresh = false;
            break;
        }
    }
    context.target.evidence_fresh = evidence_fresh;

    EvaluationSource source_storage;
    if (options.source.has_value()) {
        const auto source_found = members_.find(*options.source);
        if (source_found == members_.end()) {
            if (source_must_exist) {
                return Status(ErrorCode::UnknownAccelerator,
                              "no federation member for source accelerator " + options.source->to_string());
            }
        } else {
            const CapabilityLookup vendor = source_found->second.descriptor.capabilities.lookup(cap::kVendorId);
            source_storage.accelerator = source_found->first;
            source_storage.device_generation = source_found->second.record.device_generation;
            source_storage.capability_generation = source_found->second.record.capability_generation;
            source_storage.capabilities = &source_found->second.descriptor.capabilities;
            source_storage.vendor = vendor.present && vendor.value != nullptr ? vendor.value->render() : std::string();
            context.source = &source_storage;
        }
    }
    return evaluate_compatibility(profile, context);
}

Result<CompatibilityDecision> Federation::evaluate(const WorkloadClassId& workload_id,
                                                   const AcceleratorId& accelerator,
                                                   const EvaluationOptions& options) {
    std::vector<FederationEvent> events;
    Result<CompatibilityDecision> result =
        Status(ErrorCode::InternalInvariantViolation, "evaluation produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (shutdown_) {
            return Status(ErrorCode::ShutdownInProgress, "federation authority is revoked");
        }
        const auto profile = workloads_.find(workload_id);
        if (profile == workloads_.end()) {
            result = Status(ErrorCode::NotFound, "no registered workload for class " + workload_id.to_string());
        } else {
            result = evaluate_locked(profile->second, accelerator, options, true);
            if (result.ok()) {
                const DecisionId key = result->decision_id;
                decisions_[key] = *result;
                while (decisions_.size() > max_recorded_decisions_) {
                    auto oldest = decisions_.begin();
                    for (auto it = decisions_.begin(); it != decisions_.end(); ++it) {
                        if (it->second.created_at < oldest->second.created_at ||
                            (it->second.created_at == oldest->second.created_at && it->first < oldest->first)) {
                            oldest = it;
                        }
                    }
                    decisions_.erase(oldest);
                }
                events.push_back(make_event(EventKind::DecisionRecorded, result->render(), accelerator));
                const VoidResult persisted = persist_locked();
                if (!persisted.ok()) {
                    result = persisted.status();
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<CompatibilityDecision> Federation::evaluate_transient(const WorkloadClassId& workload_id,
                                                             const AcceleratorId& accelerator,
                                                             const EvaluationOptions& options) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto profile = workloads_.find(workload_id);
    if (profile == workloads_.end()) {
        return Status(ErrorCode::NotFound, "no registered workload for class " + workload_id.to_string());
    }
    return evaluate_locked(profile->second, accelerator, options, false);
}

Result<FleetEvaluation> Federation::evaluate_fleet(const WorkloadClassId& workload_id,
                                                   const EvaluationOptions& options) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto profile = workloads_.find(workload_id);
    if (profile == workloads_.end()) {
        return Status(ErrorCode::NotFound, "no registered workload for class " + workload_id.to_string());
    }
    if (members_.size() > Limits::kMaxFleetEvaluationTargets) {
        return Status(ErrorCode::BoundsExceeded, "fleet evaluation exceeds the permitted member count");
    }
    EvaluationContext context;
    context.federation = id_;
    context.federation_generation = generation_;
    context.epoch = epoch_;
    context.policy_id = policy_.id;
    context.policy_generation = policy_.generation;
    context.policy = &policy_;
    context.workload_revision = profile->second.revision;
    context.now = now_monotonic();
    EvaluationSource source_storage;
    if (options.source.has_value()) {
        const auto source_found = members_.find(*options.source);
        if (source_found == members_.end()) {
            return Status(ErrorCode::UnknownAccelerator,
                          "no federation member for source accelerator " + options.source->to_string());
        }
        const CapabilityLookup vendor = source_found->second.descriptor.capabilities.lookup(cap::kVendorId);
        source_storage.accelerator = source_found->first;
        source_storage.device_generation = source_found->second.record.device_generation;
        source_storage.capability_generation = source_found->second.record.capability_generation;
        source_storage.capabilities = &source_found->second.descriptor.capabilities;
        source_storage.vendor = vendor.present && vendor.value != nullptr ? vendor.value->render() : std::string();
        context.source = &source_storage;
    }
    std::vector<EvaluationTarget> targets;
    targets.reserve(members_.size());
    for (const auto& entry : members_) {
        EvaluationTarget target;
        target.accelerator = entry.first;
        target.device_generation = entry.second.record.device_generation;
        target.capability_generation = entry.second.record.capability_generation;
        target.evidence_generation = entry.second.record.evidence_generation;
        target.support_level = entry.second.record.support_level;
        target.provenance = entry.second.record.provenance;
        target.capabilities = &entry.second.descriptor.capabilities;
        target.accepts_new_work = entry.second.record.accepts_new_work();
        bool fresh = true;
        for (const EvidenceRecord& evidence : entry.second.descriptor.evidence) {
            if (evidence.requires_revalidation) {
                fresh = false;
                break;
            }
            if (evidence.dynamic && policy_.required_evidence_freshness_nanos != 0 &&
                !evidence.is_fresh(context.now, policy_.required_evidence_freshness_nanos)) {
                fresh = false;
                break;
            }
        }
        target.evidence_fresh = fresh;
        targets.push_back(std::move(target));
    }
    return haf::evaluate_fleet(profile->second, context, std::move(targets));
}

Result<RankingResult> Federation::rank(const WorkloadClassId& workload_id, const RankingWeights& weights) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto profile = workloads_.find(workload_id);
    if (profile == workloads_.end()) {
        return Status(ErrorCode::NotFound, "no registered workload for class " + workload_id.to_string());
    }
    std::vector<RankingCandidate> candidates;
    candidates.reserve(members_.size());
    for (const auto& entry : members_) {
        const Result<CompatibilityDecision> decision = evaluate_locked(profile->second, entry.first, {}, false);
        if (!decision.ok()) {
            return decision.status();
        }
        RankingCandidate candidate;
        candidate.accelerator = entry.first;
        candidate.decision = *decision;
        const CapabilityLookup free_memory = entry.second.descriptor.capabilities.lookup(cap::kMemoryFreeBytes);
        if (free_memory.present && free_memory.value != nullptr && free_memory.state == CapabilityState::Supported) {
            candidate.free_memory_bytes = static_cast<std::uint64_t>(free_memory.value->integer_value());
        }
        candidates.push_back(std::move(candidate));
    }
    return rank_candidates(candidates, weights);
}

Result<CompatibilityDecision> Federation::decision(const DecisionId& id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = decisions_.find(id);
    if (found == decisions_.end()) {
        return Status(ErrorCode::NotFound, "no recorded decision with identity " + id.to_string());
    }
    return found->second;
}

std::vector<CompatibilityDecision> Federation::decisions() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<CompatibilityDecision> out;
    out.reserve(decisions_.size());
    for (const auto& entry : decisions_) {
        out.push_back(entry.second);
    }
    return out;
}

std::size_t Federation::invalidate_stale_decisions() {
    std::size_t removed = 0;
    std::vector<FederationEvent> events;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto it = decisions_.begin(); it != decisions_.end();) {
            const CompatibilityDecision& decision = it->second;
            bool current = decision.federation_generation == generation_ && decision.epoch == epoch_ &&
                           decision.policy_generation == policy_.generation;
            const auto member_found = members_.find(decision.accelerator);
            if (member_found == members_.end()) {
                current = false;
            } else {
                current = current &&
                          decision.device_generation == member_found->second.record.device_generation &&
                          decision.capability_generation == member_found->second.record.capability_generation;
            }
            const auto workload_found = workloads_.find(decision.workload);
            if (workload_found == workloads_.end() || workload_found->second.revision != decision.workload_revision) {
                current = false;
            }
            if (current) {
                ++it;
            } else {
                it = decisions_.erase(it);
                ++removed;
            }
        }
        if (removed != 0) {
            events.push_back(make_event(EventKind::DecisionInvalidated,
                                        std::to_string(removed) + " stale decisions invalidated"));
            static_cast<void>(persist_locked());
        }
    }
    dispatch(events);
    return removed;
}

Result<bool> Federation::verify_decision(const DecisionId& id) const {
    const Result<CompatibilityDecision> stored = decision(id);
    if (!stored.ok()) {
        return stored.status();
    }
    const std::string expected = to_hex(stored->fingerprint());
    if (stored->decision_id != DecisionId::from_raw(derive_identity("haf.decision", expected))) {
        return Status(ErrorCode::IntegrityFailure, "stored decision does not match its fingerprint");
    }
    return true;
}

Result<bool> Federation::decision_is_current(const DecisionId& id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = decisions_.find(id);
    if (found == decisions_.end()) {
        return Status(ErrorCode::NotFound, "no recorded decision with identity " + id.to_string());
    }
    const CompatibilityDecision& decision = found->second;
    if (decision.federation_generation != generation_ || decision.epoch != epoch_ ||
        decision.policy_generation != policy_.generation) {
        return false;
    }
    const auto member_found = members_.find(decision.accelerator);
    if (member_found == members_.end()) {
        return false;
    }
    const auto workload_found = workloads_.find(decision.workload);
    if (workload_found == workloads_.end()) {
        return false;
    }
    return decision.is_current(generation_, epoch_, policy_.generation,
                               member_found->second.record.device_generation,
                               member_found->second.record.capability_generation,
                               member_found->second.record.evidence_generation,
                               workload_found->second.revision);
}

Result<MigrationPlan> Federation::plan_migration(const WorkloadClassId& workload_id, const AcceleratorId& source,
                                                 const AcceleratorId& destination, const AuthorityClaim& claim) {
    std::vector<FederationEvent> events;
    Result<MigrationPlan> result = Status(ErrorCode::InternalInvariantViolation, "planning produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const Status authority = validate_authority_locked(claim);
        if (!authority.ok()) {
            events.push_back(make_event(EventKind::AuthorityRejected, authority.describe(), destination));
            result = authority;
        } else {
            const auto profile = workloads_.find(workload_id);
            const auto source_found = members_.find(source);
            const auto destination_found = members_.find(destination);
            if (profile == workloads_.end()) {
                result = Status(ErrorCode::NotFound, "no registered workload for class " + workload_id.to_string());
            } else if (source_found == members_.end()) {
                result = Status(ErrorCode::UnknownAccelerator, "unknown migration source " + source.to_string());
            } else if (destination_found == members_.end()) {
                result = Status(ErrorCode::UnknownAccelerator, "unknown migration destination " + destination.to_string());
            } else {
                EvaluationOptions source_options;
                source_options.source = destination;
                const Result<CompatibilityDecision> source_decision =
                    evaluate_locked(profile->second, source, source_options, false);
                EvaluationOptions destination_options;
                destination_options.source = source;
                const Result<CompatibilityDecision> destination_decision =
                    evaluate_locked(profile->second, destination, destination_options, false);
                if (!source_decision.ok()) {
                    result = source_decision.status();
                } else if (!destination_decision.ok()) {
                    result = destination_decision.status();
                } else {
                    // The generation advances before the plan is authored so
                    // that the plan is bound to the generation it actually
                    // belongs to. Advancing afterwards would make every plan
                    // stale the moment it was created.
                    generation_ = generation_.next();
                    MigrationRequest request;
                    request.federation = id_;
                    request.federation_generation = generation_;
                    request.epoch = epoch_;
                    request.policy_id = policy_.id;
                    request.policy_generation = policy_.generation;
                    request.policy = &policy_;
                    request.source.accelerator = source;
                    request.source.device_generation = source_found->second.record.device_generation;
                    request.source.capability_generation = source_found->second.record.capability_generation;
                    request.source.capabilities = &source_found->second.descriptor.capabilities;
                    request.source.provenance = source_found->second.record.provenance;
                    request.source.support_level = source_found->second.record.support_level;
                    request.destination.accelerator = destination;
                    request.destination.device_generation = destination_found->second.record.device_generation;
                    request.destination.capability_generation = destination_found->second.record.capability_generation;
                    request.destination.capabilities = &destination_found->second.descriptor.capabilities;
                    request.destination.provenance = destination_found->second.record.provenance;
                    request.destination.support_level = destination_found->second.record.support_level;
                    request.workload = &profile->second;
                    request.workload_revision = profile->second.revision;
                    request.source_decision = source_decision->id;
                    request.destination_decision = destination_decision->id;
                    {
                        const CapabilityLookup vendor =
                            source_found->second.descriptor.capabilities.lookup(cap::kVendorId);
                        request.source.vendor =
                            vendor.present && vendor.value != nullptr ? vendor.value->render() : std::string();
                    }
                    {
                        const CapabilityLookup vendor =
                            destination_found->second.descriptor.capabilities.lookup(cap::kVendorId);
                        request.destination.vendor =
                            vendor.present && vendor.value != nullptr ? vendor.value->render() : std::string();
                    }
                    const Result<MigrationPlan> plan = haf::plan_migration(request);
                    if (!plan.ok()) {
                        result = plan.status();
                    } else {
                        MigrationPlan stored = *plan;
                        plans_[stored.id] = stored;
                        decisions_[source_decision->decision_id] = *source_decision;
                        decisions_[destination_decision->decision_id] = *destination_decision;
                        while (decisions_.size() > max_recorded_decisions_) {
                            decisions_.erase(decisions_.begin());
                        }
                        events.push_back(make_event(EventKind::MigrationPlanned, stored.render(), destination));
                        result = stored;
                        const VoidResult persisted = persist_locked();
                        if (!persisted.ok()) {
                            result = persisted.status();
                        }
                    }
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<MigrationPlan> Federation::migration_plan(const MigrationPlanId& id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = plans_.find(id);
    if (found == plans_.end()) {
        return Status(ErrorCode::NotFound, "no migration plan with identity " + id.to_string());
    }
    return found->second;
}

std::vector<MigrationPlan> Federation::migration_plans() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<MigrationPlan> out;
    out.reserve(plans_.size());
    for (const auto& entry : plans_) {
        out.push_back(entry.second);
    }
    return out;
}

Result<MigrationPlan> Federation::advance_migration(const MigrationPlanId& id, MigrationState target,
                                                    const AuthorityClaim& claim) {
    std::vector<FederationEvent> events;
    Result<MigrationPlan> result = Status(ErrorCode::InternalInvariantViolation, "advance produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const Status authority = validate_authority_locked(claim);
        if (!authority.ok()) {
            events.push_back(make_event(EventKind::AuthorityRejected, authority.describe()));
            result = authority;
        } else {
            auto found = plans_.find(id);
            if (found == plans_.end()) {
                result = Status(ErrorCode::NotFound, "no migration plan with identity " + id.to_string());
            } else {
                MigrationPlan& plan = found->second;
                if (claim.check_policy_generation && claim.policy_generation != policy_.generation) {
                    result = Status(ErrorCode::StalePolicy, "claim presents a stale policy generation for a plan");
                } else if (!plan.is_current(epoch_, policy_.generation, plan.source_generation,
                                            plan.destination_generation, plan.source_capability_generation,
                                            plan.destination_capability_generation, plan.workload_revision)) {
                    result = Status(ErrorCode::StaleMigration,
                                    "migration plan is bound to generations that are no longer current");
                } else {
                    const auto source_found = members_.find(plan.source);
                    const auto destination_found = members_.find(plan.destination);
                    if (source_found == members_.end() || destination_found == members_.end()) {
                        result = Status(ErrorCode::StaleMigration,
                                        "migration plan references an accelerator that is no longer a member");
                    } else if (source_found->second.record.device_generation != plan.source_generation ||
                               destination_found->second.record.device_generation != plan.destination_generation ||
                               source_found->second.record.capability_generation !=
                                   plan.source_capability_generation ||
                               destination_found->second.record.capability_generation !=
                                   plan.destination_capability_generation) {
                        result = Status(ErrorCode::StaleMigration,
                                        "an endpoint generation changed after the plan was created");
                    } else if (plan.state == target) {
                        result = plan;
                    } else if (!is_legal_migration_transition(plan.state, target)) {
                        result = Status(ErrorCode::InvalidTransition,
                                        "illegal migration transition " + migration_transition_text(plan.state, target));
                    } else {
                        plan.state = target;
                        events.push_back(make_event(target == MigrationState::Committed ? EventKind::MigrationCommitted
                                                                                        : EventKind::MigrationAdvanced,
                                                    plan.render(), plan.destination));
                        if (target == MigrationState::Committed) {
                            // Authority moves at commit: the source stops
                            // accepting new work and is put into Draining.
                            if (source_found->second.record.state == MemberState::Active ||
                                source_found->second.record.state == MemberState::Degraded) {
                                source_found->second.record.state = MemberState::Draining;
                                source_found->second.record.state_generation =
                                    source_found->second.record.state_generation.next();
                                source_found->second.record.updated_at = now_timestamp();
                                source_found->second.record.history.push_back(std::string("DRAINING migration-commit"));
                                std::sort(source_found->second.record.history.begin(),
                                          source_found->second.record.history.end());
                            }
                        }
                        generation_ = generation_.next();
                        result = plan;
                        const VoidResult persisted = persist_locked();
                        if (!persisted.ok()) {
                            result = persisted.status();
                        }
                    }
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<MigrationPlan> Federation::commit_migration(const MigrationPlanId& id, const AuthorityClaim& claim) {
    return advance_migration(id, MigrationState::Committed, claim);
}

Result<MigrationPlan> Federation::abort_migration(const MigrationPlanId& id, std::string reason,
                                                  const AuthorityClaim& claim) {
    std::vector<FederationEvent> events;
    Result<MigrationPlan> result = Status(ErrorCode::InternalInvariantViolation, "abort produced no outcome");
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const Status authority = validate_authority_locked(claim);
        if (!authority.ok()) {
            events.push_back(make_event(EventKind::AuthorityRejected, authority.describe()));
            result = authority;
        } else {
            auto found = plans_.find(id);
            if (found == plans_.end()) {
                result = Status(ErrorCode::NotFound, "no migration plan with identity " + id.to_string());
            } else if (is_terminal_migration_state(found->second.state)) {
                result = Status(ErrorCode::InvalidTransition,
                                "migration plan is already terminal in state " +
                                    std::string(to_string(found->second.state)));
            } else {
                found->second.state = MigrationState::Aborted;
                found->second.refusal_reasons.push_back(CompatibilityReason{
                    ErrorCode::Cancelled, "abort", std::move(reason), RequirementStrength::Hard,
                    CapabilityState::Unknown});
                sort_reasons(found->second.refusal_reasons);
                generation_ = generation_.next();
                events.push_back(make_event(EventKind::MigrationAborted, found->second.render(), found->second.destination));
                result = found->second;
                const VoidResult persisted = persist_locked();
                if (!persisted.ok()) {
                    result = persisted.status();
                }
            }
        }
    }
    dispatch(events);
    return result;
}

Result<bool> Federation::migration_plan_is_current(const MigrationPlanId& id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = plans_.find(id);
    if (found == plans_.end()) {
        return Status(ErrorCode::NotFound, "no migration plan with identity " + id.to_string());
    }
    const MigrationPlan& plan = found->second;
    if (plan.epoch != epoch_ || plan.policy_generation != policy_.generation) {
        return false;
    }
    const auto source_found = members_.find(plan.source);
    const auto destination_found = members_.find(plan.destination);
    if (source_found == members_.end() || destination_found == members_.end()) {
        return false;
    }
    return plan.is_current(epoch_, policy_.generation, source_found->second.record.device_generation,
                           destination_found->second.record.device_generation,
                           source_found->second.record.capability_generation,
                           destination_found->second.record.capability_generation, plan.workload_revision);
}

FederationSnapshot Federation::snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return snapshot_locked();
}

VoidResult Federation::persist() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    force_persist_ = true;
    const VoidResult result = persist_locked();
    force_persist_ = false;
    return result;
}

AuditReport Federation::audit() const { return audit_snapshot(snapshot()); }

Result<std::string> Federation::render_members() const {
    const FederationSnapshot current = snapshot();
    std::string out;
    out += "federation ";
    out += current.federation.to_string();
    out += " name=";
    out += current.name;
    out += " generation=";
    out += std::to_string(current.generation.value());
    out += " epoch=";
    out += std::to_string(current.epoch.value());
    out += " members=";
    out += std::to_string(current.members.size());
    for (const MemberRecord& record : current.members) {
        out += "\n";
        out += record.accelerator.to_string();
        out += " state=";
        out += to_string(record.state);
        out += " support=";
        out += to_string(record.support_level);
        out += " evidence=";
        out += to_string(record.provenance);
        out += " device_generation=";
        out += std::to_string(record.device_generation.value());
        out += " capability_generation=";
        out += std::to_string(record.capability_generation.value());
        out += " evidence_generation=";
        out += std::to_string(record.evidence_generation.value());
        out += " agent=";
        out += record.agent.to_string();
        out += " boot=";
        out += record.agent_boot.to_string();
        const AcceleratorDescriptor* descriptor = current.find_descriptor(record.accelerator);
        if (descriptor != nullptr) {
            out += " vendor=";
            out += descriptor->vendor_token;
            out += " product=";
            out += descriptor->product_token;
            out += " architecture=";
            out += descriptor->architecture_token;
            out += " runtime=";
            out += descriptor->runtime_family;
            out += " runtime_version=";
            out += descriptor->runtime_version.to_string();
        }
    }
    return out;
}

Result<std::string> Federation::explain_member(const AcceleratorId& accelerator) const {
    const FederationSnapshot current = snapshot();
    const MemberRecord* record = current.find_member(accelerator);
    const AcceleratorDescriptor* descriptor = current.find_descriptor(accelerator);
    if (record == nullptr || descriptor == nullptr) {
        return Status(ErrorCode::UnknownAccelerator,
                      "no federation member for accelerator " + accelerator.to_string());
    }
    std::string out;
    out += "accelerator ";
    out += accelerator.to_string();
    out += "\n  membership state: ";
    out += to_string(record->state);
    out += " (state generation ";
    out += std::to_string(record->state_generation.value());
    out += ")";
    out += "\n  accepts new work: ";
    out += record->accepts_new_work() ? "yes" : "no";
    out += "\n  lifecycle history:";
    for (const std::string& entry : record->history) {
        out += "\n    ";
        out += entry;
    }
    out += "\n  ownership: agent=";
    out += record->agent.to_string();
    out += " boot=";
    out += record->agent_boot.to_string();
    out += " node=";
    out += descriptor->node.to_string();
    out += "\n  physical device: ";
    out += record->physical_device.to_string();
    out += "\n  incarnation device generation: ";
    out += std::to_string(record->device_generation.value());
    out += "\n  identity: vendor=";
    out += descriptor->vendor_token;
    out += " product=";
    out += descriptor->product_token;
    out += " architecture=";
    out += descriptor->architecture_token;
    out += " device_generation=";
    out += descriptor->device_generation_token;
    out += "\n  runtime: family=";
    out += descriptor->runtime_family;
    out += " version=";
    out += descriptor->runtime_version.to_string();
    out += " driver=";
    out += descriptor->driver_version.to_string();
    out += "\n  support level: ";
    out += to_string(record->support_level);
    out += "\n  evidence provenance: ";
    out += to_string(record->provenance);
    out += "\n  generations: federation=";
    out += std::to_string(record->federation_generation.value());
    out += " epoch=";
    out += std::to_string(record->epoch.value());
    out += " capability=";
    out += std::to_string(record->capability_generation.value());
    out += " evidence=";
    out += std::to_string(record->evidence_generation.value());
    out += " policy=";
    out += std::to_string(record->policy_generation.value());
    out += "\n  evidence records:";
    if (descriptor->evidence.empty()) {
        out += "\n    (none)";
    }
    for (const EvidenceRecord& evidence : descriptor->evidence) {
        out += "\n    adapter=";
        out += evidence.adapter;
        out += " source=";
        out += evidence.source;
        out += " kind=";
        out += to_string(evidence.kind);
        out += " provenance=";
        out += to_string(evidence.provenance);
        out += " requires_revalidation=";
        out += evidence.requires_revalidation ? "yes" : "no";
        out += " dynamic=";
        out += evidence.dynamic ? "yes" : "no";
        out += " digest=";
        out += to_hex(evidence.payload_digest);
    }
    out += "\n  advertised capabilities:";
    for (const CapabilityRecord& capability : descriptor->capabilities.records()) {
        out += "\n    ";
        out += capability.key.name();
        out += " = ";
        out += to_string(capability.state);
        if (capability.state == CapabilityState::Supported || capability.state == CapabilityState::Degraded) {
            out += " (";
            out += capability.value.render();
            out += ")";
        }
    }
    out += "\n  closed namespaces:";
    if (descriptor->capabilities.closed_namespaces().empty()) {
        out += " (none)";
    }
    for (const std::string& name_space : descriptor->capabilities.closed_namespaces()) {
        out += " ";
        out += name_space;
    }
    return out;
}

VoidResult Federation::shutdown() {
    std::vector<FederationEvent> events;
    VoidResult result;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (shutdown_) {
            return VoidResult();
        }
        events.push_back(make_event(EventKind::ShutdownStarted, "federation authority revocation requested"));
        shutdown_ = true;
        result = persist_locked();
        events.push_back(make_event(EventKind::ShutdownCompleted, "federation authority revoked"));
    }
    dispatch(events);
    return result;
}

}  // namespace haf

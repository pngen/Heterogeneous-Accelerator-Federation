#include "haf/engine/audit.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/hash.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

class AuditBuilder {
public:
    void check() noexcept { ++checks_; }

    void violate(std::string code, std::string subject, std::string detail) {
        AuditViolation violation;
        violation.code = std::move(code);
        violation.subject = std::move(subject);
        violation.detail = std::move(detail);
        violations_.push_back(std::move(violation));
    }

    [[nodiscard]] AuditReport finish() {
        std::sort(violations_.begin(), violations_.end(),
                  [](const AuditViolation& a, const AuditViolation& b) {
                      if (a.code != b.code) {
                          return a.code < b.code;
                      }
                      if (a.subject != b.subject) {
                          return a.subject < b.subject;
                      }
                      return a.detail < b.detail;
                  });
        AuditReport report;
        report.checks_run = checks_;
        report.violations = std::move(violations_);
        return report;
    }

private:
    std::size_t checks_{0};
    std::vector<AuditViolation> violations_;
};

[[nodiscard]] bool is_live(MemberState state) {
    return state == MemberState::Admitted || state == MemberState::Active || state == MemberState::Degraded ||
           state == MemberState::Draining;
}

}  // namespace

std::string AuditReport::render() const {
    std::string out;
    out += "audit checks=";
    out += std::to_string(checks_run);
    out += " violations=";
    out += std::to_string(violations.size());
    for (const AuditViolation& violation : violations) {
        out += "\n  [";
        out += violation.code;
        out += "] ";
        out += violation.subject;
        out += ": ";
        out += violation.detail;
    }
    return out;
}

AuditReport audit_snapshot(const FederationSnapshot& snapshot) {
    AuditBuilder audit;

    // --- Identity uniqueness ---------------------------------------------
    audit.check();
    {
        std::set<std::string> seen;
        for (const MemberRecord& record : snapshot.members) {
            const std::string key = record.accelerator.to_string();
            if (!seen.insert(key).second) {
                audit.violate("identity.uniqueness", key, "accelerator identity appears in more than one member record");
            }
        }
    }
    audit.check();
    {
        std::set<std::string> seen;
        for (const AcceleratorDescriptor& descriptor : snapshot.descriptors) {
            const std::string key = descriptor.id.to_string();
            if (!seen.insert(key).second) {
                audit.violate("identity.uniqueness", key, "accelerator identity appears in more than one descriptor");
            }
        }
    }
    audit.check();
    {
        std::set<std::string> seen;
        for (const CompatibilityDecision& decision : snapshot.decisions) {
            const std::string key = decision.decision_id.to_string();
            if (!seen.insert(key).second) {
                audit.violate("identity.uniqueness", key, "decision identity is duplicated");
            }
        }
    }

    // --- Membership / descriptor consistency ------------------------------
    for (const MemberRecord& record : snapshot.members) {
        audit.check();
        const AcceleratorDescriptor* descriptor = snapshot.find_descriptor(record.accelerator);
        if (descriptor == nullptr) {
            audit.violate("membership.descriptor", record.accelerator.to_string(),
                          "member has no matching accelerator descriptor");
            continue;
        }
        audit.check();
        if (descriptor->agent != record.agent || descriptor->agent_boot != record.agent_boot) {
            audit.violate("membership.ownership", record.accelerator.to_string(),
                          "member agent ownership disagrees with the descriptor");
        }
        audit.check();
        if (descriptor->generation != record.device_generation) {
            audit.violate("membership.device_generation", record.accelerator.to_string(),
                          "member device generation disagrees with the descriptor");
        }
        audit.check();
        if (descriptor->capabilities.generation() != record.capability_generation) {
            audit.violate("membership.capability_generation", record.accelerator.to_string(),
                          "member capability generation disagrees with the descriptor capability set");
        }
        audit.check();
        if (descriptor->support_level != record.support_level || descriptor->provenance != record.provenance) {
            audit.violate("membership.support_level", record.accelerator.to_string(),
                          "member support level or evidence provenance disagrees with the descriptor");
        }
        audit.check();
        if (record.federation != snapshot.federation) {
            audit.violate("membership.federation", record.accelerator.to_string(),
                          "member belongs to a different federation identity");
        }
        audit.check();
        if (record.epoch > snapshot.epoch && is_live(record.state)) {
            audit.violate("membership.epoch", record.accelerator.to_string(),
                          "live member claims an epoch newer than the federation epoch");
        }
        audit.check();
        if (is_live(record.state) && record.evidence_generation.is_initial()) {
            audit.violate("membership.evidence", record.accelerator.to_string(),
                          "live member has no evidence generation");
        }
    }

    // --- Duplicate live physical claims ------------------------------------
    {
        std::map<std::string, std::pair<std::string, std::string>> claimed;
        for (const MemberRecord& record : snapshot.members) {
            audit.check();
            if (!is_live(record.state)) {
                continue;
            }
            const std::string key = record.physical_device.to_string();
            const std::string boot = record.agent_boot.to_string();
            const auto found = claimed.find(key);
            if (found == claimed.end()) {
                claimed.emplace(key, std::make_pair(boot, record.accelerator.to_string()));
                continue;
            }
            if (found->second.first != boot) {
                audit.violate("membership.duplicate_live_boot", key,
                              "physical device is claimed by two live boots: " + found->second.first + " and " + boot);
            }
        }
    }

    // --- Lifecycle legality -------------------------------------------------
    for (const MemberRecord& record : snapshot.members) {
        audit.check();
        if (is_terminal_state(record.state) && record.state != MemberState::Retired) {
            audit.violate("lifecycle.terminal", record.accelerator.to_string(), "member is in an undefined terminal state");
        }
        audit.check();
        if (record.history.empty()) {
            audit.violate("lifecycle.history", record.accelerator.to_string(), "member has no lifecycle history");
        }
        audit.check();
        // Canonical order means sorted; the same transition may legitimately
        // appear more than once (a device can return to ACTIVE on every
        // revalidation), so duplicates are not a violation.
        std::string previous;
        for (std::size_t i = 0; i < record.history.size(); ++i) {
            if (i != 0 && record.history[i] < previous) {
                audit.violate("lifecycle.history_order", record.accelerator.to_string(),
                              "member history is not in canonical order");
                break;
            }
            previous = record.history[i];
        }
    }

    // --- Policy consistency -------------------------------------------------
    audit.check();
    if (snapshot.policy != snapshot.policy_data.id) {
        audit.violate("policy.identity", snapshot.policy.to_string(),
                      "snapshot policy identity disagrees with the embedded policy");
    }
    audit.check();
    if (snapshot.policy_generation != snapshot.policy_data.generation) {
        audit.violate("policy.generation", snapshot.policy.to_string(),
                      "snapshot policy generation disagrees with the embedded policy generation");
    }

    // --- Decisions ----------------------------------------------------------
    for (const CompatibilityDecision& decision : snapshot.decisions) {
        audit.check();
        if (decision.federation != snapshot.federation) {
            audit.violate("decision.federation", decision.decision_id.to_string(),
                          "decision references a different federation identity");
        }
        audit.check();
        if (snapshot.find_descriptor(decision.accelerator) == nullptr) {
            audit.violate("decision.accelerator", decision.decision_id.to_string(),
                          "decision references an accelerator that is not present");
        }
        audit.check();
        if (decision.policy != snapshot.policy) {
            audit.violate("decision.policy", decision.decision_id.to_string(),
                          "decision references a policy that is not the active policy");
        }
        audit.check();
        if (snapshot.find_workload(decision.workload) == nullptr) {
            audit.violate("decision.workload", decision.decision_id.to_string(),
                          "decision references a workload that is not registered");
        }
        audit.check();
        const std::string expected = to_hex(decision.fingerprint());
        (void)expected;
        if (decision.decision_id !=
            DecisionId::from_raw(derive_identity("haf.decision", decision.fingerprint_hex()))) {
            audit.violate("decision.fingerprint", decision.decision_id.to_string(),
                          "decision identity does not match its content fingerprint");
        }
        audit.check();
        const AcceleratorDescriptor* descriptor = snapshot.find_descriptor(decision.accelerator);
        if (descriptor != nullptr && decision.capability_generation != descriptor->capabilities.generation()) {
            audit.violate("decision.stale_capability", decision.decision_id.to_string(),
                          "decision was produced from a capability generation that is no longer current");
        }
    }

    // --- Migration plans -----------------------------------------------------
    for (const MigrationPlan& plan : snapshot.plans) {
        audit.check();
        if (plan.federation != snapshot.federation) {
            audit.violate("migration.federation", plan.id.to_string(),
                          "migration plan references a different federation identity");
        }
        audit.check();
        const AcceleratorDescriptor* source = snapshot.find_descriptor(plan.source);
        const AcceleratorDescriptor* destination = snapshot.find_descriptor(plan.destination);
        if (source == nullptr || destination == nullptr) {
            audit.violate("migration.endpoint", plan.id.to_string(),
                          "migration plan references a source or destination that is not present");
        } else {
            audit.check();
            if (source->generation != plan.source_generation) {
                audit.violate("migration.source_generation", plan.id.to_string(),
                              "migration plan source generation is no longer current");
            }
            audit.check();
            if (destination->generation != plan.destination_generation) {
                audit.violate("migration.destination_generation", plan.id.to_string(),
                              "migration plan destination generation is no longer current");
            }
            audit.check();
            if (source->capabilities.generation() != plan.source_capability_generation) {
                audit.violate("migration.source_capability", plan.id.to_string(),
                              "migration plan source capability generation is no longer current");
            }
            audit.check();
            if (destination->capabilities.generation() != plan.destination_capability_generation) {
                audit.violate("migration.destination_capability", plan.id.to_string(),
                              "migration plan destination capability generation is no longer current");
            }
        }
        audit.check();
        if (plan.source == plan.destination) {
            audit.violate("migration.self", plan.id.to_string(), "migration plan moves a workload onto itself");
        }
        audit.check();
        if (plan.policy != snapshot.policy) {
            audit.violate("migration.policy", plan.id.to_string(),
                          "migration plan references a policy that is not the active policy");
        }
        audit.check();
        if (plan.state == MigrationState::Refused && plan.refusal_reasons.empty()) {
            audit.violate("migration.refusal", plan.id.to_string(), "refused migration plan carries no refusal reason");
        }
    }

    // At most one committed plan per workload: two committed plans for the same
    // workload could describe two authoritative executions.
    {
        std::map<std::string, std::size_t> committed_per_workload;
        for (const MigrationPlan& plan : snapshot.plans) {
            audit.check();
            if (plan.state == MigrationState::Committed) {
                ++committed_per_workload[plan.workload.to_string()];
            }
        }
        for (const auto& entry : committed_per_workload) {
            if (entry.second > 1) {
                audit.violate("migration.double_authority", entry.first,
                              "more than one committed migration plan exists for this workload");
            }
        }
    }

    // --- Capability references ----------------------------------------------
    for (const AcceleratorDescriptor& descriptor : snapshot.descriptors) {
        audit.check();
        const Status status = descriptor.capabilities.validate();
        if (!status.ok()) {
            audit.violate("capability.invalid", descriptor.id.to_string(), status.describe());
        }
        audit.check();
        if (descriptor.capabilities.records().empty()) {
            audit.violate("capability.empty", descriptor.id.to_string(), "descriptor advertises no capabilities");
        }
        audit.check();
        for (const EvidenceRecord& evidence : descriptor.evidence) {
            if (evidence.subject != descriptor.id) {
                audit.violate("evidence.subject", descriptor.id.to_string(),
                              "evidence record is bound to a different accelerator");
            }
        }
    }

    // --- Workload requirements ----------------------------------------------
    for (const WorkloadProfile& profile : snapshot.workloads) {
        audit.check();
        const Status status = profile.validate();
        if (!status.ok()) {
            audit.violate("workload.invalid", profile.class_id.to_string(), status.describe());
        }
    }

    // --- Generation monotonicity ---------------------------------------------
    audit.check();
    for (const MemberRecord& record : snapshot.members) {
        if (is_live(record.state) && record.state_generation.is_initial()) {
            audit.violate("generation.monotonic", record.accelerator.to_string(),
                          "live member has an initial state generation");
        }
    }

    return audit.finish();
}

}  // namespace haf

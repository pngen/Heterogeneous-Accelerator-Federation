#include "haf/federation/events.hpp"

namespace haf {

std::string_view to_string(EventKind kind) noexcept {
    switch (kind) {
        case EventKind::FederationOpened: return "FEDERATION_OPENED";
        case EventKind::FederationRecovered: return "FEDERATION_RECOVERED";
        case EventKind::MemberObserved: return "MEMBER_OBSERVED";
        case EventKind::MemberAdmitted: return "MEMBER_ADMITTED";
        case EventKind::MemberActivated: return "MEMBER_ACTIVATED";
        case EventKind::MemberDegraded: return "MEMBER_DEGRADED";
        case EventKind::MemberDraining: return "MEMBER_DRAINING";
        case EventKind::MemberFenced: return "MEMBER_FENCED";
        case EventKind::MemberRetired: return "MEMBER_RETIRED";
        case EventKind::CapabilityUpdated: return "CAPABILITY_UPDATED";
        case EventKind::PolicyUpdated: return "POLICY_UPDATED";
        case EventKind::DecisionRecorded: return "DECISION_RECORDED";
        case EventKind::DecisionInvalidated: return "DECISION_INVALIDATED";
        case EventKind::MigrationPlanned: return "MIGRATION_PLANNED";
        case EventKind::MigrationAdvanced: return "MIGRATION_ADVANCED";
        case EventKind::MigrationCommitted: return "MIGRATION_COMMITTED";
        case EventKind::MigrationAborted: return "MIGRATION_ABORTED";
        case EventKind::AuthorityRejected: return "AUTHORITY_REJECTED";
        case EventKind::StorePersisted: return "STORE_PERSISTED";
        case EventKind::ShutdownStarted: return "SHUTDOWN_STARTED";
        case EventKind::ShutdownCompleted: return "SHUTDOWN_COMPLETED";
    }
    return "UNKNOWN";
}

}  // namespace haf

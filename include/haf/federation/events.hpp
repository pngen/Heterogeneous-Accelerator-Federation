// Heterogeneous Accelerator Federation - federation events.
//
// Events are dispatched synchronously on the calling thread AFTER the internal
// lock has been released. This is deliberate:
//
//   * a sink may call back into the same federation without deadlocking;
//   * a sink can never observe a partially mutated federation;
//   * a sink that throws is contained by the runtime and cannot corrupt state.
//
// Sinks must therefore be fast and must not assume ordering across threads.

#ifndef HAF_FEDERATION_EVENTS_HPP
#define HAF_FEDERATION_EVENTS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/ids.hpp"
#include "haf/core/time.hpp"

namespace haf {

enum class EventKind : std::uint8_t {
    FederationOpened = 0,
    FederationRecovered = 1,
    MemberObserved = 2,
    MemberAdmitted = 3,
    MemberActivated = 4,
    MemberDegraded = 5,
    MemberDraining = 6,
    MemberFenced = 7,
    MemberRetired = 8,
    CapabilityUpdated = 9,
    PolicyUpdated = 10,
    DecisionRecorded = 11,
    DecisionInvalidated = 12,
    MigrationPlanned = 13,
    MigrationAdvanced = 14,
    MigrationCommitted = 15,
    MigrationAborted = 16,
    AuthorityRejected = 17,
    StorePersisted = 18,
    ShutdownStarted = 19,
    ShutdownCompleted = 20,
};

[[nodiscard]] std::string_view to_string(EventKind kind) noexcept;

struct FederationEvent {
    EventId id{};
    EventKind kind{EventKind::FederationOpened};
    FederationId federation{};
    FederationGeneration generation{};
    CoordinatorEpoch epoch{};
    AcceleratorId accelerator{};
    std::string detail;
    Timestamp at{};
};

using EventSink = std::function<void(const FederationEvent&)>;

}  // namespace haf

#endif  // HAF_FEDERATION_EVENTS_HPP

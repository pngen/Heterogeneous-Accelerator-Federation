#include "haf/model/membership.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include "haf/core/limits.hpp"

namespace haf {
namespace {

constexpr std::size_t kMaxHistoryEntries = 64;

}  // namespace

bool MemberRecord::is_live() const noexcept {
    switch (state) {
        case MemberState::Admitted:
        case MemberState::Active:
        case MemberState::Degraded:
        case MemberState::Draining:
            return true;
        default:
            return false;
    }
}

bool MemberRecord::accepts_new_work() const noexcept { return state == MemberState::Active; }

void MemberRecord::serialize(ByteWriter& writer) const {
    writer.id128(federation.raw());
    writer.generation(federation_generation);
    writer.generation(epoch);
    writer.id128(agent.raw());
    writer.id128(agent_boot.raw());
    writer.generation(agent_generation);
    writer.id128(accelerator.raw());
    writer.id128(physical_device.raw());
    writer.generation(device_generation);
    writer.generation(capability_generation);
    writer.generation(evidence_generation);
    writer.generation(policy_generation);
    writer.u8(static_cast<std::uint8_t>(state));
    writer.generation(state_generation);
    writer.u8(static_cast<std::uint8_t>(support_level));
    writer.u8(static_cast<std::uint8_t>(provenance));
    writer.i64(to_unix_nanos(admitted_at));
    writer.i64(to_unix_nanos(updated_at));
    writer.u32(static_cast<std::uint32_t>(history.size()));
    for (const std::string& entry : history) {
        writer.string(entry);
    }
}

bool MemberRecord::deserialize(ByteReader& reader, MemberRecord& out, bool persisted_scale) {
    const std::uint32_t history_limit =
        persisted_scale ? static_cast<std::uint32_t>(kMaxHistoryEntries) : static_cast<std::uint32_t>(kMaxHistoryEntries);
    MemberRecord record;
    Id128 raw_federation;
    Id128 raw_agent;
    Id128 raw_boot;
    Id128 raw_accelerator;
    Id128 raw_physical;
    std::uint8_t raw_state = 0;
    std::uint8_t raw_support = 0;
    std::uint8_t raw_provenance = 0;
    std::int64_t admitted_at = 0;
    std::int64_t updated_at = 0;
    if (!reader.id128(raw_federation) || !reader.generation(record.federation_generation) ||
        !reader.generation(record.epoch) || !reader.id128(raw_agent) || !reader.id128(raw_boot) ||
        !reader.generation(record.agent_generation) || !reader.id128(raw_accelerator) ||
        !reader.id128(raw_physical) || !reader.generation(record.device_generation) ||
        !reader.generation(record.capability_generation) || !reader.generation(record.evidence_generation) ||
        !reader.generation(record.policy_generation) || !reader.u8(raw_state) ||
        !reader.generation(record.state_generation) || !reader.u8(raw_support) || !reader.u8(raw_provenance) ||
        !reader.i64(admitted_at) || !reader.i64(updated_at)) {
        return false;
    }
    if (!member_state_from_wire(raw_state, record.state)) {
        reader.fail(ErrorCode::MalformedData, "member state is outside the declared domain");
        return false;
    }
    if (!support_level_from_wire(raw_support, record.support_level)) {
        reader.fail(ErrorCode::MalformedData, "support level is outside the declared domain");
        return false;
    }
    if (!evidence_provenance_from_wire(raw_provenance, record.provenance)) {
        reader.fail(ErrorCode::MalformedData, "evidence provenance is outside the declared domain");
        return false;
    }
    std::uint32_t history_count = 0;
    if (!reader.count(history_count, history_limit, 4)) {
        return false;
    }
    record.history.reserve(history_count);
    for (std::uint32_t i = 0; i < history_count; ++i) {
        std::string entry;
        if (!reader.string(entry, Limits::kMaxDescriptionBytes)) {
            return false;
        }
        record.history.push_back(std::move(entry));
    }
    record.federation = FederationId::from_raw(raw_federation);
    record.agent = AgentId::from_raw(raw_agent);
    record.agent_boot = AgentBootId::from_raw(raw_boot);
    record.accelerator = AcceleratorId::from_raw(raw_accelerator);
    record.physical_device = PhysicalDeviceId::from_raw(raw_physical);
    record.admitted_at = from_unix_nanos(admitted_at);
    record.updated_at = from_unix_nanos(updated_at);
    if (record.accelerator.is_nil() || record.agent.is_nil()) {
        reader.fail(ErrorCode::MalformedData, "member record is missing a required identity");
        return false;
    }
    out = std::move(record);
    return true;
}

}  // namespace haf

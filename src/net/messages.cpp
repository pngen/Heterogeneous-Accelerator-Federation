#include "haf/net/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/limits.hpp"

namespace haf::net {
namespace {

constexpr std::size_t kMaxTextBytes = Limits::kMaxDescriptionBytes;

[[nodiscard]] Result<ByteBuffer> finish(ByteWriter& writer) { return writer.take(); }

[[nodiscard]] Status truncated(const ByteReader& reader, const char* what) {
    if (reader.failed()) {
        return reader.error();
    }
    return Status(ErrorCode::MalformedData, std::string("message payload ended before ") + what);
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello: return "HELLO";
        case MessageType::HelloAck: return "HELLO_ACK";
        case MessageType::Advertise: return "ADVERTISE";
        case MessageType::AdvertiseAck: return "ADVERTISE_ACK";
        case MessageType::Query: return "QUERY";
        case MessageType::QueryResponse: return "QUERY_RESPONSE";
        case MessageType::Evaluate: return "EVALUATE";
        case MessageType::EvaluateResponse: return "EVALUATE_RESPONSE";
        case MessageType::PlanMigration: return "PLAN_MIGRATION";
        case MessageType::PlanMigrationResponse: return "PLAN_MIGRATION_RESPONSE";
        case MessageType::Transition: return "TRANSITION";
        case MessageType::TransitionResponse: return "TRANSITION_RESPONSE";
        case MessageType::Heartbeat: return "HEARTBEAT";
        case MessageType::HeartbeatAck: return "HEARTBEAT_ACK";
        case MessageType::Goodbye: return "GOODBYE";
        case MessageType::ErrorResponse: return "ERROR";
        case MessageType::RegisterWorkload: return "REGISTER_WORKLOAD";
        case MessageType::RegisterWorkloadAck: return "REGISTER_WORKLOAD_ACK";
    }
    return "UNKNOWN";
}

bool message_type_from_wire(std::uint16_t raw, MessageType& out) noexcept {
    if (raw < 1 || raw > 18) {
        return false;
    }
    out = static_cast<MessageType>(raw);
    return true;
}

bool is_known_request(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello:
        case MessageType::Advertise:
        case MessageType::Query:
        case MessageType::Evaluate:
        case MessageType::PlanMigration:
        case MessageType::Transition:
        case MessageType::Heartbeat:
        case MessageType::Goodbye:
        case MessageType::RegisterWorkload:
            return true;
        default:
            return false;
    }
}

std::string_view to_string(ClientRole role) noexcept {
    switch (role) {
        case ClientRole::Agent: return "agent";
        case ClientRole::Inspector: return "inspector";
    }
    return "inspector";
}

bool client_role_from_wire(std::uint8_t raw, ClientRole& out) noexcept {
    switch (raw) {
        case 0: out = ClientRole::Agent; return true;
        case 1: out = ClientRole::Inspector; return true;
        default: return false;
    }
}

std::string_view to_string(QueryKind kind) noexcept {
    switch (kind) {
        case QueryKind::FederationSummary: return "federation-summary";
        case QueryKind::MemberList: return "member-list";
        case QueryKind::MemberDetail: return "member-detail";
        case QueryKind::MemberExplain: return "member-explain";
        case QueryKind::Policy: return "policy";
        case QueryKind::CapabilityList: return "capability-list";
        case QueryKind::Audit: return "audit";
        case QueryKind::StoreVerify: return "store-verify";
        case QueryKind::AdapterAvailability: return "adapter-availability";
        case QueryKind::WorkloadList: return "workload-list";
        case QueryKind::PlanList: return "plan-list";
        case QueryKind::RecoveryReport: return "recovery-report";
        case QueryKind::CompatibilityMatrix: return "compatibility-matrix";
        case QueryKind::PortabilityExplain: return "portability-explain";
    }
    return "unknown";
}

bool query_kind_from_wire(std::uint8_t raw, QueryKind& out) noexcept {
    if (raw > static_cast<std::uint8_t>(QueryKind::PortabilityExplain)) {
        return false;
    }
    out = static_cast<QueryKind>(raw);
    return true;
}

void write_authority_claim(ByteWriter& writer, const AuthorityClaim& claim) {
    writer.boolean(claim.check_federation_generation);
    writer.generation(claim.federation_generation);
    writer.boolean(claim.check_epoch);
    writer.generation(claim.epoch);
    writer.boolean(claim.check_agent);
    writer.id128(claim.agent.raw());
    writer.boolean(claim.check_agent_boot);
    writer.id128(claim.agent_boot.raw());
    writer.boolean(claim.check_device_generation);
    writer.generation(claim.device_generation);
    writer.boolean(claim.check_capability_generation);
    writer.generation(claim.capability_generation);
    writer.boolean(claim.check_policy_generation);
    writer.generation(claim.policy_generation);
    writer.boolean(claim.check_workload_revision);
    writer.generation(claim.workload_revision);
    writer.string(claim.purpose);
}

bool read_authority_claim(ByteReader& reader, AuthorityClaim& claim) {
    Id128 raw_agent;
    Id128 raw_boot;
    if (!reader.boolean(claim.check_federation_generation) || !reader.generation(claim.federation_generation) ||
        !reader.boolean(claim.check_epoch) || !reader.generation(claim.epoch) || !reader.boolean(claim.check_agent) ||
        !reader.id128(raw_agent) || !reader.boolean(claim.check_agent_boot) || !reader.id128(raw_boot) ||
        !reader.boolean(claim.check_device_generation) || !reader.generation(claim.device_generation) ||
        !reader.boolean(claim.check_capability_generation) || !reader.generation(claim.capability_generation) ||
        !reader.boolean(claim.check_policy_generation) || !reader.generation(claim.policy_generation) ||
        !reader.boolean(claim.check_workload_revision) || !reader.generation(claim.workload_revision) ||
        !reader.string(claim.purpose, Limits::kMaxDescriptionBytes)) {
        return false;
    }
    claim.agent = AgentId::from_raw(raw_agent);
    claim.agent_boot = AgentBootId::from_raw(raw_boot);
    return true;
}

Result<ByteBuffer> encode_message(const HelloRequest& message) {
    ByteWriter writer;
    writer.u16(message.protocol_version);
    writer.u8(static_cast<std::uint8_t>(message.role));
    writer.id128(message.federation.raw());
    writer.id128(message.agent.raw());
    writer.id128(message.agent_boot.raw());
    writer.id128(message.node.raw());
    writer.string(message.node_token);
    writer.string(message.client_name);
    writer.string(message.client_version);
    writer.u64(message.nonce);
    return finish(writer);
}

Result<HelloRequest> decode_hello_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    HelloRequest message;
    Id128 federation;
    Id128 agent;
    Id128 boot;
    Id128 node;
    std::uint8_t role = 0;
    if (!reader.u16(message.protocol_version) || !reader.u8(role) || !reader.id128(federation) ||
        !reader.id128(agent) || !reader.id128(boot) || !reader.id128(node) ||
        !reader.string(message.node_token, Limits::kMaxNameBytes) ||
        !reader.string(message.client_name, Limits::kMaxNameBytes) ||
        !reader.string(message.client_version, Limits::kMaxTokenBytes) || !reader.u64(message.nonce)) {
        return truncated(reader, "the hello request was complete");
    }
    if (!client_role_from_wire(role, message.role)) {
        return Status(ErrorCode::MalformedData, "hello request declares an unknown client role");
    }
    message.federation = FederationId::from_raw(federation);
    message.agent = AgentId::from_raw(agent);
    message.agent_boot = AgentBootId::from_raw(boot);
    message.node = NodeId::from_raw(node);
    return message;
}

Result<ByteBuffer> encode_message(const HelloResponse& message) {
    ByteWriter writer;
    writer.boolean(message.accepted);
    writer.u16(message.negotiated_version);
    writer.generation(message.epoch);
    writer.generation(message.generation);
    writer.generation(message.policy_generation);
    writer.id128(message.session.raw());
    writer.string(message.server_version);
    writer.string(message.detail);
    return finish(writer);
}

Result<HelloResponse> decode_hello_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    HelloResponse message;
    Id128 session;
    if (!reader.boolean(message.accepted) || !reader.u16(message.negotiated_version) ||
        !reader.generation(message.epoch) || !reader.generation(message.generation) ||
        !reader.generation(message.policy_generation) || !reader.id128(session) ||
        !reader.string(message.server_version, Limits::kMaxTokenBytes) ||
        !reader.string(message.detail, kMaxTextBytes)) {
        return truncated(reader, "the hello response was complete");
    }
    message.session = SessionId::from_raw(session);
    return message;
}

Result<ByteBuffer> encode_message(const AdvertiseRequest& message) {
    if (message.devices.size() > Limits::kMaxAdvertisementsPerMessage) {
        return Status(ErrorCode::BoundsExceeded, "advertisement exceeds the permitted device count");
    }
    ByteWriter writer;
    write_authority_claim(writer, message.claim);
    writer.u32(static_cast<std::uint32_t>(message.devices.size()));
    for (const AcceleratorDescriptor& descriptor : message.devices) {
        descriptor.serialize(writer);
    }
    return finish(writer);
}

Result<AdvertiseRequest> decode_advertise_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    AdvertiseRequest message;
    if (!read_authority_claim(reader, message.claim)) {
        return truncated(reader, "the advertisement authority claim was complete");
    }
    std::uint32_t count = 0;
    if (!reader.count(count, static_cast<std::uint32_t>(Limits::kMaxAdvertisementsPerMessage), 8)) {
        return reader.error();
    }
    message.devices.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        AcceleratorDescriptor descriptor;
        if (!AcceleratorDescriptor::deserialize(reader, descriptor, false)) {
            return reader.failed() ? reader.error()
                                   : Status(ErrorCode::MalformedAdvertisement,
                                            "advertisement contains a malformed accelerator descriptor");
        }
        message.devices.push_back(std::move(descriptor));
    }
    if (reader.remaining() != 0) {
        return Status(ErrorCode::MalformedAdvertisement, "advertisement payload has trailing bytes");
    }
    return message;
}

Result<ByteBuffer> encode_message(const AdvertiseResponse& message) {
    if (message.members.size() > Limits::kMaxMembersPerResponse) {
        return Status(ErrorCode::BoundsExceeded, "advertisement response exceeds the permitted member count");
    }
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(message.members.size()));
    for (const MemberRecord& record : message.members) {
        record.serialize(writer);
    }
    writer.string(message.detail);
    return finish(writer);
}

Result<AdvertiseResponse> decode_advertise_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    AdvertiseResponse message;
    std::uint32_t count = 0;
    if (!reader.count(count, static_cast<std::uint32_t>(Limits::kMaxMembersPerResponse), 8)) {
        return reader.error();
    }
    message.members.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        MemberRecord record;
        if (!MemberRecord::deserialize(reader, record, false)) {
            return reader.failed() ? reader.error()
                                   : Status(ErrorCode::MalformedData, "advertisement response member is malformed");
        }
        message.members.push_back(std::move(record));
    }
    if (!reader.string(message.detail, kMaxTextBytes)) {
        return truncated(reader, "the advertisement response detail was complete");
    }
    return message;
}

Result<ByteBuffer> encode_message(const QueryRequest& message) {
    ByteWriter writer;
    writer.u8(static_cast<std::uint8_t>(message.kind));
    writer.id128(message.accelerator.raw());
    writer.id128(message.secondary.raw());
    writer.id128(message.workload.raw());
    writer.boolean(message.has_workload);
    return finish(writer);
}

Result<QueryRequest> decode_query_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    QueryRequest message;
    std::uint8_t kind = 0;
    Id128 accelerator;
    Id128 secondary;
    Id128 workload;
    if (!reader.u8(kind) || !reader.id128(accelerator) || !reader.id128(secondary) || !reader.id128(workload) ||
        !reader.boolean(message.has_workload)) {
        return truncated(reader, "the query request was complete");
    }
    if (!query_kind_from_wire(kind, message.kind)) {
        return Status(ErrorCode::MalformedData, "query request declares an unknown query kind");
    }
    message.accelerator = AcceleratorId::from_raw(accelerator);
    message.secondary = AcceleratorId::from_raw(secondary);
    message.workload = WorkloadClassId::from_raw(workload);
    return message;
}

Result<ByteBuffer> encode_message(const QueryResponse& message) {
    ByteWriter writer;
    writer.u8(static_cast<std::uint8_t>(message.kind));
    writer.u32(message.item_count);
    writer.string(message.text);
    return finish(writer);
}

Result<QueryResponse> decode_query_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    QueryResponse message;
    std::uint8_t kind = 0;
    if (!reader.u8(kind) || !reader.u32(message.item_count) || !reader.string(message.text, kMaxTextBytes)) {
        return truncated(reader, "the query response was complete");
    }
    if (!query_kind_from_wire(kind, message.kind)) {
        return Status(ErrorCode::MalformedData, "query response declares an unknown query kind");
    }
    return message;
}

Result<ByteBuffer> encode_message(const EvaluateRequest& message) {
    ByteWriter writer;
    writer.id128(message.workload.raw());
    writer.id128(message.accelerator.raw());
    writer.boolean(message.has_source);
    writer.id128(message.source.raw());
    return finish(writer);
}

Result<EvaluateRequest> decode_evaluate_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    EvaluateRequest message;
    Id128 workload;
    Id128 accelerator;
    Id128 source;
    if (!reader.id128(workload) || !reader.id128(accelerator) || !reader.boolean(message.has_source) ||
        !reader.id128(source)) {
        return truncated(reader, "the evaluate request was complete");
    }
    message.workload = WorkloadClassId::from_raw(workload);
    message.accelerator = AcceleratorId::from_raw(accelerator);
    message.source = AcceleratorId::from_raw(source);
    return message;
}

Result<ByteBuffer> encode_message(const EvaluateResponse& message) {
    ByteWriter writer;
    message.decision.serialize(writer);
    writer.string(message.rendered);
    return finish(writer);
}

Result<EvaluateResponse> decode_evaluate_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    EvaluateResponse message;
    if (!CompatibilityDecision::deserialize(reader, message.decision, false)) {
        return reader.failed() ? reader.error()
                               : Status(ErrorCode::MalformedData, "evaluate response decision is malformed");
    }
    if (!reader.string(message.rendered, kMaxTextBytes)) {
        return truncated(reader, "the evaluate response rendering was complete");
    }
    return message;
}

Result<ByteBuffer> encode_message(const PlanMigrationRequest& message) {
    ByteWriter writer;
    writer.id128(message.workload.raw());
    writer.id128(message.source.raw());
    writer.id128(message.destination.raw());
    write_authority_claim(writer, message.claim);
    return finish(writer);
}

Result<PlanMigrationRequest> decode_plan_migration_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    PlanMigrationRequest message;
    Id128 workload;
    Id128 source;
    Id128 destination;
    if (!reader.id128(workload) || !reader.id128(source) || !reader.id128(destination) ||
        !read_authority_claim(reader, message.claim)) {
        return truncated(reader, "the migration planning request was complete");
    }
    message.workload = WorkloadClassId::from_raw(workload);
    message.source = AcceleratorId::from_raw(source);
    message.destination = AcceleratorId::from_raw(destination);
    return message;
}

Result<ByteBuffer> encode_message(const PlanMigrationResponse& message) {
    ByteWriter writer;
    message.plan.serialize(writer);
    writer.string(message.rendered);
    return finish(writer);
}

Result<PlanMigrationResponse> decode_plan_migration_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    PlanMigrationResponse message;
    if (!MigrationPlan::deserialize(reader, message.plan, false)) {
        return reader.failed() ? reader.error()
                               : Status(ErrorCode::MalformedData, "migration plan response is malformed");
    }
    if (!reader.string(message.rendered, kMaxTextBytes)) {
        return truncated(reader, "the migration plan rendering was complete");
    }
    return message;
}

Result<ByteBuffer> encode_message(const TransitionRequest& message) {
    ByteWriter writer;
    writer.id128(message.accelerator.raw());
    writer.u8(static_cast<std::uint8_t>(message.target));
    write_authority_claim(writer, message.claim);
    return finish(writer);
}

Result<TransitionRequest> decode_transition_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    TransitionRequest message;
    Id128 accelerator;
    std::uint8_t target = 0;
    if (!reader.id128(accelerator) || !reader.u8(target) || !read_authority_claim(reader, message.claim)) {
        return truncated(reader, "the transition request was complete");
    }
    if (!member_state_from_wire(target, message.target)) {
        return Status(ErrorCode::MalformedData, "transition request declares an unknown member state");
    }
    message.accelerator = AcceleratorId::from_raw(accelerator);
    return message;
}

Result<ByteBuffer> encode_message(const TransitionResponse& message) {
    ByteWriter writer;
    message.member.serialize(writer);
    writer.string(message.detail);
    return finish(writer);
}

Result<TransitionResponse> decode_transition_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    TransitionResponse message;
    if (!MemberRecord::deserialize(reader, message.member, false)) {
        return reader.failed() ? reader.error()
                               : Status(ErrorCode::MalformedData, "transition response member is malformed");
    }
    if (!reader.string(message.detail, kMaxTextBytes)) {
        return truncated(reader, "the transition response detail was complete");
    }
    return message;
}

Result<ByteBuffer> encode_message(const HeartbeatRequest& message) {
    ByteWriter writer;
    write_authority_claim(writer, message.claim);
    writer.generation(message.sequence);
    return finish(writer);
}

Result<HeartbeatRequest> decode_heartbeat_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    HeartbeatRequest message;
    if (!read_authority_claim(reader, message.claim) || !reader.generation(message.sequence)) {
        return truncated(reader, "the heartbeat request was complete");
    }
    return message;
}

Result<ByteBuffer> encode_message(const HeartbeatResponse& message) {
    ByteWriter writer;
    writer.generation(message.epoch);
    writer.generation(message.generation);
    writer.generation(message.policy_generation);
    writer.boolean(message.authority_still_valid);
    return finish(writer);
}

Result<HeartbeatResponse> decode_heartbeat_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    HeartbeatResponse message;
    if (!reader.generation(message.epoch) || !reader.generation(message.generation) ||
        !reader.generation(message.policy_generation) || !reader.boolean(message.authority_still_valid)) {
        return truncated(reader, "the heartbeat response was complete");
    }
    return message;
}

Result<ByteBuffer> encode_message(const GoodbyeRequest& message) {
    ByteWriter writer;
    writer.string(message.reason);
    return finish(writer);
}

Result<GoodbyeRequest> decode_goodbye_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    GoodbyeRequest message;
    if (!reader.string(message.reason, kMaxTextBytes)) {
        return truncated(reader, "the goodbye request was complete");
    }
    return message;
}

Result<ByteBuffer> encode_message(const ErrorResponse& message) {
    ByteWriter writer;
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.string(message.message);
    writer.string(message.detail);
    return finish(writer);
}

Result<ErrorResponse> decode_error_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    ErrorResponse message;
    std::uint16_t code = 0;
    if (!reader.u16(code) || !reader.string(message.message, kMaxTextBytes) ||
        !reader.string(message.detail, kMaxTextBytes)) {
        return truncated(reader, "the error response was complete");
    }
    if (code > static_cast<std::uint16_t>(ErrorCode::Unsupported)) {
        return Status(ErrorCode::MalformedData, "error response declares an unknown error code");
    }
    message.code = static_cast<ErrorCode>(code);
    return message;
}

Result<ByteBuffer> encode_message(const RegisterWorkloadRequest& message) {
    ByteWriter writer;
    message.profile.serialize(writer);
    write_authority_claim(writer, message.claim);
    return finish(writer);
}

Result<RegisterWorkloadRequest> decode_register_workload_request(const ByteBuffer& payload) {
    ByteReader reader(payload);
    RegisterWorkloadRequest message;
    if (!WorkloadProfile::deserialize(reader, message.profile, false)) {
        return reader.failed() ? reader.error()
                               : Status(ErrorCode::MalformedData, "workload registration profile is malformed");
    }
    if (!read_authority_claim(reader, message.claim)) {
        return truncated(reader, "the workload registration claim was complete");
    }
    return message;
}

Result<ByteBuffer> encode_message(const RegisterWorkloadResponse& message) {
    ByteWriter writer;
    writer.id128(message.class_id.raw());
    writer.generation(message.revision);
    writer.string(message.detail);
    return finish(writer);
}

Result<RegisterWorkloadResponse> decode_register_workload_response(const ByteBuffer& payload) {
    ByteReader reader(payload);
    RegisterWorkloadResponse message;
    Id128 class_id;
    if (!reader.id128(class_id) || !reader.generation(message.revision) ||
        !reader.string(message.detail, kMaxTextBytes)) {
        return truncated(reader, "the workload registration response was complete");
    }
    message.class_id = WorkloadClassId::from_raw(class_id);
    return message;
}

}  // namespace haf::net

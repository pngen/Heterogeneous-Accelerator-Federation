// Heterogeneous Accelerator Federation - control-plane messages.
//
// Message payloads are typed structures, not generic maps. Every decoder
// validates enum domains, counts, lengths, and cross-field consistency before
// returning a value.

#ifndef HAF_NET_MESSAGES_HPP
#define HAF_NET_MESSAGES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/federation/authority.hpp"
#include "haf/federation/snapshot.hpp"
#include "haf/model/accelerator.hpp"

namespace haf::net {

enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    Advertise = 3,
    AdvertiseAck = 4,
    Query = 5,
    QueryResponse = 6,
    Evaluate = 7,
    EvaluateResponse = 8,
    PlanMigration = 9,
    PlanMigrationResponse = 10,
    Transition = 11,
    TransitionResponse = 12,
    Heartbeat = 13,
    HeartbeatAck = 14,
    Goodbye = 15,
    ErrorResponse = 16,
    RegisterWorkload = 17,
    RegisterWorkloadAck = 18,
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;
[[nodiscard]] bool message_type_from_wire(std::uint16_t raw, MessageType& out) noexcept;
[[nodiscard]] bool is_known_request(MessageType type) noexcept;

enum class ClientRole : std::uint8_t {
    Agent = 0,
    Inspector = 1,
};

[[nodiscard]] std::string_view to_string(ClientRole role) noexcept;
[[nodiscard]] bool client_role_from_wire(std::uint8_t raw, ClientRole& out) noexcept;

enum class QueryKind : std::uint8_t {
    FederationSummary = 0,
    MemberList = 1,
    MemberDetail = 2,
    MemberExplain = 3,
    Policy = 4,
    CapabilityList = 5,
    Audit = 6,
    StoreVerify = 7,
    AdapterAvailability = 8,
    WorkloadList = 9,
    PlanList = 10,
    RecoveryReport = 11,
    CompatibilityMatrix = 12,
    PortabilityExplain = 13,
};

[[nodiscard]] std::string_view to_string(QueryKind kind) noexcept;
[[nodiscard]] bool query_kind_from_wire(std::uint8_t raw, QueryKind& out) noexcept;

struct HelloRequest {
    std::uint16_t protocol_version{1};
    ClientRole role{ClientRole::Inspector};
    FederationId federation{};
    AgentId agent{};
    AgentBootId agent_boot{};
    NodeId node{};
    std::string node_token;
    std::string client_name;
    std::string client_version;
    std::uint64_t nonce{0};
};

struct HelloResponse {
    bool accepted{false};
    std::uint16_t negotiated_version{1};
    CoordinatorEpoch epoch{};
    FederationGeneration generation{};
    PolicyGeneration policy_generation{};
    SessionId session{};
    std::string server_version;
    std::string detail;
};

struct AdvertiseRequest {
    AuthorityClaim claim{};
    std::vector<AcceleratorDescriptor> devices;
};

struct AdvertiseResponse {
    std::vector<MemberRecord> members;
    std::string detail;
};

struct QueryRequest {
    QueryKind kind{QueryKind::FederationSummary};
    AcceleratorId accelerator{};
    /// Secondary accelerator, used by PortabilityExplain.
    AcceleratorId secondary{};
    WorkloadClassId workload{};
    bool has_workload{false};
};

struct QueryResponse {
    QueryKind kind{QueryKind::FederationSummary};
    std::uint32_t item_count{0};
    std::string text;
};

struct EvaluateRequest {
    WorkloadClassId workload{};
    AcceleratorId accelerator{};
    bool has_source{false};
    AcceleratorId source{};
};

struct EvaluateResponse {
    CompatibilityDecision decision{};
    std::string rendered;
};

struct PlanMigrationRequest {
    WorkloadClassId workload{};
    AcceleratorId source{};
    AcceleratorId destination{};
    AuthorityClaim claim{};
};

struct PlanMigrationResponse {
    MigrationPlan plan{};
    std::string rendered;
};

struct TransitionRequest {
    AcceleratorId accelerator{};
    MemberState target{MemberState::Observed};
    AuthorityClaim claim{};
};

struct TransitionResponse {
    MemberRecord member{};
    std::string detail;
};

struct HeartbeatRequest {
    AuthorityClaim claim{};
    MessageSequence sequence{};
};

struct HeartbeatResponse {
    CoordinatorEpoch epoch{};
    FederationGeneration generation{};
    PolicyGeneration policy_generation{};
    bool authority_still_valid{true};
};

struct GoodbyeRequest {
    std::string reason;
};

struct ErrorResponse {
    ErrorCode code{ErrorCode::Ok};
    std::string message;
    std::string detail;
};

struct RegisterWorkloadRequest {
    WorkloadProfile profile{};
    AuthorityClaim claim{};
};

struct RegisterWorkloadResponse {
    WorkloadClassId class_id{};
    WorkloadRevision revision{};
    std::string detail;
};

// --- Encoding / decoding ---------------------------------------------------

[[nodiscard]] Result<ByteBuffer> encode_message(const HelloRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const HelloResponse& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const AdvertiseRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const AdvertiseResponse& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const QueryRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const QueryResponse& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const EvaluateRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const EvaluateResponse& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const PlanMigrationRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const PlanMigrationResponse& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const TransitionRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const TransitionResponse& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const HeartbeatRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const HeartbeatResponse& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const GoodbyeRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const ErrorResponse& message);

[[nodiscard]] Result<HelloRequest> decode_hello_request(const ByteBuffer& payload);
[[nodiscard]] Result<HelloResponse> decode_hello_response(const ByteBuffer& payload);
[[nodiscard]] Result<AdvertiseRequest> decode_advertise_request(const ByteBuffer& payload);
[[nodiscard]] Result<AdvertiseResponse> decode_advertise_response(const ByteBuffer& payload);
[[nodiscard]] Result<QueryRequest> decode_query_request(const ByteBuffer& payload);
[[nodiscard]] Result<QueryResponse> decode_query_response(const ByteBuffer& payload);
[[nodiscard]] Result<EvaluateRequest> decode_evaluate_request(const ByteBuffer& payload);
[[nodiscard]] Result<EvaluateResponse> decode_evaluate_response(const ByteBuffer& payload);
[[nodiscard]] Result<PlanMigrationRequest> decode_plan_migration_request(const ByteBuffer& payload);
[[nodiscard]] Result<PlanMigrationResponse> decode_plan_migration_response(const ByteBuffer& payload);
[[nodiscard]] Result<TransitionRequest> decode_transition_request(const ByteBuffer& payload);
[[nodiscard]] Result<TransitionResponse> decode_transition_response(const ByteBuffer& payload);
[[nodiscard]] Result<HeartbeatRequest> decode_heartbeat_request(const ByteBuffer& payload);
[[nodiscard]] Result<HeartbeatResponse> decode_heartbeat_response(const ByteBuffer& payload);
[[nodiscard]] Result<GoodbyeRequest> decode_goodbye_request(const ByteBuffer& payload);
[[nodiscard]] Result<ErrorResponse> decode_error_response(const ByteBuffer& payload);
[[nodiscard]] Result<ByteBuffer> encode_message(const RegisterWorkloadRequest& message);
[[nodiscard]] Result<ByteBuffer> encode_message(const RegisterWorkloadResponse& message);
[[nodiscard]] Result<RegisterWorkloadRequest> decode_register_workload_request(const ByteBuffer& payload);
[[nodiscard]] Result<RegisterWorkloadResponse> decode_register_workload_response(const ByteBuffer& payload);

// --- Shared fragments ------------------------------------------------------

void write_authority_claim(ByteWriter& writer, const AuthorityClaim& claim);
[[nodiscard]] bool read_authority_claim(ByteReader& reader, AuthorityClaim& claim);

}  // namespace haf::net

#endif  // HAF_NET_MESSAGES_HPP

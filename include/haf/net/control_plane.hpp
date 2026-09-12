// Heterogeneous Accelerator Federation - coordinator control plane.
//
// The control plane is the only place where network input becomes federation
// authority. It performs, in order:
//
//   HELLO              -> protocol version negotiation, federation identity
//                         check, role binding, duplicate live boot rejection
//   session validation -> every later message must carry the session's epoch
//                         and generation; a stale session cannot mutate
//   ADVERTISE          -> advertisement validation, evidence binding, policy
//                         validation, admission, generation assignment,
//                         activation
//   QUERY / EVALUATE / PLAN / TRANSITION / HEARTBEAT
//
// Nothing in this file is allowed to trust a peer: every field is validated by
// the federation runtime, and every rejection is a typed error with a stable
// machine-readable code.

#ifndef HAF_NET_CONTROL_PLANE_HPP
#define HAF_NET_CONTROL_PLANE_HPP

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "haf/adapters/registry.hpp"
#include "haf/federation/federation.hpp"
#include "haf/net/server.hpp"

namespace haf::net {

struct ControlPlaneConfig {
    FederationId federation{};
    std::string name{"federation"};
    FederationPolicy policy{};
    std::filesystem::path store_path{};
    std::uint64_t id_seed{0};
    bool persist{true};
    std::string server_version{"haf_coordinator/1.0.0"};
    std::vector<adapters::AdapterAvailability> adapters;
};

class ControlPlane final : public ConnectionHandler {
public:
    [[nodiscard]] static Result<std::unique_ptr<ControlPlane>> create(const ControlPlaneConfig& config);

    [[nodiscard]] Result<ResponseMessage> handle(MessageType type, const ByteBuffer& payload,
                                                 ConnectionState& connection) override;
    void on_disconnected(ConnectionState& connection) override;

    [[nodiscard]] Federation& federation() { return *federation_; }
    [[nodiscard]] const Federation& federation() const { return *federation_; }

    /// Number of sessions that completed the handshake.
    [[nodiscard]] std::size_t established_session_count() const;

    /// True when an agent session is bound to the given agent and boot.
    [[nodiscard]] bool has_live_boot(const AgentId& agent, const AgentBootId& boot) const;

private:
    explicit ControlPlane(ControlPlaneConfig config);

    struct SessionData {
        ClientRole role{ClientRole::Inspector};
        FederationId federation{};
        AgentId agent{};
        AgentBootId agent_boot{};
        CoordinatorEpoch epoch{};
        FederationGeneration generation{};
        std::string client_name;
        std::string client_version;
        std::string node_token;
    };

    [[nodiscard]] Result<ResponseMessage> handle_hello(const ByteBuffer& payload, ConnectionState& connection);
    [[nodiscard]] Result<ResponseMessage> handle_advertise(const ByteBuffer& payload, ConnectionState& connection);
    [[nodiscard]] Result<ResponseMessage> handle_query(const ByteBuffer& payload, ConnectionState& connection);
    [[nodiscard]] Result<ResponseMessage> handle_evaluate(const ByteBuffer& payload, ConnectionState& connection);
    [[nodiscard]] Result<ResponseMessage> handle_plan(const ByteBuffer& payload, ConnectionState& connection);
    [[nodiscard]] Result<ResponseMessage> handle_transition(const ByteBuffer& payload, ConnectionState& connection);
    [[nodiscard]] Result<ResponseMessage> handle_heartbeat(const ByteBuffer& payload, ConnectionState& connection);
    [[nodiscard]] Result<ResponseMessage> handle_goodbye(const ByteBuffer& payload, ConnectionState& connection);
    [[nodiscard]] Result<ResponseMessage> handle_register_workload(const ByteBuffer& payload,
                                                                   ConnectionState& connection);

    [[nodiscard]] Status require_session(const ConnectionState& connection, SessionData*& session);
    [[nodiscard]] Status require_agent_session(const ConnectionState& connection, SessionData*& session);
    [[nodiscard]] Status validate_session_authority(const SessionData& session) const;

    [[nodiscard]] std::string render_query(const QueryRequest& request) const;

    ControlPlaneConfig config_;
    std::unique_ptr<Federation> federation_;
    mutable std::mutex sessions_mutex_;
    std::map<std::string, SessionData> sessions_;
};

}  // namespace haf::net

#endif  // HAF_NET_CONTROL_PLANE_HPP

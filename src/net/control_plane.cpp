#include "haf/net/control_plane.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/limits.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/model/capability_key.hpp"

namespace haf::net {
namespace {

[[nodiscard]] std::string key_for(const ConnectionState& connection) { return connection.id.to_string(); }

[[nodiscard]] Result<ResponseMessage> make_response(MessageType type, ByteBuffer payload) {
    ResponseMessage response;
    response.type = type;
    response.payload = std::move(payload);
    response.error = false;
    return response;
}

[[nodiscard]] Result<ResponseMessage> make_error(ErrorCode code, const std::string& message,
                                                 const std::string& detail = std::string()) {
    ResponseMessage response;
    response.type = MessageType::ErrorResponse;
    response.error = true;
    ErrorResponse error;
    error.code = code;
    error.message = message;
    error.detail = detail;
    Result<ByteBuffer> payload = encode_message(error);
    if (!payload.ok()) {
        return payload.status();
    }
    response.payload = *payload;
    return response;
}

[[nodiscard]] std::string generation_line(const Federation& federation) {
    std::ostringstream stream;
    stream << "epoch=" << federation.epoch().value() << " generation=" << federation.generation().value()
           << " policy_generation=" << federation.policy_generation().value();
    return stream.str();
}

}  // namespace

ControlPlane::ControlPlane(ControlPlaneConfig config) : config_(std::move(config)) {}

Result<std::unique_ptr<ControlPlane>> ControlPlane::create(const ControlPlaneConfig& config) {
    std::unique_ptr<ControlPlane> plane(new ControlPlane(config));
    FederationConfig federation_config;
    federation_config.id = config.federation;
    federation_config.name = config.name;
    federation_config.policy = config.policy;
    federation_config.store_path = config.store_path;
    federation_config.id_seed = config.id_seed;
    federation_config.persist = config.persist;
    Result<std::unique_ptr<Federation>> federation = Federation::open(federation_config);
    if (!federation.ok()) {
        return federation.status();
    }
    plane->federation_ = std::move(*federation);
    return std::unique_ptr<ControlPlane>(std::move(plane));
}

std::size_t ControlPlane::established_session_count() const {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    return sessions_.size();
}

bool ControlPlane::has_live_boot(const AgentId& agent, const AgentBootId& boot) const {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    for (const auto& entry : sessions_) {
        if (entry.second.role == ClientRole::Agent && entry.second.agent == agent &&
            entry.second.agent_boot == boot) {
            return true;
        }
    }
    return false;
}

Status ControlPlane::require_session(const ConnectionState& connection, SessionData*& session) {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    const auto found = sessions_.find(key_for(connection));
    if (found == sessions_.end()) {
        return Status(ErrorCode::HandshakeFailed, "no session is established on this connection; send HELLO first");
    }
    session = &found->second;
    return Status::success();
}

Status ControlPlane::require_agent_session(const ConnectionState& connection, SessionData*& session) {
    const Status status = require_session(connection, session);
    if (!status.ok()) {
        return status;
    }
    if (session->role != ClientRole::Agent) {
        return Status(ErrorCode::ProtocolViolation, "this operation requires an agent session");
    }
    return Status::success();
}

Status ControlPlane::validate_session_authority(const SessionData& session) const {
    if (session.epoch != federation_->epoch()) {
        return Status(ErrorCode::StaleEpoch,
                      "session was established under coordinator epoch " + std::to_string(session.epoch.value()) +
                          " but the current epoch is " + std::to_string(federation_->epoch().value()));
    }
    return Status::success();
}

Result<ResponseMessage> ControlPlane::handle(MessageType type, const ByteBuffer& payload,
                                             ConnectionState& connection) {
    switch (type) {
        case MessageType::Hello:
            return handle_hello(payload, connection);
        case MessageType::Advertise:
            return handle_advertise(payload, connection);
        case MessageType::Query:
            return handle_query(payload, connection);
        case MessageType::Evaluate:
            return handle_evaluate(payload, connection);
        case MessageType::PlanMigration:
            return handle_plan(payload, connection);
        case MessageType::Transition:
            return handle_transition(payload, connection);
        case MessageType::Heartbeat:
            return handle_heartbeat(payload, connection);
        case MessageType::Goodbye:
            return handle_goodbye(payload, connection);
        case MessageType::RegisterWorkload:
            return handle_register_workload(payload, connection);
        default:
            return make_error(ErrorCode::UnknownMessageType, "message type is not handled by the control plane",
                              std::string(to_string(type)));
    }
}

void ControlPlane::on_disconnected(ConnectionState& connection) {
    SessionData session;
    bool had_session = false;
    {
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        const auto found = sessions_.find(key_for(connection));
        if (found != sessions_.end()) {
            session = found->second;
            had_session = true;
        }
        sessions_.erase(key_for(connection));
    }
    if (!had_session || session.role != ClientRole::Agent || session.agent.is_nil()) {
        return;
    }
    // An agent session that has gone away no longer holds authority. Every
    // member it owned is fenced immediately, so a dead or half-open session can
    // never keep a device eligible for new work, and a later boot of the same
    // physical device can supersede it instead of colliding with it.
    for (const MemberRecord& record : federation_->members()) {
        if (record.agent != session.agent || record.agent_boot != session.agent_boot) {
            continue;
        }
        if (!record.is_live() || record.state == MemberState::Fenced) {
            continue;
        }
        AuthorityClaim claim = federation_->epoch_claim("session ended");
        claim.check_agent = true;
        claim.agent = record.agent;
        claim.check_agent_boot = true;
        claim.agent_boot = record.agent_boot;
        static_cast<void>(federation_->fence(record.accelerator, claim));
    }
}

Result<ResponseMessage> ControlPlane::handle_hello(const ByteBuffer& payload, ConnectionState& connection) {
    Result<HelloRequest> decoded = decode_hello_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    const HelloRequest& request = *decoded;

    HelloResponse response;
    response.server_version = config_.server_version;
    response.epoch = federation_->epoch();
    response.generation = federation_->generation();
    response.policy_generation = federation_->policy_generation();
    response.session = connection.id;

    if (request.protocol_version < kMinimumProtocolVersion || request.protocol_version > kProtocolVersion) {
        response.accepted = false;
        response.detail = "server supports protocol versions " + std::to_string(kMinimumProtocolVersion) + ".." +
                          std::to_string(kProtocolVersion) + " but the client offered " +
                          std::to_string(request.protocol_version);
        Result<ByteBuffer> encoded = encode_message(response);
        if (!encoded.ok()) {
            return encoded.status();
        }
        ResponseMessage message;
        message.type = MessageType::HelloAck;
        message.error = false;
        message.payload = *encoded;
        return message;
    }
    response.negotiated_version = std::min(request.protocol_version, kProtocolVersion);

    if (!request.federation.is_nil() && request.federation != federation_->id()) {
        response.accepted = false;
        response.detail = "client targets federation " + request.federation.to_string() + " but this coordinator owns " +
                          federation_->id().to_string();
        Result<ByteBuffer> encoded = encode_message(response);
        if (!encoded.ok()) {
            return encoded.status();
        }
        ResponseMessage message;
        message.type = MessageType::HelloAck;
        message.error = false;
        message.payload = *encoded;
        return message;
    }
    if (request.role == ClientRole::Agent) {
        if (request.agent.is_nil() || request.agent_boot.is_nil()) {
            response.accepted = false;
            // A refused handshake is a negotiated outcome, not a protocol
            // fault, so it is returned as a HelloAck with accepted=false
            // rather than as an error frame.
            response.detail = "an agent handshake must present a non-nil agent and boot identity";
            Result<ByteBuffer> encoded = encode_message(response);
            if (!encoded.ok()) {
                return encoded.status();
            }
            ResponseMessage message;
            message.type = MessageType::HelloAck;
            message.error = false;
            message.payload = *encoded;
            return message;
        }
        // Duplicate live boot: the same agent+boot handshaking twice while the
        // first session is still established is a contradictory federation
        // state and is refused.
        {
            std::lock_guard<std::mutex> guard(sessions_mutex_);
            const std::string self = key_for(connection);
            for (const auto& entry : sessions_) {
                if (entry.first == self) {
                    continue;
                }
                if (entry.second.role == ClientRole::Agent && entry.second.agent == request.agent &&
                    entry.second.agent_boot == request.agent_boot) {
                    response.accepted = false;
                    response.detail = "agent boot identity is already established on another live session";
                    Result<ByteBuffer> encoded = encode_message(response);
                    if (!encoded.ok()) {
                        return encoded.status();
                    }
                    ResponseMessage message;
                    message.type = MessageType::HelloAck;
                    message.error = false;
                    message.payload = *encoded;
                    return message;
                }
            }
        }
    }

    SessionData session;
    session.role = request.role;
    session.federation = request.federation;
    session.agent = request.agent;
    session.agent_boot = request.agent_boot;
    session.epoch = federation_->epoch();
    session.generation = federation_->generation();
    session.client_name = request.client_name;
    session.client_version = request.client_version;
    session.node_token = request.node_token;
    {
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        sessions_[key_for(connection)] = session;
    }

    response.accepted = true;
    response.detail = "session established as " + std::string(to_string(request.role)) + "; " +
                      generation_line(*federation_);
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    ResponseMessage message;
    message.type = MessageType::HelloAck;
    message.error = false;
    message.payload = *encoded;
    return message;
}

Result<ResponseMessage> ControlPlane::handle_advertise(const ByteBuffer& payload, ConnectionState& connection) {
    SessionData* session = nullptr;
    const Status session_status = require_agent_session(connection, session);
    if (!session_status.ok()) {
        return make_error(session_status.code(), session_status.message(), connection.peer);
    }
    const Status authority = validate_session_authority(*session);
    if (!authority.ok()) {
        return make_error(authority.code(), authority.message(), connection.peer);
    }
    Result<AdvertiseRequest> decoded = decode_advertise_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    AdvertiseRequest& request = *decoded;
    if (request.devices.empty()) {
        return make_error(ErrorCode::MalformedAdvertisement, "an advertisement must contain at least one accelerator",
                          connection.peer);
    }
    // The claim must name the session's own agent and boot. A peer cannot
    // advertise on behalf of another agent.
    if (request.claim.agent != session->agent || request.claim.agent_boot != session->agent_boot) {
        return make_error(ErrorCode::StaleBoot,
                          "advertisement claim does not match the identity established during the handshake",
                          connection.peer);
    }
    // The claim must be internally consistent with the descriptors.
    for (const AcceleratorDescriptor& descriptor : request.devices) {
        if (descriptor.agent != session->agent || descriptor.agent_boot != session->agent_boot) {
            return make_error(ErrorCode::StaleBoot,
                              "advertised accelerator is owned by a different agent or boot identity",
                              descriptor.id.to_string());
        }
    }

    AdvertiseResponse response;
    const FederationPolicy policy = federation_->policy();
    for (const AcceleratorDescriptor& descriptor : request.devices) {
        AuthorityClaim claim = request.claim;
        claim.check_policy_generation = false;
        Result<MemberRecord> observed = federation_->observe(descriptor, claim);
        if (!observed.ok()) {
            return make_error(observed.status().code(), observed.status().message(), descriptor.id.to_string());
        }
        if (observed->state == MemberState::Observed) {
            AuthorityClaim admit_claim = claim;
            Result<MemberRecord> admitted = federation_->admit(descriptor.id, admit_claim);
            if (!admitted.ok()) {
                return make_error(admitted.status().code(), admitted.status().message(), descriptor.id.to_string());
            }
            observed = admitted;
        }
        if (observed->state == MemberState::Admitted) {
            if (policy.require_real_evidence_for_active && observed->provenance != EvidenceProvenance::Real) {
                response.detail += "member " + descriptor.id.to_string() +
                                   " admitted but not activated: policy requires REAL evidence for active membership; ";
            } else {
                AuthorityClaim activate_claim = claim;
                Result<MemberRecord> activated = federation_->activate(descriptor.id, activate_claim);
                if (!activated.ok()) {
                    return make_error(activated.status().code(), activated.status().message(),
                                      descriptor.id.to_string());
                }
                observed = activated;
            }
        }
        if (observed->state == MemberState::Degraded) {
            AuthorityClaim activate_claim = claim;
            Result<MemberRecord> activated = federation_->activate(descriptor.id, activate_claim);
            if (!activated.ok()) {
                return make_error(activated.status().code(), activated.status().message(),
                                  descriptor.id.to_string());
            }
            observed = activated;
        }
        session->generation = federation_->generation();
        response.members.push_back(*observed);
    }
    if (response.detail.empty()) {
        response.detail = "advertisement accepted for " + std::to_string(request.devices.size()) + " accelerator(s)";
    } else {
        response.detail += "advertisement accepted for " + std::to_string(request.devices.size()) + " accelerator(s)";
    }
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return make_response(MessageType::AdvertiseAck, *encoded);
}

Result<ResponseMessage> ControlPlane::handle_query(const ByteBuffer& payload, ConnectionState& connection) {
    SessionData* session = nullptr;
    const Status session_status = require_session(connection, session);
    if (!session_status.ok()) {
        return make_error(session_status.code(), session_status.message(), connection.peer);
    }
    const Status authority = validate_session_authority(*session);
    if (!authority.ok()) {
        return make_error(authority.code(), authority.message(), connection.peer);
    }
    Result<QueryRequest> decoded = decode_query_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    QueryResponse response;
    response.kind = decoded->kind;
    response.text = render_query(*decoded);
    response.item_count = static_cast<std::uint32_t>(federation_->member_count());
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return make_response(MessageType::QueryResponse, *encoded);
}

std::string ControlPlane::render_query(const QueryRequest& request) const {
    const QueryKind kind = request.kind;
    const AcceleratorId& accelerator = request.accelerator;
    switch (kind) {
        case QueryKind::FederationSummary: {
            std::ostringstream stream;
            stream << "federation " << federation_->id().to_string() << " name=" << federation_->name() << " "
                   << generation_line(*federation_) << " members=" << federation_->member_count()
                   << " workloads=" << federation_->workloads().size()
                   << " store=" << federation_->store_location();
            return stream.str();
        }
        case QueryKind::MemberList: {
            Result<std::string> rendered = federation_->render_members();
            return rendered.ok() ? *rendered : rendered.status().describe();
        }
        case QueryKind::MemberDetail:
        case QueryKind::MemberExplain: {
            Result<std::string> rendered = federation_->explain_member(accelerator);
            return rendered.ok() ? *rendered : rendered.status().describe();
        }
        case QueryKind::Policy: {
            const FederationPolicy policy = federation_->policy();
            std::ostringstream stream;
            stream << "policy " << policy.id.to_string() << " name=" << policy.name
                   << " generation=" << policy.generation.value()
                   << " synthetic_evidence=" << to_string(policy.synthetic_evidence_policy)
                   << " require_real_for_active=" << (policy.require_real_evidence_for_active ? "yes" : "no")
                   << " allow_cross_vendor_migration=" << (policy.allow_cross_vendor_migration ? "yes" : "no")
                   << " allow_cross_vendor_reconstruction=" << (policy.allow_cross_vendor_reconstruction ? "yes" : "no")
                   << " allow_degraded_capabilities=" << (policy.allow_degraded_capabilities ? "yes" : "no")
                   << " evidence_freshness_nanos=" << policy.required_evidence_freshness_nanos
                   << " minimum_memory_bytes=" << policy.minimum_memory_bytes
                   << " minimum_runtime_version=" << policy.minimum_runtime_version.to_string();
            auto append = [&stream](const char* label, const std::vector<std::string>& values) {
                stream << " " << label << "=[";
                for (std::size_t i = 0; i < values.size(); ++i) {
                    if (i != 0) {
                        stream << ",";
                    }
                    stream << values[i];
                }
                stream << "]";
            };
            append("allowed_vendors", policy.allowed_vendors);
            append("forbidden_vendors", policy.forbidden_vendors);
            append("deprecated_architectures", policy.deprecated_architectures);
            append("required_capabilities", policy.required_capabilities);
            append("denied_capabilities", policy.denied_capabilities);
            append("tags", policy.tags);
            stream << " allowed_portability=[";
            for (std::size_t i = 0; i < policy.allowed_portability_classes.size(); ++i) {
                if (i != 0) {
                    stream << ",";
                }
                stream << to_string(policy.allowed_portability_classes[i]);
            }
            stream << "]";
            return stream.str();
        }
        case QueryKind::CapabilityList: {
            std::ostringstream stream;
            stream << "capability vocabulary (" << all_core_capability_keys().size() << " core keys)";
            for (const CapabilityKey& key : all_core_capability_keys()) {
                stream << "\n" << key.name() << " " << to_string(key.kind());
            }
            return stream.str();
        }
        case QueryKind::Audit: {
            const AuditReport report = federation_->audit();
            return report.render();
        }
        case QueryKind::StoreVerify: {
            const FederationSnapshot snapshot = federation_->snapshot();
            std::ostringstream stream;
            stream << "store=" << federation_->store_location() << " digest=" << snapshot.digest_hex()
                   << " members=" << snapshot.members.size() << " decisions=" << snapshot.decisions.size()
                   << " plans=" << snapshot.plans.size();
            return stream.str();
        }
        case QueryKind::AdapterAvailability: {
            std::ostringstream stream;
            stream << "adapter availability";
            for (const adapters::AdapterAvailability& availability : config_.adapters) {
                stream << "\n" << availability.name << " support=" << to_string(availability.support_level)
                       << " evidence=" << to_string(availability.provenance)
                       << " available=" << (availability.available ? "yes" : "no") << " detail="
                       << availability.detail;
            }
            return stream.str();
        }
        case QueryKind::WorkloadList: {
            std::ostringstream stream;
            stream << "registered workloads";
            for (const WorkloadProfile& profile : federation_->workloads()) {
                stream << "\n" << profile.name << " class=" << profile.class_id.to_string()
                       << " revision=" << profile.revision.value() << " hard=" << profile.hard_requirement_count()
                       << " soft=" << profile.soft_requirement_count()
                       << " execution_mode=" << to_string(profile.execution_mode)
                       << " min_portability=" << to_string(profile.minimum_portability)
                       << " migration_need=" << to_string(profile.migration_need);
            }
            return stream.str();
        }
        case QueryKind::PlanList: {
            std::ostringstream stream;
            stream << "migration plans";
            for (const MigrationPlan& plan : federation_->migration_plans()) {
                stream << "\n" << plan.render();
            }
            return stream.str();
        }
        case QueryKind::CompatibilityMatrix: {
            std::ostringstream stream;
            stream << "compatibility matrix";
            const std::vector<WorkloadProfile> profiles = federation_->workloads();
            const std::vector<AcceleratorId> accelerators = federation_->member_ids();
            const bool filtered = request.has_workload && !request.workload.is_nil();
            if (profiles.empty()) {
                stream << "\n(no workloads are registered)";
                return stream.str();
            }
            if (accelerators.empty()) {
                stream << "\n(no federation members)";
                return stream.str();
            }
            const std::size_t workload_count = filtered ? 1U : profiles.size();
            if (workload_count * accelerators.size() > Limits::kMaxMatrixCells) {
                stream << "\n(matrix of " << workload_count << " workloads x " << accelerators.size()
                       << " accelerators exceeds the permitted cell count; narrow the query)";
                return stream.str();
            }
            std::vector<CompatibilityDecision> decisions;
            decisions.reserve(workload_count * accelerators.size());
            for (const WorkloadProfile& profile : profiles) {
                if (filtered && profile.class_id != request.workload) {
                    continue;
                }
                for (const AcceleratorId& id : accelerators) {
                    Result<CompatibilityDecision> decision =
                        federation_->evaluate_transient(profile.class_id, id);
                    if (!decision.ok()) {
                        stream << "\n" << profile.name << " " << id.to_string() << " ERROR "
                               << decision.status().describe();
                        continue;
                    }
                    decisions.push_back(*decision);
                }
            }
            stream << " cells=" << decisions.size();
            for (const CompatibilityDecision& decision : decisions) {
                stream << "\n" << decision.workload.to_string() << " | " << decision.accelerator.to_string() << " | "
                       << to_string(decision.outcome) << " | portability=" << to_string(decision.portability)
                       << " | evidence=" << to_string(decision.provenance)
                       << " | support=" << to_string(decision.support_level)
                       << " | federation_generation=" << decision.federation_generation.value()
                       << " | epoch=" << decision.epoch.value()
                       << " | device_generation=" << decision.device_generation.value()
                       << " | capability_generation=" << decision.capability_generation.value()
                       << " | evidence_generation=" << decision.evidence_generation.value()
                       << " | policy_generation=" << decision.policy_generation.value()
                       << " | workload_revision=" << decision.workload_revision.value()
                       << " | fingerprint=" << decision.fingerprint_hex();
                for (const CompatibilityReason& reason : decision.reasons) {
                    if (reason.code == ErrorCode::Ok) {
                        continue;
                    }
                    stream << "\n    blocked_by " << to_string(reason.code) << " " << reason.subject << " ("
                           << to_string(reason.strength) << "): " << reason.detail;
                }
            }
            return stream.str();
        }
        case QueryKind::PortabilityExplain: {
            std::ostringstream stream;
            stream << "portability";
            if (accelerator.is_nil() || request.secondary.is_nil()) {
                stream << "\n(a source and a destination accelerator are required)";
                return stream.str();
            }
            Result<AcceleratorDescriptor> source = federation_->descriptor(accelerator);
            Result<AcceleratorDescriptor> destination = federation_->descriptor(request.secondary);
            if (!source.ok()) {
                stream << "\n" << source.status().describe();
                return stream.str();
            }
            if (!destination.ok()) {
                stream << "\n" << destination.status().describe();
                return stream.str();
            }
            const CapabilityLookup source_vendor = source->capabilities.lookup(cap::kVendorId);
            const CapabilityLookup destination_vendor = destination->capabilities.lookup(cap::kVendorId);
            PortabilityRequest portability_request;
            portability_request.source_capabilities = &source->capabilities;
            portability_request.destination_capabilities = &destination->capabilities;
            portability_request.source_vendor =
                source_vendor.present && source_vendor.value != nullptr ? source_vendor.value->render() : std::string();
            portability_request.destination_vendor = destination_vendor.present && destination_vendor.value != nullptr
                                                         ? destination_vendor.value->render()
                                                         : std::string();
            const FederationPolicy policy = federation_->policy();
            portability_request.policy_allows_cross_vendor_migration = policy.allow_cross_vendor_migration;
            const PortabilityResult result = classify_portability(portability_request);
            stream << "\nfrom=" << accelerator.to_string() << " to=" << request.secondary.to_string()
                   << " class=" << to_string(result.value)
                   << " strength=" << static_cast<int>(portability_strength(result.value))
                   << " transformation_steps=" << static_cast<int>(portability_transformation_steps(result.value))
                   << " requires_binary_transformation="
                   << (requires_binary_transformation(result.value) ? "yes" : "no")
                   << " requires_state_reconstruction="
                   << (requires_state_reconstruction(result.value) ? "yes" : "no")
                   << " implies_live_state_transfer="
                   << (implies_live_state_transfer(result.value) ? "yes" : "no");
            for (const CompatibilityReason& reason : result.reasons) {
                stream << "\n  reason " << to_string(reason.code) << " " << reason.subject << ": " << reason.detail;
            }
            stream << "\n  source_evidence=" << to_string(source->provenance)
                   << " destination_evidence=" << to_string(destination->provenance);
            return stream.str();
        }
        case QueryKind::RecoveryReport: {
            const RecoveryReport& report = federation_->recovery_report();
            std::ostringstream stream;
            stream << "recovered=" << (report.recovered ? "yes" : "no") << " previous_epoch="
                   << report.previous_epoch.value() << " epoch=" << report.new_epoch.value()
                   << " generation=" << report.new_generation.value()
                   << " members_restored=" << report.members_restored
                   << " requiring_revalidation=" << report.members_requiring_revalidation
                   << " decisions_invalidated=" << report.decisions_invalidated
                   << " plans_aborted=" << report.plans_aborted << " detail=" << report.detail;
            return stream.str();
        }
    }
    return "unknown query";
}

Result<ResponseMessage> ControlPlane::handle_evaluate(const ByteBuffer& payload, ConnectionState& connection) {
    SessionData* session = nullptr;
    const Status session_status = require_session(connection, session);
    if (!session_status.ok()) {
        return make_error(session_status.code(), session_status.message(), connection.peer);
    }
    const Status authority = validate_session_authority(*session);
    if (!authority.ok()) {
        return make_error(authority.code(), authority.message(), connection.peer);
    }
    Result<EvaluateRequest> decoded = decode_evaluate_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    EvaluationOptions options;
    if (decoded->has_source) {
        options.source = decoded->source;
    }
    Result<CompatibilityDecision> decision =
        federation_->evaluate(decoded->workload, decoded->accelerator, options);
    if (!decision.ok()) {
        return make_error(decision.status().code(), decision.status().message(), connection.peer);
    }
    EvaluateResponse response;
    response.decision = *decision;
    response.rendered = decision->render();
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return make_response(MessageType::EvaluateResponse, *encoded);
}

Result<ResponseMessage> ControlPlane::handle_plan(const ByteBuffer& payload, ConnectionState& connection) {
    SessionData* session = nullptr;
    const Status session_status = require_session(connection, session);
    if (!session_status.ok()) {
        return make_error(session_status.code(), session_status.message(), connection.peer);
    }
    const Status authority = validate_session_authority(*session);
    if (!authority.ok()) {
        return make_error(authority.code(), authority.message(), connection.peer);
    }
    Result<PlanMigrationRequest> decoded = decode_plan_migration_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    AuthorityClaim claim = decoded->claim;
    claim.check_agent = false;
    claim.check_agent_boot = false;
    Result<MigrationPlan> plan =
        federation_->plan_migration(decoded->workload, decoded->source, decoded->destination, claim);
    if (!plan.ok()) {
        return make_error(plan.status().code(), plan.status().message(), connection.peer);
    }
    PlanMigrationResponse response;
    response.plan = *plan;
    response.rendered = plan->render();
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return make_response(MessageType::PlanMigrationResponse, *encoded);
}

Result<ResponseMessage> ControlPlane::handle_transition(const ByteBuffer& payload, ConnectionState& connection) {
    SessionData* session = nullptr;
    const Status session_status = require_agent_session(connection, session);
    if (!session_status.ok()) {
        return make_error(session_status.code(), session_status.message(), connection.peer);
    }
    const Status authority = validate_session_authority(*session);
    if (!authority.ok()) {
        return make_error(authority.code(), authority.message(), connection.peer);
    }
    Result<TransitionRequest> decoded = decode_transition_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    AuthorityClaim claim = decoded->claim;
    if (claim.agent != session->agent || claim.agent_boot != session->agent_boot) {
        return make_error(ErrorCode::StaleBoot,
                          "transition claim does not match the identity established during the handshake",
                          connection.peer);
    }
    Result<MemberRecord> member = federation_->transition(decoded->accelerator, decoded->target, claim);
    if (!member.ok()) {
        return make_error(member.status().code(), member.status().message(), connection.peer);
    }
    TransitionResponse response;
    response.member = *member;
    response.detail = "member transitioned to " + std::string(to_string(member->state));
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return make_response(MessageType::TransitionResponse, *encoded);
}

Result<ResponseMessage> ControlPlane::handle_heartbeat(const ByteBuffer& payload, ConnectionState& connection) {
    SessionData* session = nullptr;
    const Status session_status = require_session(connection, session);
    if (!session_status.ok()) {
        return make_error(session_status.code(), session_status.message(), connection.peer);
    }
    Result<HeartbeatRequest> decoded = decode_heartbeat_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    HeartbeatResponse response;
    response.epoch = federation_->epoch();
    response.generation = federation_->generation();
    response.policy_generation = federation_->policy_generation();
    response.authority_still_valid = true;

    // A heartbeat that presents stale authority is refused with a typed error,
    // which is how a restarted coordinator fences pre-restart sessions.
    if (decoded->claim.check_epoch && decoded->claim.epoch != federation_->epoch()) {
        return make_error(ErrorCode::StaleEpoch,
                          "heartbeat presents coordinator epoch " +
                              std::to_string(decoded->claim.epoch.value()) + " but the current epoch is " +
                              std::to_string(federation_->epoch().value()));
    }
    if (decoded->claim.check_federation_generation &&
        decoded->claim.federation_generation != federation_->generation()) {
        return make_error(ErrorCode::StaleFederationGeneration,
                          "heartbeat presents federation generation " +
                              std::to_string(decoded->claim.federation_generation.value()) +
                              " but the current generation is " +
                              std::to_string(federation_->generation().value()));
    }
    if (session->role == ClientRole::Agent &&
        (decoded->claim.agent != session->agent || decoded->claim.agent_boot != session->agent_boot)) {
        return make_error(ErrorCode::StaleBoot,
                          "heartbeat presents an agent boot identity that differs from the established session");
    }
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return make_response(MessageType::HeartbeatAck, *encoded);
}

Result<ResponseMessage> ControlPlane::handle_register_workload(const ByteBuffer& payload,
                                                                  ConnectionState& connection) {
    SessionData* session = nullptr;
    const Status session_status = require_session(connection, session);
    if (!session_status.ok()) {
        return make_error(session_status.code(), session_status.message(), connection.peer);
    }
    const Status authority = validate_session_authority(*session);
    if (!authority.ok()) {
        return make_error(authority.code(), authority.message(), connection.peer);
    }
    Result<RegisterWorkloadRequest> decoded = decode_register_workload_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    AuthorityClaim claim = decoded->claim;
    claim.check_agent = false;
    claim.check_agent_boot = false;
    const Result<WorkloadRevision> revision = federation_->register_workload(decoded->profile, claim);
    if (!revision.ok()) {
        return make_error(revision.status().code(), revision.status().message(), connection.peer);
    }
    RegisterWorkloadResponse response;
    response.class_id = decoded->profile.class_id;
    response.revision = *revision;
    response.detail = "workload registered with revision " + std::to_string(revision->value());
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return make_response(MessageType::RegisterWorkloadAck, *encoded);
}

Result<ResponseMessage> ControlPlane::handle_goodbye(const ByteBuffer& payload, ConnectionState& connection) {
    Result<GoodbyeRequest> decoded = decode_goodbye_request(payload);
    if (!decoded.ok()) {
        return make_error(decoded.status().code(), decoded.status().message(), connection.peer);
    }
    {
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        sessions_.erase(key_for(connection));
    }
    TransitionResponse response;
    response.detail = "session closed: " + decoded->reason;
    Result<ByteBuffer> encoded = encode_message(response);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return make_response(MessageType::TransitionResponse, *encoded);
}

}  // namespace haf::net

// haf_cli - deterministic inspection and exercise surface for a federation.
//
// The CLI is an inspector: it never mutates federation state. Every command
// produces deterministic text so that output can be diffed across runs.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "app_support.hpp"
#include "haf/adapters/registry.hpp"
#include "haf/adapters/synthetic.hpp"
#include "haf/engine/audit.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/federation/federation.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/net/client.hpp"
#include "haf/persist/file_store.hpp"

namespace {

using namespace haf;
using namespace haf::app;

const std::vector<std::string> kValueFlags = {
    "--connect", "--store", "--accelerator", "--from", "--to", "--workload", "--seed", "--name"};

int usage() {
    std::cout
        << "haf_cli [--connect HOST:PORT | --store PATH] <command> [arguments]\n"
           "\n"
           "Inspection commands:\n"
           "  federation show                       federation identity, generations, and store\n"
           "  member list                           all federation members\n"
           "  member show <accelerator-id>          one member's record\n"
           "  member explain <accelerator-id>       full explanation of one member\n"
           "  capability list                       the federation capability vocabulary\n"
           "  policy show                           the active federation policy\n"
           "  workload list                         registered workload requirement profiles\n"
           "  plan list                             recorded migration plans\n"
           "  adapters                              adapter availability (REAL/SYNTHETIC/UNSUPPORTED)\n"
           "  recovery                              conservative recovery report\n"
           "  audit                                 invariant audit; zero violations expected\n"
           "  snapshot verify [--store PATH]        verify a durable store without a coordinator\n"
           "\n"
           "Decision commands:\n"
           "  workload evaluate <workload-class-id> [--accelerator ID] [--from ID]\n"
           "  compatibility matrix [--workload ID]\n"
           "  portability explain --from ID --to ID\n"
           "  migration plan <workload-class-id> --from ID --to ID\n"
           "  demo                                  run a self-contained federation demonstration\n";
    return 0;
}

struct Session {
    net::ControlClient client;
};

/// Resolve a user-supplied identifier: a 32-hex identity, or a canonical
/// taxonomy token that is hashed into the same identity space.
template <class Tag>
Result<StrongId<Tag>> resolve_id(const std::string& text, const char* domain) {
    if (text.size() == 32) {
        return parse_id<Tag>(text);
    }
    const Result<std::string> token = canonical_token(text, Limits::kMaxTokenBytes);
    if (!token.ok()) {
        return Status(ErrorCode::InvalidArgument, std::string("cannot interpret identifier '") + text + "'");
    }
    return StrongId<Tag>::from_raw(derive_identity(domain, *token));
}

template <class Tag>
Result<StrongId<Tag>> resolve_id_or(const std::string& text, const char* domain) {
    return resolve_id<Tag>(text, domain);
}

Result<net::ClientResponse> send(net::ControlClient& client, net::MessageType type, const ByteBuffer& payload) {
    return client.request(type, payload);
}

int report_error(const net::ClientResponse& response) {
    const Result<net::ErrorResponse> error = net::ControlClient::decode_error(response);
    if (error.ok()) {
        std::cout << "error code=" << to_string(error->code) << " message=" << error->message;
        if (!error->detail.empty()) {
            std::cout << " detail=" << error->detail;
        }
        std::cout << std::endl;
    } else {
        std::cout << "error (unparseable error response)" << std::endl;
    }
    return 1;
}

int run_query(net::ControlClient& client, net::QueryRequest request) {
    Result<ByteBuffer> payload = net::encode_message(request);
    if (!payload.ok()) {
        std::cout << "cannot encode query: " << payload.status().describe() << std::endl;
        return 1;
    }
    Result<net::ClientResponse> response = send(client, net::MessageType::Query, *payload);
    if (!response.ok()) {
        std::cout << "query failed: " << response.status().describe() << std::endl;
        return 1;
    }
    if (response->error) {
        return report_error(*response);
    }
    Result<net::QueryResponse> decoded = net::decode_query_response(response->payload);
    if (!decoded.ok()) {
        std::cout << "cannot decode query response: " << decoded.status().describe() << std::endl;
        return 1;
    }
    std::cout << decoded->text << std::endl;
    return 0;
}

int verify_store(const std::string& path) {
    if (path.empty()) {
        std::cout << "snapshot verify requires --store PATH" << std::endl;
        return 2;
    }
    FileStore store(path);
    const Result<ByteBuffer> payload = store.read();
    if (!payload.ok()) {
        std::cout << "store=" << store.location() << " status=REJECTED reason=" << payload.status().describe()
                  << std::endl;
        return 1;
    }
    const Result<FederationSnapshot> decoded = decode_snapshot(*payload);
    if (!decoded.ok()) {
        std::cout << "store=" << store.location() << " status=REJECTED reason=" << decoded.status().describe()
                  << std::endl;
        return 1;
    }
    const AuditReport report = audit_snapshot(*decoded);
    std::cout << "store=" << store.location() << " status=VERIFIED"
              << " federation=" << decoded->federation.to_string() << " generation=" << decoded->generation.value()
              << " epoch=" << decoded->epoch.value() << " members=" << decoded->members.size()
              << " workloads=" << decoded->workloads.size() << " digest=" << decoded->digest_hex() << std::endl;
    std::cout << report.render() << std::endl;
    return report.ok() ? 0 : 1;
}

int run_demo(const std::string& store_path) {
    // A self-contained demonstration that exercises the same public API a
    // downstream consumer would use, with no coordinator involved.
    FederationConfig config;
    config.name = "demo-federation";
    config.store_path = store_path;
    config.id_seed = 7;
    config.persist = !store_path.empty();
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    if (!federation.ok()) {
        std::cout << "cannot open federation: " << federation.status().describe() << std::endl;
        return 1;
    }
    std::cout << "demo federation=" << (*federation)->id().to_string()
              << " epoch=" << (*federation)->epoch().value()
              << " generation=" << (*federation)->generation().value() << std::endl;

    adapters::AdapterContext context;
    context.agent = AgentId::from_raw(derive_identity("haf.demo.agent", "demo-agent"));
    context.agent_boot = AgentBootId::from_raw(derive_identity("haf.demo.boot", "demo-boot"));
    context.node_token = "demo-node";
    context.node = node_id_from_token(context.node_token);

    const std::vector<adapters::DeviceProfile> profiles = {adapters::cuda_class_profile(),
                                                           adapters::rocm_class_profile()};
    std::vector<AcceleratorId> ids;
    for (const adapters::DeviceProfile& profile : profiles) {
        Result<AcceleratorDescriptor> descriptor =
            adapters::build_descriptor(profile, context, "demo-adapter", 0);
        if (!descriptor.ok()) {
            std::cout << "cannot build descriptor: " << descriptor.status().describe() << std::endl;
            return 1;
        }
        const AuthorityClaim claim = (*federation)->epoch_claim("demo advertisement");
        Result<MemberRecord> observed = (*federation)->observe(*descriptor, claim);
        if (!observed.ok()) {
            std::cout << "observe failed: " << observed.status().describe() << std::endl;
            return 1;
        }
        Result<MemberRecord> admitted = (*federation)->admit(descriptor->id, claim);
        if (!admitted.ok()) {
            std::cout << "admit failed: " << admitted.status().describe() << std::endl;
            return 1;
        }
        Result<MemberRecord> active = (*federation)->activate(descriptor->id, claim);
        if (!active.ok()) {
            std::cout << "activate failed: " << active.status().describe() << std::endl;
            return 1;
        }
        ids.push_back(descriptor->id);
        std::cout << "admitted " << descriptor->id.to_string() << " vendor=" << descriptor->vendor_token
                  << " architecture=" << descriptor->architecture_token
                  << " evidence=" << to_string(descriptor->provenance) << std::endl;
    }

    WorkloadProfile workload;
    workload.name = "demo-fp8-decode";
    workload.description = "requires fp8 tensor math and a CUDA code object target";
    workload.class_id = workload_class_id_from_token(workload.name);
    workload.requirement_id = derive_requirement_id(workload);
    std::vector<CapabilityRequirement> requirements;
    for (Result<CapabilityRequirement> requirement :
         {require_equals(cap::kVendorId, "nvidia", RequirementStrength::Hard),
          require_present(cap::kNumericFp8E4m3, RequirementStrength::Hard),
          require_tokens_superset(cap::kIsaCodeObjectTargets, {"sm-120"}, RequirementStrength::Hard),
          require_at_least(cap::kMemoryTotalBytes, 8LL * 1024 * 1024 * 1024, RequirementStrength::Hard)}) {
        if (!requirement.ok()) {
            std::cout << "cannot build requirement: " << requirement.status().describe() << std::endl;
            return 1;
        }
        requirements.push_back(*requirement);
    }
    workload.requirements = std::move(requirements);
    const AuthorityClaim claim = (*federation)->epoch_claim("demo workload");
    Result<WorkloadRevision> revision = (*federation)->register_workload(workload, claim);
    if (!revision.ok()) {
        std::cout << "register workload failed: " << revision.status().describe() << std::endl;
        return 1;
    }
    std::cout << "workload " << workload.class_id.to_string() << " revision=" << revision->value() << std::endl;

    for (const AcceleratorId& id : ids) {
        Result<CompatibilityDecision> decision =
            (*federation)->evaluate(workload.class_id, id, EvaluationOptions{});
        if (!decision.ok()) {
            std::cout << "evaluate failed: " << decision.status().describe() << std::endl;
            return 1;
        }
        std::cout << decision->render() << std::endl;
    }

    Result<MigrationPlan> plan =
        (*federation)->plan_migration(workload.class_id, ids[0], ids[1], (*federation)->epoch_claim("demo plan"));
    if (!plan.ok()) {
        std::cout << "plan failed: " << plan.status().describe() << std::endl;
        return 1;
    }
    std::cout << plan->render() << std::endl;

    const AuditReport report = (*federation)->audit();
    std::cout << report.render() << std::endl;
    return report.ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Arguments arguments(argc, argv);
    if (argc <= 1 || arguments.has("--help") || arguments.has("-h")) {
        return usage();
    }

    const std::vector<std::string> positional = arguments.positional(kValueFlags);
    if (positional.empty()) {
        return usage();
    }
    const std::string& command = positional[0];

    const std::string store_path = arguments.string_value("--store", "");
    if (command == "snapshot" && positional.size() >= 2 && positional[1] == "verify") {
        return verify_store(store_path);
    }
    if (command == "demo") {
        return run_demo(store_path);
    }

    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    const std::string endpoint = arguments.string_value("--connect", "");
    if (endpoint.empty()) {
        std::cout << "this command requires --connect HOST:PORT" << std::endl;
        return 2;
    }
    if (!parse_endpoint(endpoint, host, port)) {
        std::cout << "invalid --connect endpoint: " << endpoint << std::endl;
        return 2;
    }

    net::ClientConfig client_config;
    client_config.host = host;
    client_config.port = port;
    client_config.name = "haf_cli";
    client_config.version = "1.0.0";
    Result<net::ControlClient> client = net::ControlClient::connect(client_config);
    if (!client.ok()) {
        std::cout << "cannot connect to coordinator: " << client.status().describe() << std::endl;
        return 3;
    }

    net::HelloRequest hello;
    hello.protocol_version = net::kProtocolVersion;
    hello.role = net::ClientRole::Inspector;
    hello.client_name = "haf_cli";
    hello.client_version = "1.0.0";
    hello.nonce = 1;
    {
        Result<ByteBuffer> payload = net::encode_message(hello);
        if (!payload.ok()) {
            return 1;
        }
        Result<net::ClientResponse> response = send(*client, net::MessageType::Hello, *payload);
        if (!response.ok()) {
            std::cout << "handshake failed: " << response.status().describe() << std::endl;
            return 3;
        }
        if (response->error) {
            return report_error(*response);
        }
    }

    auto resolve_accelerator = [&](const std::string& text) -> Result<AcceleratorId> {
        return resolve_id<AcceleratorIdTag>(text, "haf.accelerator");
    };

    int exit_code = 0;
    if (command == "federation" && positional.size() >= 2 && positional[1] == "show") {
        net::QueryRequest request;
        request.kind = net::QueryKind::FederationSummary;
        exit_code = run_query(*client, request);
    } else if (command == "member" && positional.size() >= 2 && positional[1] == "list") {
        net::QueryRequest request;
        request.kind = net::QueryKind::MemberList;
        exit_code = run_query(*client, request);
    } else if (command == "member" && positional.size() >= 3 && positional[1] == "show") {
        Result<AcceleratorId> id = resolve_accelerator(positional[2]);
        if (!id.ok()) {
            std::cout << id.status().describe() << std::endl;
            return 2;
        }
        net::QueryRequest request;
        request.kind = net::QueryKind::MemberDetail;
        request.accelerator = *id;
        exit_code = run_query(*client, request);
    } else if (command == "member" && positional.size() >= 3 && positional[1] == "explain") {
        Result<AcceleratorId> id = resolve_accelerator(positional[2]);
        if (!id.ok()) {
            std::cout << id.status().describe() << std::endl;
            return 2;
        }
        net::QueryRequest request;
        request.kind = net::QueryKind::MemberExplain;
        request.accelerator = *id;
        exit_code = run_query(*client, request);
    } else if (command == "capability" && positional.size() >= 2 && positional[1] == "list") {
        net::QueryRequest request;
        request.kind = net::QueryKind::CapabilityList;
        exit_code = run_query(*client, request);
    } else if (command == "policy" && positional.size() >= 2 && positional[1] == "show") {
        net::QueryRequest request;
        request.kind = net::QueryKind::Policy;
        exit_code = run_query(*client, request);
    } else if (command == "workload" && positional.size() >= 2 && positional[1] == "list") {
        net::QueryRequest request;
        request.kind = net::QueryKind::WorkloadList;
        exit_code = run_query(*client, request);
    } else if (command == "plan" && positional.size() >= 2 && positional[1] == "list") {
        net::QueryRequest request;
        request.kind = net::QueryKind::PlanList;
        exit_code = run_query(*client, request);
    } else if (command == "adapters") {
        net::QueryRequest request;
        request.kind = net::QueryKind::AdapterAvailability;
        exit_code = run_query(*client, request);
    } else if (command == "recovery") {
        net::QueryRequest request;
        request.kind = net::QueryKind::RecoveryReport;
        exit_code = run_query(*client, request);
    } else if (command == "audit") {
        net::QueryRequest request;
        request.kind = net::QueryKind::Audit;
        exit_code = run_query(*client, request);
    } else if (command == "compatibility" && positional.size() >= 2 && positional[1] == "matrix") {
        net::QueryRequest request;
        request.kind = net::QueryKind::CompatibilityMatrix;
        const std::string workload_text = arguments.string_value("--workload", "");
        if (!workload_text.empty()) {
            Result<WorkloadClassId> id = resolve_id<WorkloadClassIdTag>(workload_text, "haf.workload-class");
            if (!id.ok()) {
                std::cout << id.status().describe() << std::endl;
                return 2;
            }
            request.workload = *id;
            request.has_workload = true;
        }
        exit_code = run_query(*client, request);
    } else if (command == "portability" && positional.size() >= 2 && positional[1] == "explain") {
        Result<AcceleratorId> from = resolve_accelerator(arguments.string_value("--from", ""));
        Result<AcceleratorId> to = resolve_accelerator(arguments.string_value("--to", ""));
        if (!from.ok() || !to.ok()) {
            std::cout << "portability explain requires --from ID and --to ID" << std::endl;
            return 2;
        }
        net::QueryRequest request;
        request.kind = net::QueryKind::PortabilityExplain;
        request.accelerator = *from;
        request.secondary = *to;
        exit_code = run_query(*client, request);
    } else if (command == "workload" && positional.size() >= 3 && positional[1] == "evaluate") {
        Result<WorkloadClassId> workload = resolve_id<WorkloadClassIdTag>(positional[2], "haf.workload-class");
        if (!workload.ok()) {
            std::cout << workload.status().describe() << std::endl;
            return 2;
        }
        const std::string accelerator_text = arguments.string_value("--accelerator", "");
        const std::string source_text = arguments.string_value("--from", "");
        if (accelerator_text.empty()) {
            std::cout << "workload evaluate requires --accelerator ID (use 'compatibility matrix' for the fleet)"
                      << std::endl;
            return 2;
        }
        Result<AcceleratorId> accelerator = resolve_accelerator(accelerator_text);
        if (!accelerator.ok()) {
            std::cout << accelerator.status().describe() << std::endl;
            return 2;
        }
        net::EvaluateRequest request;
        request.workload = *workload;
        request.accelerator = *accelerator;
        if (!source_text.empty()) {
            Result<AcceleratorId> source = resolve_accelerator(source_text);
            if (!source.ok()) {
                std::cout << source.status().describe() << std::endl;
                return 2;
            }
            request.has_source = true;
            request.source = *source;
        }
        Result<ByteBuffer> payload = net::encode_message(request);
        if (!payload.ok()) {
            return 1;
        }
        Result<net::ClientResponse> response = send(*client, net::MessageType::Evaluate, *payload);
        if (!response.ok()) {
            std::cout << "evaluate failed: " << response.status().describe() << std::endl;
            return 1;
        }
        if (response->error) {
            return report_error(*response);
        }
        Result<net::EvaluateResponse> decoded = net::decode_evaluate_response(response->payload);
        if (!decoded.ok()) {
            std::cout << "cannot decode evaluate response: " << decoded.status().describe() << std::endl;
            return 1;
        }
        std::cout << decoded->rendered << std::endl;
    } else if (command == "migration" && positional.size() >= 3 && positional[1] == "plan") {
        Result<WorkloadClassId> workload = resolve_id<WorkloadClassIdTag>(positional[2], "haf.workload-class");
        Result<AcceleratorId> from = resolve_accelerator(arguments.string_value("--from", ""));
        Result<AcceleratorId> to = resolve_accelerator(arguments.string_value("--to", ""));
        if (!workload.ok() || !from.ok() || !to.ok()) {
            std::cout << "migration plan requires a workload class id, --from ID, and --to ID" << std::endl;
            return 2;
        }
        net::PlanMigrationRequest request;
        request.workload = *workload;
        request.source = *from;
        request.destination = *to;
        request.claim.check_epoch = false;
        request.claim.check_federation_generation = false;
        request.claim.purpose = "cli migration plan";
        Result<ByteBuffer> payload = net::encode_message(request);
        if (!payload.ok()) {
            return 1;
        }
        Result<net::ClientResponse> response = send(*client, net::MessageType::PlanMigration, *payload);
        if (!response.ok()) {
            std::cout << "plan failed: " << response.status().describe() << std::endl;
            return 1;
        }
        if (response->error) {
            return report_error(*response);
        }
        Result<net::PlanMigrationResponse> decoded = net::decode_plan_migration_response(response->payload);
        if (!decoded.ok()) {
            std::cout << "cannot decode plan response: " << decoded.status().describe() << std::endl;
            return 1;
        }
        std::cout << decoded->rendered << std::endl;
    } else {
        return usage();
    }

    net::GoodbyeRequest goodbye;
    goodbye.reason = "cli inspection complete";
    Result<ByteBuffer> payload = net::encode_message(goodbye);
    if (payload.ok()) {
        static_cast<void>(send(*client, net::MessageType::Goodbye, *payload));
    }
    client->close();
    return exit_code;
}

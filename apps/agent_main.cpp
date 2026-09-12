// haf_agent - represents an accelerator-bearing node and advertises observed
// devices through adapters.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "app_support.hpp"
#include "haf/adapters/adapter.hpp"
#include "haf/adapters/registry.hpp"
#include "haf/adapters/rocm_synthetic.hpp"
#include "haf/adapters/synthetic.hpp"
#include "haf/core/idgen.hpp"
#include "haf/federation/authority.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/net/client.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(HAF_HAVE_CUDA_ADAPTER)
#include "haf/adapters/cuda.hpp"
#endif

namespace {

std::atomic<bool> g_stop{false};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
extern "C" void console_handler(int) { g_stop.store(true); }
#endif

void install_signal_handler() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, console_handler);
    std::signal(SIGTERM, console_handler);
#endif
}

using haf::adapters::AcceleratorAdapter;
using haf::adapters::AdapterAvailability;

struct AdapterSet {
    std::vector<std::unique_ptr<AcceleratorAdapter>> adapters;
    std::vector<AdapterAvailability> availability;
};

/// Build the adapter set requested by --profile. Every path states plainly
/// whether it is REAL, SYNTHETIC, or UNSUPPORTED.
AdapterSet build_adapters(const std::string& profile) {
    AdapterSet set;
    const bool want_real = profile == "real" || profile == "all";
    const bool want_synthetic = profile != "real";

#if defined(HAF_HAVE_CUDA_ADAPTER)
    if (want_real) {
        std::unique_ptr<AcceleratorAdapter> cuda = haf::adapters::make_cuda_adapter();
        const haf::adapters::CudaInventory inventory = haf::adapters::cuda_inventory();
        AdapterAvailability availability;
        availability.name = "cuda-runtime";
        availability.support_level = haf::SupportLevel::Native;
        availability.provenance = haf::EvidenceProvenance::Real;
        availability.available = cuda != nullptr;
        availability.detail = inventory.available
                                  ? ("CUDA runtime " + inventory.runtime_version + ", driver " +
                                     inventory.driver_version + ", devices=" + std::to_string(inventory.device_count))
                                  : inventory.detail;
        set.availability.push_back(availability);
        if (cuda != nullptr) {
            set.adapters.push_back(std::move(cuda));
        }
    }
#else
    if (want_real) {
        AdapterAvailability availability;
        availability.name = "cuda-runtime";
        availability.support_level = haf::SupportLevel::Unsupported;
        availability.provenance = haf::EvidenceProvenance::Unsupported;
        availability.available = false;
        availability.detail = "this build does not include the CUDA adapter (no CUDA toolkit at configure time)";
        set.availability.push_back(availability);
    }
#endif

    if (want_synthetic) {
        const bool cuda_class = profile == "all" || profile == "synthetic-cuda";
        if (cuda_class) {
#if defined(HAF_HAVE_CUDA_ADAPTER)
            // Real CUDA is present: model a CUDA-class device only when asked.
            const bool real_present = !set.adapters.empty();
            if (!real_present || profile == "synthetic-cuda") {
                set.adapters.push_back(haf::adapters::make_cuda_class_synthetic_adapter());
            }
#else
            set.adapters.push_back(haf::adapters::make_cuda_class_synthetic_adapter());
            AdapterAvailability availability;
            availability.name = "synthetic-cuda-like";
            availability.support_level = haf::SupportLevel::Synthetic;
            availability.provenance = haf::EvidenceProvenance::Synthetic;
            availability.available = true;
            availability.detail = "deterministic CUDA-class model; no real CUDA device was observed";
            set.availability.push_back(availability);
#endif
        }
        if (profile == "all" || profile == "synthetic-rocm") {
            set.adapters.push_back(haf::adapters::make_rocm_synthetic_adapter());
            AdapterAvailability availability;
            availability.name = "synthetic-rocm-like";
            availability.support_level = haf::SupportLevel::Synthetic;
            availability.provenance = haf::EvidenceProvenance::Synthetic;
            availability.available = true;
            availability.detail = haf::adapters::rocm_support_statement();
            set.availability.push_back(availability);
        }
        if (profile == "all" || profile == "synthetic-intel") {
            set.adapters.push_back(haf::adapters::make_intel_synthetic_adapter());
            AdapterAvailability availability;
            availability.name = "synthetic-level-zero-like";
            availability.support_level = haf::SupportLevel::Synthetic;
            availability.provenance = haf::EvidenceProvenance::Synthetic;
            availability.available = true;
            availability.detail = "deterministic Intel/Level-Zero-class model; no Intel accelerator present";
            set.availability.push_back(availability);
        }
        if (profile == "partial") {
            set.adapters.push_back(
                haf::adapters::make_synthetic_adapter("synthetic-partial-evidence", haf::adapters::partially_evidenced_profile()));
            AdapterAvailability availability;
            availability.name = "synthetic-partial-evidence";
            availability.support_level = haf::SupportLevel::Synthetic;
            availability.provenance = haf::EvidenceProvenance::Synthetic;
            availability.available = true;
            availability.detail = "deterministic device with deliberately incomplete evidence";
            set.availability.push_back(availability);
        }
    }
    return set;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace haf;
    using namespace haf::app;

    Arguments arguments(argc, argv);
    if (arguments.has("--help")) {
        std::cout << "haf_agent --connect HOST:PORT [--node TOKEN] [--agent HEX] [--boot HEX] [--seed N]\n"
                     "          [--profile real|all|synthetic-cuda|synthetic-rocm|synthetic-intel|partial]\n"
                     "          [--heartbeat-millis N] [--run-seconds N] [--stop-file PATH]\n";
        return 0;
    }
    install_signal_handler();

    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    const std::string endpoint = arguments.string_value("--connect", "127.0.0.1:0");
    if (!parse_endpoint(endpoint, host, port)) {
        std::cerr << "invalid --connect endpoint: " << endpoint << std::endl;
        return 2;
    }
    const std::string profile = arguments.string_value("--profile", "all");
    const std::string node_token = arguments.string_value("--node", "node-local");
    const std::uint64_t seed = arguments.uint_value("--seed", 1);
    const std::int64_t heartbeat_millis = arguments.int_value("--heartbeat-millis", 250);
    const std::int64_t run_seconds = arguments.int_value("--run-seconds", 0);
    const std::filesystem::path stop_file = arguments.string_value("--stop-file", "");

    // Identities are generated from an explicit seed so that a test can
    // reproduce an agent exactly, and a new boot is a new identity.
    const IdGenerator generator = make_deterministic_id_generator(seed);
    AgentId agent;
    AgentBootId boot;
    {
        IdGenerator local = generator;
        agent = local.next_id<AgentIdTag>();
        boot = local.next_id<AgentBootIdTag>();
        const std::string agent_text = arguments.string_value("--agent", "");
        const std::string boot_text = arguments.string_value("--boot", "");
        if (!agent_text.empty()) {
            const Result<AgentId> parsed = parse_id<AgentIdTag>(agent_text);
            if (!parsed.ok()) {
                std::cerr << "invalid --agent: " << parsed.status().describe() << std::endl;
                return 2;
            }
            agent = *parsed;
        }
        if (!boot_text.empty()) {
            const Result<AgentBootId> parsed = parse_id<AgentBootIdTag>(boot_text);
            if (!parsed.ok()) {
                std::cerr << "invalid --boot: " << parsed.status().describe() << std::endl;
                return 2;
            }
            boot = *parsed;
        }
    }

    AdapterSet adapter_set = build_adapters(profile);
    for (const AdapterAvailability& availability : adapter_set.availability) {
        emit("AGENT_ADAPTER name=" + availability.name +
             " support=" + std::string(to_string(availability.support_level)) +
             " evidence=" + std::string(to_string(availability.provenance)) +
             " available=" + std::string(availability.available ? "yes" : "no"));
    }
    if (adapter_set.adapters.empty()) {
        emit("AGENT_UNSUPPORTED no adapter is available for profile '" + profile + "'");
        return 4;
    }

    net::ClientConfig client_config;
    client_config.host = host;
    client_config.port = port;
    client_config.name = "haf_agent";
    client_config.version = "1.0.0";
    Result<net::ControlClient> client = net::ControlClient::connect(client_config);
    if (!client.ok()) {
        std::cerr << "cannot connect to coordinator: " << client.status().describe() << std::endl;
        return 3;
    }

    adapters::AdapterContext context;
    context.agent = agent;
    context.agent_boot = boot;
    context.node_token = node_token;
    context.node = node_id_from_token(node_token);
    context.seed = seed;

    std::vector<AcceleratorDescriptor> devices;
    for (const std::unique_ptr<AcceleratorAdapter>& adapter : adapter_set.adapters) {
        Result<adapters::AdapterObservation> observation = adapter->observe(context);
        if (!observation.ok()) {
            emit("AGENT_ADAPTER_ERROR name=" + adapter->name() + " error=" + observation.status().describe());
            continue;
        }
        for (AcceleratorDescriptor& descriptor : observation->devices) {
            devices.push_back(std::move(descriptor));
        }
        emit("AGENT_OBSERVED adapter=" + adapter->name() +
             " devices=" + std::to_string(observation->devices.size()) +
             " evidence=" + std::string(to_string(observation->provenance)));
    }
    if (devices.empty()) {
        emit("AGENT_UNSUPPORTED no accelerator was observed by any available adapter");
        return 4;
    }

    // --- Handshake --------------------------------------------------------
    net::HelloRequest hello;
    hello.protocol_version = net::kProtocolVersion;
    hello.role = net::ClientRole::Agent;
    hello.agent = agent;
    hello.agent_boot = boot;
    hello.node = context.node;
    hello.node_token = node_token;
    hello.client_name = "haf_agent";
    hello.client_version = "1.0.0";
    hello.nonce = seed;
    {
        Result<ByteBuffer> payload = net::encode_message(hello);
        if (!payload.ok()) {
            std::cerr << "cannot encode hello: " << payload.status().describe() << std::endl;
            return 1;
        }
        Result<net::ClientResponse> response = client->request(net::MessageType::Hello, *payload);
        if (!response.ok()) {
            std::cerr << "handshake failed: " << response.status().describe() << std::endl;
            return 3;
        }
        if (response->type != net::MessageType::HelloAck) {
            Result<net::ErrorResponse> error = net::ControlClient::decode_error(*response);
            emit(std::string("AGENT_REJECTED code=") + (error.ok() ? std::string(to_string(error->code)) : "unknown") +
                 " message=" + (error.ok() ? error->message : std::string()));
            return 5;
        }
        Result<net::HelloResponse> ack = net::decode_hello_response(response->payload);
        if (!ack.ok()) {
            emit("AGENT_REJECTED code=MalformedData message=" + ack.status().describe());
            return 5;
        }
        if (!ack->accepted) {
            emit("AGENT_REJECTED code=HandshakeFailed message=" + ack->detail);
            return 5;
        }
        emit("AGENT_HELLO agent=" + agent.to_string() + " boot=" + boot.to_string() +
             " epoch=" + std::to_string(ack->epoch.value()) +
             " generation=" + std::to_string(ack->generation.value()) +
             " policy_generation=" + std::to_string(ack->policy_generation.value()) +
             " federation=" + (hello.federation.is_nil() ? std::string("any") : hello.federation.to_string()));
    }

    // --- Capability advertisement -----------------------------------------
    AuthorityClaim claim;
    claim.check_epoch = false;
    claim.check_federation_generation = false;
    claim.check_agent = true;
    claim.agent = agent;
    claim.check_agent_boot = true;
    claim.agent_boot = boot;
    claim.purpose = "capability advertisement";

    net::AdvertiseRequest advertise;
    advertise.claim = claim;
    advertise.devices = devices;
    {
        Result<ByteBuffer> payload = net::encode_message(advertise);
        if (!payload.ok()) {
            std::cerr << "cannot encode advertisement: " << payload.status().describe() << std::endl;
            return 1;
        }
        Result<net::ClientResponse> response = client->request(net::MessageType::Advertise, *payload);
        if (!response.ok()) {
            std::cerr << "advertisement failed: " << response.status().describe() << std::endl;
            return 3;
        }
        if (response->error) {
            Result<net::ErrorResponse> error = net::ControlClient::decode_error(*response);
            emit(std::string("AGENT_ADVERTISEMENT_REJECTED code=") +
                 (error.ok() ? std::string(to_string(error->code)) : "unknown") +
                 " message=" + (error.ok() ? error->message : std::string()));
            return 6;
        }
        Result<net::AdvertiseResponse> ack = net::decode_advertise_response(response->payload);
        if (!ack.ok()) {
            std::cerr << "cannot decode advertisement response: " << ack.status().describe() << std::endl;
            return 1;
        }
        std::string ids;
        for (const MemberRecord& member : ack->members) {
            if (!ids.empty()) {
                ids += ",";
            }
            ids += member.accelerator.to_string() + ":" + std::string(to_string(member.state));
        }
        emit("AGENT_ADVERTISED count=" + std::to_string(ack->members.size()) + " members=" + ids);
        emit("AGENT_READY agent=" + agent.to_string() + " boot=" + boot.to_string());
    }

    // --- Heartbeat loop ---------------------------------------------------
    const auto started_at = std::chrono::steady_clock::now();
    MessageSequence sequence(1);
    bool authority_lost = false;
    while (!g_stop.load()) {
        if (stop_requested(stop_file)) {
            break;
        }
        if (run_seconds > 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at)
                    .count();
            if (elapsed >= run_seconds) {
                break;
            }
        }
        net::HeartbeatRequest heartbeat;
        heartbeat.claim = claim;
        heartbeat.sequence = sequence;
        sequence = sequence.next();
        Result<ByteBuffer> payload = net::encode_message(heartbeat);
        if (!payload.ok()) {
            break;
        }
        Result<net::ClientResponse> response = client->request(net::MessageType::Heartbeat, *payload);
        if (!response.ok()) {
            emit("AGENT_DISCONNECTED detail=" + response.status().describe());
            authority_lost = true;
            break;
        }
        if (response->error) {
            Result<net::ErrorResponse> error = net::ControlClient::decode_error(*response);
            emit(std::string("AGENT_AUTHORITY_REJECTED code=") +
                 (error.ok() ? std::string(to_string(error->code)) : "unknown") +
                 " message=" + (error.ok() ? error->message : std::string()));
            authority_lost = true;
            break;
        }
        sleep_millis(static_cast<std::uint64_t>(heartbeat_millis < 1 ? 1 : heartbeat_millis));
    }

    // --- Graceful shutdown -------------------------------------------------
    net::GoodbyeRequest goodbye;
    goodbye.reason = authority_lost ? "authority rejected" : "agent stopping";
    Result<ByteBuffer> payload = net::encode_message(goodbye);
    if (payload.ok() && client->connected()) {
        static_cast<void>(client->request(net::MessageType::Goodbye, *payload));
    }
    client->close();
    emit(std::string("AGENT_STOPPED authority_lost=") + (authority_lost ? "yes" : "no"));
    return authority_lost ? 7 : 0;
}

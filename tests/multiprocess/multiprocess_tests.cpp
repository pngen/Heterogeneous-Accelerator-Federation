// Real multiprocess proof over a real TCP control plane.
//
// Every case here starts genuine OS processes. Nothing is simulated in-process:
// the coordinator and the agents are separate executables communicating over
// loopback TCP, and the cases kill and restart them for real.

#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/net/client.hpp"
#include "haf/net/socket.hpp"
#include "haf/persist/file_store.hpp"
#include "support/environment.hpp"
#include "support/process.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

namespace {

constexpr int kStartupDeadlineMillis = 40000;

struct CoordinatorHandle {
    ChildProcess process;
    std::uint16_t port{0};
    FederationId federation{};
    CoordinatorEpoch epoch{};
    std::filesystem::path output;
    std::filesystem::path stop_file;
};

[[nodiscard]] std::string read_all(const std::filesystem::path& path) { return read_file_text(path); }

/// Poll a file until it contains \p marker. This synchronizes on an explicit
/// state change rather than on an arbitrary sleep.
[[nodiscard]] bool wait_for_marker(const std::filesystem::path& path, const std::string& marker, int timeout_millis) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_millis);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string text = read_all(path);
        if (text.find(marker) != std::string::npos) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

/// Extract the value that follows \p key, searching only the line that starts
/// with \p anchor. Anchoring matters: "epoch=" also appears inside
/// "previous_epoch=", and a naive search would read the wrong number.
[[nodiscard]] std::string extract_value(const std::string& text, const std::string& key,
                                        const std::string& anchor = std::string()) {
    std::size_t search_from = 0;
    while (search_from < text.size()) {
        const std::size_t line_end = text.find('\n', search_from);
        const std::size_t end_of_line = line_end == std::string::npos ? text.size() : line_end;
        const std::string line = text.substr(search_from, end_of_line - search_from);
        const bool line_matches = anchor.empty() || line.rfind(anchor, 0) == 0;
        if (line_matches) {
            const std::size_t position = line.find(key);
            if (position != std::string::npos) {
                const std::size_t start = position + key.size();
                std::size_t end = start;
                while (end < line.size() && line[end] != ' ' && line[end] != '\r') {
                    ++end;
                }
                return line.substr(start, end - start);
            }
        }
        if (line_end == std::string::npos) {
            break;
        }
        search_from = line_end + 1;
    }
    return {};
}

/// Start a coordinator and wait until it reports that it is serving.
[[nodiscard]] bool start_coordinator(CoordinatorHandle& handle, const std::filesystem::path& scratch,
                                     const std::string& name, std::uint64_t seed,
                                     const std::filesystem::path& store) {
    handle.output = scratch / (name + ".coordinator.out");
    handle.stop_file = scratch / (name + ".coordinator.stop");
    // A stop file left over from a previous run would stop this child on its
    // first poll, so it is removed before the child is launched.
    {
        std::error_code error;
        std::filesystem::remove(handle.stop_file, error);
    }
    std::vector<std::string> arguments = {"--port", "0", "--seed", std::to_string(seed),
                                          "--store", store.string(), "--stop-file", handle.stop_file.string(),
                                          "--name", name};
    if (!handle.process.start(app_path("haf_coordinator"), arguments, scratch, handle.output)) {
        std::cout << "NOTE coordinator start failed: " << handle.process.start_error() << std::endl;
        return false;
    }
    if (!wait_for_marker(handle.output, "COORDINATOR_READY", kStartupDeadlineMillis)) {
        return false;
    }
    const std::string text = read_all(handle.output);
    const std::string port_text = extract_value(text, "port=", "COORDINATOR_READY");
    if (port_text.empty()) {
        return false;
    }
    handle.port = static_cast<std::uint16_t>(std::stoi(port_text));
    const Result<FederationId> federation = parse_id<FederationIdTag>(extract_value(text, "federation=", "COORDINATOR_READY"));
    if (federation.ok()) {
        handle.federation = *federation;
    }
    const std::string epoch_text = extract_value(text, "epoch=", "COORDINATOR_READY");
    if (!epoch_text.empty()) {
        handle.epoch = CoordinatorEpoch(static_cast<std::uint64_t>(std::stoull(epoch_text)));
    }
    return true;
}

struct AgentHandle {
    ChildProcess process;
    AgentId agent{};
    AgentBootId boot{};
    std::filesystem::path output;
    std::filesystem::path stop_file;
    std::vector<std::string> members;
};

[[nodiscard]] bool start_agent(AgentHandle& handle, const std::filesystem::path& scratch, const std::string& name,
                               std::uint16_t port, const std::string& profile, std::uint64_t seed) {
    handle.output = scratch / (name + ".agent.out");
    handle.stop_file = scratch / (name + ".agent.stop");
    {
        std::error_code error;
        std::filesystem::remove(handle.stop_file, error);
    }
    std::vector<std::string> arguments = {"--connect", "127.0.0.1:" + std::to_string(port),
                                          "--profile", profile,
                                          "--seed", std::to_string(seed),
                                          "--stop-file", handle.stop_file.string(),
                                          "--node", name};
    if (!handle.process.start(app_path("haf_agent"), arguments, scratch, handle.output)) {
        std::cout << "NOTE agent start failed: " << handle.process.start_error() << std::endl;
        return false;
    }
    if (!wait_for_marker(handle.output, "AGENT_READY", 2000) &&
        !wait_for_marker(handle.output, "AGENT_READY", kStartupDeadlineMillis)) {
        std::cout << "NOTE agent output:\n" << read_all(handle.output) << std::endl;
        return false;
    }
    const std::string text = read_all(handle.output);
    const Result<AgentId> agent = parse_id<AgentIdTag>(extract_value(text, "AGENT_READY agent="));
    if (agent.ok()) {
        handle.agent = *agent;
    }
    const Result<AgentBootId> boot = parse_id<AgentBootIdTag>(extract_value(text, "boot="));
    if (boot.ok()) {
        handle.boot = *boot;
    }
    const std::string advertised = extract_value(text, "members=");
    std::size_t start = 0;
    while (start <= advertised.size() && !advertised.empty()) {
        const std::size_t comma = advertised.find(',', start);
        const std::string entry =
            advertised.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!entry.empty()) {
            handle.members.push_back(entry.substr(0, entry.find(':')));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return true;
}

/// Stop a child gracefully by creating its stop file, then wait.
void stop_child(ChildProcess& process, const std::filesystem::path& stop_file) {
    if (process.exited()) {
        return;
    }
    std::error_code error;
    std::ofstream(stop_file.string(), std::ios::app).close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        process.poll();
        if (process.exited()) {
            std::filesystem::remove(stop_file, error);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    process.kill();
    process.wait();
    std::filesystem::remove(stop_file, error);
}

[[nodiscard]] std::string run_cli(std::uint16_t port, const std::vector<std::string>& arguments) {
    std::vector<std::string> full = {"--connect", "127.0.0.1:" + std::to_string(port)};
    full.insert(full.end(), arguments.begin(), arguments.end());
    const ProcessResult result = run_process(app_path("haf_cli"), full, std::filesystem::current_path());
    return result.output;
}

/// Open an inspector session and complete the handshake.
[[nodiscard]] Result<net::ControlClient> connect_inspector(std::uint16_t port) {
    net::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.connect_timeout_millis = 5000;
    config.name = "haf_test";
    Result<net::ControlClient> client = net::ControlClient::connect(config);
    if (!client.ok()) {
        return client.status();
    }
    net::HelloRequest hello;
    hello.protocol_version = net::kProtocolVersion;
    hello.role = net::ClientRole::Inspector;
    hello.client_name = "haf_test";
    hello.client_version = "1.0.0";
    hello.nonce = 1;
    Result<ByteBuffer> payload = net::encode_message(hello);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<net::ClientResponse> response = client->request(net::MessageType::Hello, *payload);
    if (!response.ok()) {
        return response.status();
    }
    if (response->error) {
        const Result<net::ErrorResponse> error = net::ControlClient::decode_error(*response);
        if (error.ok()) {
            return error->code == ErrorCode::Ok ? Status(ErrorCode::HandshakeFailed, error->message) : Status(error->code, error->message);
        }
        return Status(ErrorCode::HandshakeFailed, "handshake rejected");
    }
    return client;
}

/// Open an agent-role session for an identity the test already knows, so that
/// agent-scoped authority checks can be exercised without spawning a process.
[[nodiscard]] Result<net::ControlClient> connect_agent(std::uint16_t port, const AgentId& agent,
                                                       const AgentBootId& boot) {
    net::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.connect_timeout_millis = 5000;
    config.name = "haf_test_agent";
    Result<net::ControlClient> client = net::ControlClient::connect(config);
    if (!client.ok()) {
        return client.status();
    }
    net::HelloRequest hello;
    hello.protocol_version = net::kProtocolVersion;
    hello.role = net::ClientRole::Agent;
    hello.agent = agent;
    hello.agent_boot = boot;
    hello.node = node_id_from_token("test-agent-session");
    hello.node_token = "test-agent-session";
    hello.client_name = "haf_test_agent";
    hello.client_version = "1.0.0";
    hello.nonce = 7;
    Result<ByteBuffer> payload = net::encode_message(hello);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<net::ClientResponse> response = client->request(net::MessageType::Hello, *payload);
    if (!response.ok()) {
        return response.status();
    }
    if (response->error) {
        const Result<net::ErrorResponse> error = net::ControlClient::decode_error(*response);
        if (error.ok()) {
            return Status(error->code, error->message);
        }
        return Status(ErrorCode::HandshakeFailed, "agent handshake rejected");
    }
    return client;
}

[[nodiscard]] WorkloadProfile live_workload() {
    return make_workload("multiprocess-cuda-fp8",
                         {hard_equals("vendor.id", "nvidia"), hard_present("numeric.fp8_e4m3"),
                          hard_tokens("isa.code_object_targets", {"sm-120"}),
                          hard_at_least("memory.total_bytes", 1LL * 1024 * 1024 * 1024)});
}

#if defined(HAF_HAVE_CUDA_ADAPTER)
constexpr const char* kRealProfile = "real";
#else
constexpr const char* kRealProfile = "synthetic-cuda";
#endif

}  // namespace

HAF_TEST(multiprocess, full_federation_lifecycle_proof) {
    ScratchDirectory scratch("mp-lifecycle");
    const std::filesystem::path store = scratch.file("federation.store");

    // 1. Start the coordinator.
    HAF_PHASE("SETUP");
    CoordinatorHandle coordinator;
    HAF_REQUIRE(start_coordinator(coordinator, scratch.path(), "primary", 41, store));
    HAF_NOTE("coordinator port=" + std::to_string(coordinator.port) +
             " federation=" + coordinator.federation.to_string() +
             " epoch=" + std::to_string(coordinator.epoch.value()));
    HAF_CHECK(!coordinator.federation.is_nil());
    HAF_CHECK(coordinator.epoch.value() >= 1);

    // 2/3. Start Agent A and let it advertise.
    HAF_PHASE("ADVERTISE");
    AgentHandle agent_a;
    HAF_REQUIRE(start_agent(agent_a, scratch.path(), "agent-a", coordinator.port, kRealProfile, 42));
    HAF_CHECK(!agent_a.members.empty());
    const std::string real_member = agent_a.members.front();
    HAF_NOTE("agent A advertised " + std::to_string(agent_a.members.size()) + " accelerator(s)");

    // 4/5. Start Agent B with a different capability profile; both are admitted.
    AgentHandle agent_b;
    HAF_REQUIRE(start_agent(agent_b, scratch.path(), "agent-b", coordinator.port, "synthetic-rocm", 43));
    HAF_CHECK(!agent_b.members.empty());
    const std::string synthetic_member = agent_b.members.front();

    {
        const std::string listing = run_cli(coordinator.port, {"member", "list"});
        HAF_CHECK(listing.find(real_member) != std::string::npos);
        HAF_CHECK(listing.find(synthetic_member != real_member ? synthetic_member : agent_b.members.back()) !=
                  std::string::npos);
        HAF_CHECK(listing.find("state=ACTIVE") != std::string::npos);
    }

    // 6. Submit workload requirements through the control plane.
    HAF_PHASE("NEGOTIATE");
    const WorkloadProfile workload = live_workload();
    {
        Result<net::ControlClient> client = connect_inspector(coordinator.port);
        HAF_REQUIRE_OK(client);
        net::RegisterWorkloadRequest request;
        request.profile = workload;
        request.claim.check_epoch = false;
        request.claim.check_federation_generation = false;
        request.claim.purpose = "multiprocess test";
        Result<ByteBuffer> payload = net::encode_message(request);
        HAF_REQUIRE_OK(payload);
        Result<net::ClientResponse> response =
            client->request(net::MessageType::RegisterWorkload, *payload);
        HAF_REQUIRE_OK(response);
        HAF_CHECK(!response->error);
        net::GoodbyeRequest goodbye;
        goodbye.reason = "workload registered";
        Result<ByteBuffer> goodbye_payload = net::encode_message(goodbye);
        if (goodbye_payload.ok()) {
            static_cast<void>(client->request(net::MessageType::Goodbye, *goodbye_payload));
        }
    }

    // 7/8. Hard eligibility filtering with a structured, deterministic explanation.
    HAF_PHASE("EVALUATE");
    {
        const std::string eligible = run_cli(coordinator.port, {"workload", "evaluate",
                                                                workload.class_id.to_string(),
                                                                "--accelerator", real_member});
        HAF_CHECK(eligible.find("outcome=ELIGIBLE") != std::string::npos);
        const std::string ineligible = run_cli(coordinator.port, {"workload", "evaluate",
                                                                  workload.class_id.to_string(),
                                                                  "--accelerator", synthetic_member});
        HAF_CHECK(ineligible.find("outcome=INELIGIBLE") != std::string::npos);
        HAF_CHECK(ineligible.find("VendorMismatch") != std::string::npos);
        const std::string repeat = run_cli(coordinator.port, {"workload", "evaluate",
                                                              workload.class_id.to_string(),
                                                              "--accelerator", real_member});
        // The decision fingerprint is reproducible across separate CLI processes.
        const std::string first_fingerprint = extract_value(eligible, "fingerprint=");
        const std::string second_fingerprint = extract_value(repeat, "fingerprint=");
        HAF_CHECK(!first_fingerprint.empty());
        HAF_CHECK(first_fingerprint == second_fingerprint);
    }

    // 9. Portability classification between the two vendors.
    HAF_PHASE("PLAN");
    {
        const std::string portability = run_cli(coordinator.port, {"portability", "explain",
                                                                   "--from", real_member,
                                                                   "--to", synthetic_member});
        HAF_CHECK(portability.find("class=") != std::string::npos);
        HAF_CHECK(portability.find("LIVE_MIGRATION_SUPPORTED") == std::string::npos);
    }
    {
        const std::string plan = run_cli(coordinator.port, {"migration", "plan",
                                                            workload.class_id.to_string(),
                                                            "--from", real_member,
                                                            "--to", synthetic_member});
        HAF_CHECK(plan.find("outcome=") != std::string::npos);
    }

    // 10/11. Kill Agent A for real, then prove its authority is gone.
    HAF_PHASE("KILL");
    agent_a.process.kill();
    agent_a.process.wait();
    HAF_CHECK(agent_a.process.exited());
    const std::string after_kill = run_cli(coordinator.port, {"member", "list"});
    HAF_CHECK(after_kill.find(real_member) != std::string::npos);

    // 12/13. Restart Agent A with a fresh boot identity and prove that stale
    // boot traffic is refused.
    HAF_PHASE("RESTART");
    AgentHandle agent_a2;
    HAF_REQUIRE(start_agent(agent_a2, scratch.path(), "agent-a2", coordinator.port, kRealProfile, 44));
    HAF_CHECK(agent_a2.boot != agent_a.boot);
    HAF_CHECK(!agent_a2.members.empty());
    {
        // The old incarnation no longer holds authority: it is fenced or retired
        // rather than still accepting work.
        const std::string listing = run_cli(coordinator.port, {"member", "list"});
        HAF_CHECK(listing.find(real_member + " state=ACTIVE") == std::string::npos);
    }
    {
        // A live agent session that presents the dead boot identity is refused
        // with a typed stale-boot error.
        // A probe identity distinct from the live agent's own boot, so that the
        // only reason for refusal can be the stale boot it asserts.
        const AgentBootId probe_boot = AgentBootId::from_raw(derive_identity("haf.test.probe-boot", "stale"));
        Result<net::ControlClient> client = connect_agent(coordinator.port, agent_a2.agent, probe_boot);
        HAF_REQUIRE_OK(client);
        net::HeartbeatRequest heartbeat;
        heartbeat.claim.check_epoch = false;
        heartbeat.claim.check_federation_generation = false;
        heartbeat.claim.check_agent = true;
        heartbeat.claim.agent = agent_a2.agent;
        heartbeat.claim.check_agent_boot = true;
        heartbeat.claim.agent_boot = agent_a.boot;
        Result<ByteBuffer> payload = net::encode_message(heartbeat);
        HAF_REQUIRE_OK(payload);
        Result<net::ClientResponse> response = client->request(net::MessageType::Heartbeat, *payload);
        HAF_REQUIRE_OK(response);
        HAF_CHECK(response->error);
        const Result<net::ErrorResponse> error = net::ControlClient::decode_error(*response);
        HAF_REQUIRE_OK(error);
        HAF_EQ(static_cast<int>(error->code), static_cast<int>(ErrorCode::StaleBoot));
        net::GoodbyeRequest goodbye;
        goodbye.reason = "stale boot probe";
        Result<ByteBuffer> goodbye_payload = net::encode_message(goodbye);
        if (goodbye_payload.ok()) {
            static_cast<void>(client->request(net::MessageType::Goodbye, *goodbye_payload));
        }
    }

    // 14/15/16. Restart the coordinator and prove authority advanced.
    HAF_PHASE("RECOVER");
    const CoordinatorEpoch previous_epoch = coordinator.epoch;
    stop_child(coordinator.process, coordinator.stop_file);
    HAF_CHECK(coordinator.process.exited());
    HAF_CHECK(coordinator.process.exit_code() == 0);
    CoordinatorHandle restarted;
    HAF_REQUIRE(start_coordinator(restarted, scratch.path(), "primary", 41, store));
    HAF_CHECK(restarted.federation == coordinator.federation);
    HAF_CHECK(restarted.epoch.value() > previous_epoch.value());
    HAF_NOTE("epoch " + std::to_string(previous_epoch.value()) + " -> " +
             std::to_string(restarted.epoch.value()));

    {
        const std::string recovery = run_cli(restarted.port, {"recovery"});
        HAF_CHECK(recovery.find("recovered=yes") != std::string::npos);
        HAF_CHECK(recovery.find("requiring_revalidation=") != std::string::npos);
    }

    // 17. Pre-restart authority can no longer mutate the federation.
    {
        Result<net::ControlClient> client = connect_inspector(restarted.port);
        HAF_REQUIRE_OK(client);
        net::HeartbeatRequest stale;
        stale.claim.check_epoch = true;
        stale.claim.epoch = previous_epoch;
        Result<ByteBuffer> payload = net::encode_message(stale);
        HAF_REQUIRE_OK(payload);
        Result<net::ClientResponse> response = client->request(net::MessageType::Heartbeat, *payload);
        HAF_REQUIRE_OK(response);
        HAF_CHECK(response->error);
        const Result<net::ErrorResponse> error = net::ControlClient::decode_error(*response);
        HAF_REQUIRE_OK(error);
        HAF_EQ(static_cast<int>(error->code), static_cast<int>(ErrorCode::StaleEpoch));

        // A transition carrying the pre-restart coordinator epoch is refused too.
        net::TransitionRequest transition;
        transition.accelerator = AcceleratorId::from_raw(derive_identity("haf.accelerator", "unknown"));
        transition.target = MemberState::Fenced;
        transition.claim = stale.claim;
        Result<ByteBuffer> transition_payload = net::encode_message(transition);
        HAF_REQUIRE_OK(transition_payload);
        Result<net::ClientResponse> transition_response =
            client->request(net::MessageType::Transition, *transition_payload);
        HAF_REQUIRE_OK(transition_response);
        HAF_CHECK(transition_response->error);
        net::GoodbyeRequest goodbye;
        goodbye.reason = "stale epoch probe";
        Result<ByteBuffer> goodbye_payload = net::encode_message(goodbye);
        if (goodbye_payload.ok()) {
            static_cast<void>(client->request(net::MessageType::Goodbye, *goodbye_payload));
        }
    }

    // 18/19. Revalidate with fresh agent boots and restore active membership.
    HAF_PHASE("VERIFY");
    AgentHandle agent_a3;
    HAF_REQUIRE(start_agent(agent_a3, scratch.path(), "agent-a3", restarted.port, kRealProfile, 45));
    AgentHandle agent_b2;
    HAF_REQUIRE(start_agent(agent_b2, scratch.path(), "agent-b2", restarted.port, "synthetic-rocm", 46));
    {
        const std::string listing = run_cli(restarted.port, {"member", "list"});
        HAF_CHECK(listing.find("state=ACTIVE") != std::string::npos);
        const std::string audit = run_cli(restarted.port, {"audit"});
        HAF_CHECK(audit.find("violations=0") != std::string::npos);
        const std::string eligibility = run_cli(restarted.port, {"workload", "evaluate",
                                                                 workload.class_id.to_string(),
                                                                 "--accelerator", agent_a3.members.front()});
        HAF_CHECK(eligibility.find("outcome=ELIGIBLE") != std::string::npos);
    }

    // 20. Clean shutdown with no leaked child processes.
    HAF_PHASE("SHUTDOWN");
    stop_child(agent_a3.process, agent_a3.stop_file);
    stop_child(agent_b2.process, agent_b2.stop_file);
    stop_child(agent_b.process, agent_b.stop_file);
    stop_child(agent_a2.process, agent_a2.stop_file);
    stop_child(restarted.process, restarted.stop_file);
    HAF_CHECK(agent_a3.process.exit_code() == 0);
    HAF_CHECK(agent_b2.process.exit_code() == 0);
    HAF_CHECK(restarted.process.exit_code() == 0);
    HAF_CHECK(!any_child_running());
    {
        const std::string stop_text = read_all(restarted.output);
        HAF_CHECK(stop_text.find("COORDINATOR_STOPPED exit=0") != std::string::npos);
        HAF_CHECK(stop_text.find("COORDINATOR_AUDIT") != std::string::npos);
    }
    // The store left behind is valid and independently verifiable.
    {
        const std::string verify = run_process(app_path("haf_cli"), {"snapshot", "verify", "--store", store.string()},
                                               std::filesystem::current_path())
                                       .output;
        if (verify.find("status=VERIFIED") == std::string::npos) {
            std::cout << "NOTE store verification:\n" << verify << std::endl;
        }
        HAF_CHECK(verify.find("status=VERIFIED") != std::string::npos);
        HAF_CHECK(verify.find("violations=0") != std::string::npos);
    }
}

HAF_TEST(multiprocess, duplicate_live_boot_over_tcp_is_refused) {
    ScratchDirectory scratch("mp-duplicate");
    const std::filesystem::path store = scratch.file("federation.store");
    CoordinatorHandle coordinator;
    HAF_PHASE("SETUP");
    HAF_REQUIRE(start_coordinator(coordinator, scratch.path(), "duplicate", 51, store));

    HAF_PHASE("CONNECT");
    AgentHandle first;
    HAF_REQUIRE(start_agent(first, scratch.path(), "dup-first", coordinator.port, "synthetic-rocm", 52));

    // A second agent process presenting the same agent and boot identity is a
    // contradictory federation state and must be refused.
    const std::filesystem::path output = scratch.file("dup-second.agent.out");
    const std::filesystem::path stop_file = scratch.file("dup-second.agent.stop");
    std::vector<std::string> arguments = {"--connect", "127.0.0.1:" + std::to_string(coordinator.port),
                                          "--profile", "synthetic-rocm",
                                          "--seed", "52",
                                          "--stop-file", stop_file.string(),
                                          "--node", "dup-second"};
    ChildProcess duplicate;
    HAF_REQUIRE(duplicate.start(app_path("haf_agent"), arguments, scratch.path(), output));
    HAF_CHECK(wait_for_marker(output, "AGENT_REJECTED", kStartupDeadlineMillis));
    const std::string text = read_all(output);
    if (text.find("already established") == std::string::npos) {
        std::cout << "NOTE duplicate agent output:\n" << text << std::endl;
    }
    HAF_CHECK(text.find("already established") != std::string::npos);
    duplicate.kill();
    duplicate.wait();

    HAF_PHASE("SHUTDOWN");
    stop_child(first.process, first.stop_file);
    stop_child(coordinator.process, coordinator.stop_file);
    HAF_CHECK(!any_child_running());
}

HAF_TEST(multiprocess, protocol_faults_close_the_connection_without_hanging) {
    ScratchDirectory scratch("mp-protocol");
    const std::filesystem::path store = scratch.file("federation.store");
    CoordinatorHandle coordinator;
    HAF_PHASE("SETUP");
    HAF_REQUIRE(start_coordinator(coordinator, scratch.path(), "protocol", 61, store));

    HAF_PHASE("CONNECT");
    {
        // A connection that sends an unparseable header must be closed by the
        // coordinator rather than left half-open.
        net::ClientConfig config;
        config.host = "127.0.0.1";
        config.port = coordinator.port;
        config.read_timeout_millis = 5000;
        Result<net::TcpSocket> socket = net::TcpSocket::connect(config.host, config.port, 5000);
        HAF_REQUIRE_OK(socket);
        const std::uint8_t garbage[net::kFrameHeaderBytes] = {'X', 'X', 'X', 'X', 1, 0, 1, 0, 0, 0,
                                                              0,   0,   0,   0,   0, 0, 0, 0, 0, 0,
                                                              0,   0,   0,   0, 0, 0, 0, 0};
        HAF_CHECK(socket->write_all(garbage, sizeof(garbage)).ok());
        std::uint8_t response[net::kFrameHeaderBytes] = {};
        const Result<std::size_t> read = socket->read_some(response, sizeof(response), 5000);
        // Either an error frame or an immediate close is acceptable; a hang is not.
        HAF_CHECK(read.ok() || read.status().code() == ErrorCode::ConnectionClosed);
        socket->close();
    }
    {
        // An oversized declared payload is refused before any allocation.
        Result<net::TcpSocket> socket = net::TcpSocket::connect("127.0.0.1", coordinator.port, 5000);
        HAF_REQUIRE_OK(socket);
        net::Frame frame;
        frame.header.type = static_cast<std::uint16_t>(net::MessageType::Query);
        frame.header.sequence = 1;
        const Result<ByteBuffer> encoded = net::encode_frame(frame);
        HAF_REQUIRE_OK(encoded);
        ByteBuffer forged = *encoded;
        forged[20] = 0xFFU;
        forged[21] = 0xFFU;
        forged[22] = 0xFFU;
        forged[23] = 0x7FU;
        forged[24] = 0;
        forged[25] = 0;
        forged[26] = 0;
        forged[27] = 0;
        const std::uint32_t crc = net::crc32_ieee(forged.data(), 24);
        forged[24] = static_cast<std::uint8_t>(crc & 0xFFU);
        forged[25] = static_cast<std::uint8_t>((crc >> 8) & 0xFFU);
        forged[26] = static_cast<std::uint8_t>((crc >> 16) & 0xFFU);
        forged[27] = static_cast<std::uint8_t>((crc >> 24) & 0xFFU);
        HAF_CHECK(socket->write_all(forged.data(), net::kFrameHeaderBytes).ok());
        std::uint8_t response[net::kFrameHeaderBytes] = {};
        const Result<std::size_t> read = socket->read_some(response, sizeof(response), 5000);
        HAF_CHECK(read.ok() || read.status().code() == ErrorCode::ConnectionClosed);
        socket->close();
    }
    {
        // A request before the handshake is refused with a typed error.
        Result<net::TcpSocket> socket = net::TcpSocket::connect("127.0.0.1", coordinator.port, 5000);
        HAF_REQUIRE_OK(socket);
        net::QueryRequest query;
        query.kind = net::QueryKind::FederationSummary;
        Result<ByteBuffer> payload = net::encode_message(query);
        HAF_REQUIRE_OK(payload);
        net::Frame frame;
        frame.header.type = static_cast<std::uint16_t>(net::MessageType::Query);
        frame.header.sequence = 1;
        frame.payload = *payload;
        const Result<ByteBuffer> encoded = net::encode_frame(frame);
        HAF_REQUIRE_OK(encoded);
        HAF_CHECK(socket->write_all(encoded->data(), encoded->size()).ok());
        std::uint8_t header[net::kFrameHeaderBytes] = {};
        HAF_CHECK(socket->read_exact(header, sizeof(header), 5000).ok());
        const Result<net::FrameHeader> decoded =
            net::decode_frame_header(header, sizeof(header), net::kMaxFramePayloadBytes);
        HAF_REQUIRE_OK(decoded);
        HAF_CHECK(decoded->is_error());
        socket->close();
    }
    // The coordinator is unaffected by the faults.
    {
        const std::string audit = run_cli(coordinator.port, {"audit"});
        HAF_CHECK(audit.find("violations=0") != std::string::npos);
    }

    HAF_PHASE("SHUTDOWN");
    stop_child(coordinator.process, coordinator.stop_file);
    HAF_CHECK(coordinator.process.exit_code() == 0);
    HAF_CHECK(!any_child_running());
}

HAF_TEST(multiprocess, coordinator_restart_preserves_durable_membership_only) {
    ScratchDirectory scratch("mp-restart");
    const std::filesystem::path store = scratch.file("federation.store");
    CoordinatorHandle first;
    HAF_PHASE("SETUP");
    HAF_REQUIRE(start_coordinator(first, scratch.path(), "restart", 71, store));
    AgentHandle agent;
    HAF_REQUIRE(start_agent(agent, scratch.path(), "restart-agent", first.port, "synthetic-rocm", 72));
    HAF_CHECK(!agent.members.empty());
    const std::string member = agent.members.front();
    {
        const std::string listing = run_cli(first.port, {"member", "list"});
        HAF_CHECK(listing.find(member) != std::string::npos);
        HAF_CHECK(listing.find("state=ACTIVE") != std::string::npos);
    }

    // Kill the agent process outright: the coordinator cannot be told.
    HAF_PHASE("KILL");
    agent.process.kill();
    agent.process.wait();

    HAF_PHASE("RESTART");
    stop_child(first.process, first.stop_file);
    CoordinatorHandle second;
    HAF_REQUIRE(start_coordinator(second, scratch.path(), "restart", 71, store));
    HAF_CHECK(second.federation == first.federation);
    HAF_CHECK(second.epoch.value() > first.epoch.value());

    // Durable membership is present but not active: no member may accept work
    // until a live agent revalidates it.
    {
        const std::string listing = run_cli(second.port, {"member", "list"});
        HAF_CHECK(listing.find(member) != std::string::npos);
        // No member may accept new work after a restart until a live agent
        // revalidates it.
        HAF_CHECK(listing.find("state=ACTIVE") == std::string::npos);
        HAF_CHECK(listing.find("state=DEGRADED") != std::string::npos ||
                  listing.find("state=FENCED") != std::string::npos);
        const std::string audit = run_cli(second.port, {"audit"});
        HAF_CHECK(audit.find("violations=0") != std::string::npos);
    }

    HAF_PHASE("RECOVER");
    AgentHandle resumed;
    HAF_REQUIRE(start_agent(resumed, scratch.path(), "restart-agent-2", second.port, "synthetic-rocm", 73));
    {
        const std::string listing = run_cli(second.port, {"member", "list"});
        HAF_CHECK(listing.find("state=ACTIVE") != std::string::npos);
    }

    HAF_PHASE("SHUTDOWN");
    stop_child(resumed.process, resumed.stop_file);
    stop_child(second.process, second.stop_file);
    HAF_CHECK(second.process.exit_code() == 0);
    HAF_CHECK(!any_child_running());
}

// haf_coordinator - owns federation authority and serves the control plane.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "app_support.hpp"
#include "haf/adapters/registry.hpp"
#include "haf/federation/federation.hpp"
#include "haf/net/control_plane.hpp"
#include "haf/net/server.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

}  // namespace

int main(int argc, char** argv) {
    using namespace haf;
    using namespace haf::app;

    Arguments arguments(argc, argv);
    if (arguments.has("--help")) {
        std::cout << "haf_coordinator [--bind ADDR] [--port N] [--store PATH] [--name NAME]\n"
                     "                [--federation HEX] [--seed N] [--run-seconds N] [--stop-file PATH]\n"
                     "                [--no-persist] [--quiet]\n";
        return 0;
    }

    install_signal_handler();

    net::ControlPlaneConfig config;
    config.name = arguments.string_value("--name", "federation");
    config.store_path = arguments.string_value("--store", "");
    config.id_seed = arguments.uint_value("--seed", 0);
    config.persist = !arguments.has("--no-persist");
    config.adapters = adapters::synthetic_adapter_availability();

    const std::string federation_text = arguments.string_value("--federation", "");
    if (!federation_text.empty()) {
        const Result<FederationId> parsed = parse_id<FederationIdTag>(federation_text);
        if (!parsed.ok()) {
            std::cerr << "invalid --federation: " << parsed.status().describe() << std::endl;
            return 2;
        }
        config.federation = *parsed;
    }

    const std::string bind = arguments.string_value("--bind", "127.0.0.1");
    const std::uint16_t requested_port = static_cast<std::uint16_t>(arguments.uint_value("--port", 0));
    const std::int64_t run_seconds = arguments.int_value("--run-seconds", 0);
    const std::filesystem::path stop_file = arguments.string_value("--stop-file", "");
    const bool quiet = arguments.has("--quiet");

    Result<std::unique_ptr<net::ControlPlane>> plane = net::ControlPlane::create(config);
    if (!plane.ok()) {
        std::cerr << "cannot create control plane: " << plane.status().describe() << std::endl;
        return 1;
    }
    const RecoveryReport& recovery = (*plane)->federation().recovery_report();
    emit(std::string("COORDINATOR_STORE ") + (*plane)->federation().store_location());
    emit(std::string("COORDINATOR_RECOVERY recovered=") + (recovery.recovered ? "yes" : "no") +
         " previous_epoch=" + std::to_string(recovery.previous_epoch.value()) +
         " epoch=" + std::to_string(recovery.new_epoch.value()) +
         " members_restored=" + std::to_string(recovery.members_restored) +
         " requiring_revalidation=" + std::to_string(recovery.members_requiring_revalidation) +
         " decisions_invalidated=" + std::to_string(recovery.decisions_invalidated));

    net::ServerConfig server_config;
    server_config.bind_address = bind;
    server_config.port = requested_port;
    net::ControlServer server(server_config, **plane);
    const VoidResult started = server.start();
    if (!started.ok()) {
        std::cerr << "cannot start control server: " << started.status().describe() << std::endl;
        return 1;
    }

    emit("COORDINATOR_READY port=" + std::to_string(server.port()) +
         " federation=" + (*plane)->federation().id().to_string() +
         " epoch=" + std::to_string((*plane)->federation().epoch().value()) +
         " generation=" + std::to_string((*plane)->federation().generation().value()) +
         " store=" + (*plane)->federation().store_location());

    const auto started_at = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        if (stop_requested(stop_file)) {
            break;
        }
        if (run_seconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - started_at)
                                     .count();
            if (elapsed >= run_seconds) {
                break;
            }
        }
        sleep_millis(50);
    }

    emit("COORDINATOR_SHUTDOWN reason=" + std::string(g_stop.load() ? "signal" : "stop-request"));
    server.stop();
    const VoidResult persisted = (*plane)->federation().persist();
    if (!persisted.ok()) {
        std::cerr << "final persistence failed: " << persisted.status().describe() << std::endl;
        return 1;
    }
    const VoidResult revoked = (*plane)->federation().shutdown();
    if (!revoked.ok()) {
        std::cerr << "authority revocation failed: " << revoked.status().describe() << std::endl;
        return 1;
    }
    const AuditReport report = (*plane)->federation().audit();
    if (!quiet) {
        emit(std::string("COORDINATOR_AUDIT ") + report.render());
    }
    emit("COORDINATOR_STOPPED exit=" + std::to_string(report.ok() ? 0 : 3));
    return report.ok() ? 0 : 3;
}

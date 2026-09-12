// Real OS process spawning for the multiprocess proof.
//
// Children are created inside a job object configured with
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so a suite that exits unexpectedly still
// leaves no orphaned coordinator or agent behind.

#ifndef HAF_TEST_PROCESS_HPP
#define HAF_TEST_PROCESS_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace haf::test {

struct ProcessResult {
    int exit_code{-1};
    bool started{false};
    std::string output;
};

class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Start a child. \p arguments excludes argv[0]. When \p stdout_path is
    /// non-empty the child's combined standard output and error are redirected
    /// to that file, which lets a test observe startup lines deterministically
    /// instead of guessing at sleeps.
    bool start(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
               const std::filesystem::path& working_directory,
               const std::filesystem::path& stdout_path = std::filesystem::path());

    /// True while the child is running.
    [[nodiscard]] bool running() const;

    /// Wait for exit and return the exit code. Returns -1 on failure.
    int wait();

    /// Terminate the child immediately (simulates a crash/kill).
    void kill();

    [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

    /// Reap the process without blocking if it has already exited.
    void poll();

    [[nodiscard]] bool exited() const noexcept { return exited_; }
    [[nodiscard]] int exit_code() const noexcept { return exit_code_; }
    /// Human-readable reason the last start() call failed. Empty on success.
    [[nodiscard]] const std::string& start_error() const noexcept { return start_error_; }

private:
    void close_handles();

    void* process_handle_{nullptr};
    void* thread_handle_{nullptr};
    std::uint64_t pid_{0};
    bool exited_{false};
    int exit_code_{-1};
    std::string start_error_;
};

/// Spawn a child, wait for it to exit, and capture combined stdout/stderr.
[[nodiscard]] ProcessResult run_process(const std::filesystem::path& executable,
                                        const std::vector<std::string>& arguments,
                                        const std::filesystem::path& working_directory);

/// True when any process started through this helper is still alive.
[[nodiscard]] bool any_child_running();

/// Kill every live child. Called at suite shutdown.
void kill_all_children();

}  // namespace haf::test

#endif  // HAF_TEST_PROCESS_HPP

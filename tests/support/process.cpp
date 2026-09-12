#include "process.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace haf::test {
namespace {

std::mutex g_children_mutex;
std::vector<ChildProcess*> g_children;

#if defined(_WIN32)

[[nodiscard]] HANDLE job_handle() {
    static HANDLE job = []() -> HANDLE {
        HANDLE created = CreateJobObjectW(nullptr, nullptr);
        if (created == nullptr) {
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
        information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(created, JobObjectExtendedLimitInformation, &information, sizeof(information)) == 0) {
            CloseHandle(created);
            return nullptr;
        }
        return created;
    }();
    return job;
}

[[nodiscard]] std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

[[nodiscard]] std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string narrow_text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow_text.data(), size, nullptr,
                        nullptr);
    return narrow_text;
}

[[nodiscard]] std::string quote_argument(const std::string& argument) {
    if (argument.find_first_of(" \t\"") == std::string::npos) {
        return argument;
    }
    std::string out = "\"";
    for (const char c : argument) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out += "\"";
    return out;
}

#endif

}  // namespace

ChildProcess::ChildProcess() {
    std::lock_guard<std::mutex> guard(g_children_mutex);
    g_children.push_back(this);
}

ChildProcess::~ChildProcess() {
    if (!exited_) {
        kill();
        wait();
    }
    close_handles();
    std::lock_guard<std::mutex> guard(g_children_mutex);
    g_children.erase(std::remove(g_children.begin(), g_children.end(), this), g_children.end());
}

void ChildProcess::close_handles() {
#if defined(_WIN32)
    if (thread_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(thread_handle_));
        thread_handle_ = nullptr;
    }
    if (process_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(process_handle_));
        process_handle_ = nullptr;
    }
#endif
}

bool ChildProcess::start(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                         const std::filesystem::path& working_directory,
                         const std::filesystem::path& stdout_path) {
#if defined(_WIN32)
    std::string command_line = quote_argument(executable.string());
    for (const std::string& argument : arguments) {
        command_line.push_back(' ');
        command_line += quote_argument(argument);
    }
    std::wstring wide_command = widen(command_line);
    std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};

    HANDLE output_file = INVALID_HANDLE_VALUE;
    if (!stdout_path.empty()) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        output_file = CreateFileW(stdout_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output_file == INVALID_HANDLE_VALUE) {
            start_error_ = "CreateFileW failed with error " + std::to_string(GetLastError()) + " for '" +
                           stdout_path.string() + "'";
            return false;
        }
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = output_file;
        startup.hStdError = output_file;
        startup.hStdInput = nullptr;
    }

    const std::wstring wide_working = working_directory.empty() ? std::wstring() : working_directory.wstring();
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr,
                                        stdout_path.empty() ? FALSE : TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                        wide_working.empty() ? nullptr : wide_working.c_str(), &startup, &information);
    if (output_file != INVALID_HANDLE_VALUE) {
        CloseHandle(output_file);
    }
    if (created == 0) {
        start_error_ = "CreateProcessW failed with error " + std::to_string(GetLastError()) + " for '" +
                       executable.string() + "'";
        return false;
    }
    start_error_.clear();
    HANDLE job = job_handle();
    if (job != nullptr) {
        AssignProcessToJobObject(job, information.hProcess);
    }
    ResumeThread(information.hThread);
    process_handle_ = information.hProcess;
    thread_handle_ = information.hThread;
    pid_ = static_cast<std::uint64_t>(information.dwProcessId);
    exited_ = false;
    exit_code_ = -1;
    return true;
#else
    static_cast<void>(stdout_path);
    const std::string path = executable.string();
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(path.c_str()));
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        if (!working_directory.empty()) {
            static_cast<void>(chdir(working_directory.c_str()));
        }
        execv(path.c_str(), argv.data());
        _exit(127);
    }
    pid_ = static_cast<std::uint64_t>(child);
    process_handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(child));
    exited_ = false;
    exit_code_ = -1;
    return true;
#endif
}

bool ChildProcess::running() const {
#if defined(_WIN32)
    if (exited_) {
        return false;
    }
    if (process_handle_ == nullptr) {
        return false;
    }
    return WaitForSingleObject(static_cast<HANDLE>(process_handle_), 0) == WAIT_TIMEOUT;
#else
    if (exited_) {
        return false;
    }
    int status = 0;
    const pid_t result = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (result == 0) {
        return true;
    }
    return false;
#endif
}

void ChildProcess::poll() {
#if defined(_WIN32)
    if (exited_ || process_handle_ == nullptr) {
        return;
    }
    DWORD code = 0;
    if (WaitForSingleObject(static_cast<HANDLE>(process_handle_), 0) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code) != 0) {
        exited_ = true;
        exit_code_ = static_cast<int>(code);
    }
#else
    if (exited_) {
        return;
    }
    int status = 0;
    const pid_t result = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (result == static_cast<pid_t>(pid_)) {
        exited_ = true;
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif
}

int ChildProcess::wait() {
#if defined(_WIN32)
    if (!exited_ && process_handle_ != nullptr) {
        WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE);
        DWORD code = 0;
        if (GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code) != 0) {
            exit_code_ = static_cast<int>(code);
        }
        exited_ = true;
    }
    return exit_code_;
#else
    if (!exited_ && pid_ != 0) {
        int status = 0;
        if (waitpid(static_cast<pid_t>(pid_), &status, 0) == static_cast<pid_t>(pid_)) {
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        exited_ = true;
    }
    return exit_code_;
#endif
}

void ChildProcess::kill() {
#if defined(_WIN32)
    if (process_handle_ != nullptr && !exited_) {
        TerminateProcess(static_cast<HANDLE>(process_handle_), 137U);
    }
#else
    if (pid_ != 0 && !exited_) {
        ::kill(static_cast<pid_t>(pid_), SIGKILL);
    }
#endif
}

bool any_child_running() {
    std::lock_guard<std::mutex> guard(g_children_mutex);
    for (ChildProcess* child : g_children) {
        if (child->running()) {
            return true;
        }
    }
    return false;
}

void kill_all_children() {
    std::vector<ChildProcess*> children;
    {
        std::lock_guard<std::mutex> guard(g_children_mutex);
        children = g_children;
    }
    for (ChildProcess* child : children) {
        child->kill();
    }
    for (ChildProcess* child : children) {
        child->wait();
    }
}

ProcessResult run_process(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                          const std::filesystem::path& working_directory) {
    ProcessResult result;
#if defined(_WIN32)
    std::string command_line = quote_argument(executable.string());
    for (const std::string& argument : arguments) {
        command_line.push_back(' ');
        command_line += quote_argument(argument);
    }
    std::wstring wide_command = widen(command_line);
    std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
    mutable_command.push_back(L'\0');

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
        return result;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION information{};

    const std::wstring wide_working = working_directory.empty() ? std::wstring() : working_directory.wstring();
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr,
                                        wide_working.empty() ? nullptr : wide_working.c_str(), &startup, &information);
    CloseHandle(write_end);
    if (created == 0) {
        CloseHandle(read_end);
        return result;
    }
    if (job_handle() != nullptr) {
        AssignProcessToJobObject(job_handle(), information.hProcess);
    }
    std::string output;
    char buffer[4096];
    DWORD read_bytes = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &read_bytes, nullptr) != 0 && read_bytes != 0) {
        output.append(buffer, read_bytes);
    }
    WaitForSingleObject(information.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(information.hProcess, &code);
    CloseHandle(read_end);
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
    result.started = true;
    result.exit_code = static_cast<int>(code);
    result.output = std::move(output);
    return result;
#else
    ChildProcess child;
    if (!child.start(executable, arguments, working_directory)) {
        return result;
    }
    result.started = true;
    result.exit_code = child.wait();
    return result;
#endif
}

}  // namespace haf::test

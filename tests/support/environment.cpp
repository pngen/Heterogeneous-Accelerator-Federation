#include "environment.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace haf::test {
namespace {

std::mutex g_cleanup_mutex;
std::vector<std::filesystem::path> g_cleanup_paths;
std::atomic<std::uint64_t> g_counter{0};

[[nodiscard]] std::filesystem::path temp_root() {
    std::error_code error;
    std::filesystem::path root = std::filesystem::temp_directory_path(error);
    if (error) {
        root = std::filesystem::current_path(error);
    }
    return root / "haf-tests";
}

[[nodiscard]] std::string unique_suffix(const std::string& tag) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream stream;
    stream << tag << "-" << static_cast<unsigned long long>(now) << "-" << g_counter.fetch_add(1);
    return stream.str();
}

}  // namespace

std::filesystem::path suite_binary_dir() {
#ifdef HAF_SUITE_BINARY_DIR
    return std::filesystem::path(HAF_SUITE_BINARY_DIR);
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path apps_binary_dir() {
#ifdef HAF_APPS_BINARY_DIR
    return std::filesystem::path(HAF_APPS_BINARY_DIR);
#else
    return suite_binary_dir();
#endif
}

std::filesystem::path app_path(const std::string& name) {
#if defined(_WIN32)
    return apps_binary_dir() / (name + ".exe");
#else
    return apps_binary_dir() / name;
#endif
}

ScratchDirectory::ScratchDirectory(const std::string& tag) {
    path_ = temp_root() / unique_suffix(tag);
    std::error_code error;
    std::filesystem::create_directories(path_, error);
}

ScratchDirectory::~ScratchDirectory() { cleanup(); }

void ScratchDirectory::cleanup() {
    if (keep_scratch()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

void register_cleanup_path(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> guard(g_cleanup_mutex);
    g_cleanup_paths.push_back(path);
}

void cleanup_registered_paths() {
    std::vector<std::filesystem::path> paths;
    {
        std::lock_guard<std::mutex> guard(g_cleanup_mutex);
        paths = g_cleanup_paths;
        g_cleanup_paths.clear();
    }
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    if (keep_scratch()) {
        return;
    }
    std::error_code error;
    // Remove the shared root when it is empty so that validation leaves no
    // scratch tree behind in the system temporary directory.
    std::filesystem::remove(temp_root(), error);
}

std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

bool write_file_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

bool replace_in_file(const std::filesystem::path& path, const std::string& from, const std::string& to) {
    std::string text = read_file_text(path);
    if (text.empty() && from.empty()) {
        return false;
    }
    const std::size_t position = text.find(from);
    if (position == std::string::npos) {
        return false;
    }
    text.replace(position, from.size(), to);
    return write_file_text(path, text);
}

bool keep_scratch() {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "HAF_TEST_KEEP_SCRATCH") != 0 || buffer == nullptr) {
        return false;
    }
    const std::string value(buffer);
    std::free(buffer);
    return value == "1";
#else
    const char* value = std::getenv("HAF_TEST_KEEP_SCRATCH");
    return value != nullptr && std::string(value) == "1";
#endif
}

}  // namespace haf::test

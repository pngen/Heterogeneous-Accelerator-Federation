// Test environment helpers: scratch directories, artifact paths, cleanup.

#ifndef HAF_TEST_ENVIRONMENT_HPP
#define HAF_TEST_ENVIRONMENT_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace haf::test {

/// Directory the suite executable was built into.
[[nodiscard]] std::filesystem::path suite_binary_dir();

/// Directory holding haf_coordinator, haf_agent, and haf_cli.
[[nodiscard]] std::filesystem::path apps_binary_dir();

/// Build the path of a built executable, appending the platform suffix.
[[nodiscard]] std::filesystem::path app_path(const std::string& name);

/// A unique scratch directory for one case. Created on first use.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::string& tag);
    ~ScratchDirectory();

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

    /// Remove the tree. Safe to call more than once.
    void cleanup();

private:
    std::filesystem::path path_;
};

/// Register a filesystem path for removal when the process exits normally.
void register_cleanup_path(const std::filesystem::path& path);

/// Remove every registered path. Called from the suite runner.
void cleanup_registered_paths();

/// Read a whole file as text. Returns an empty string on failure.
[[nodiscard]] std::string read_file_text(const std::filesystem::path& path);

/// Write text to a file, creating parent directories.
bool write_file_text(const std::filesystem::path& path, const std::string& text);

/// Replace every occurrence of \p from with \p to inside a file, in place.
bool replace_in_file(const std::filesystem::path& path, const std::string& from, const std::string& to);

/// True when environment variable HAF_TEST_KEEP_SCRATCH is set to "1".
[[nodiscard]] bool keep_scratch();

}  // namespace haf::test

#endif  // HAF_TEST_ENVIRONMENT_HPP

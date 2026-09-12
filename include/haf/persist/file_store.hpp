// Heterogeneous Accelerator Federation - integrity-protected file store.
//
// On-disk layout (all integers little-endian):
//
//   offset  size  field
//   0       8     magic "HAFSTR01"
//   8       4     container format version
//   12      4     reserved, must be zero
//   16      8     payload length
//   24      32    SHA-256 of the payload
//   56      8     header CRC (FNV-1a 64 over bytes 0..55)
//   64      N     payload
//   64+N    8     footer magic "HAFEND01"
//   72+N    8     payload length repeated
//
// Everything is validated before the payload is handed to a decoder: magic,
// version, reserved word, header checksum, payload length against a hard bound
// and against the real file size, payload digest, footer magic and repeated
// length. Truncation, corruption, and trailing garbage are all rejected.

#ifndef HAF_PERSIST_FILE_STORE_HPP
#define HAF_PERSIST_FILE_STORE_HPP

#include <filesystem>
#include <string>

#include "haf/persist/store.hpp"

namespace haf {

class FileStore final : public FederationStore {
public:
    explicit FileStore(std::filesystem::path path);

    [[nodiscard]] Result<ByteBuffer> read() const override;
    [[nodiscard]] VoidResult write(const ByteBuffer& payload) override;
    [[nodiscard]] VoidResult erase() override;
    [[nodiscard]] std::string location() const override;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Verify a file without keeping it decoded. Exposed for CLI use.
    [[nodiscard]] VoidResult verify() const;

private:
    std::filesystem::path path_;
};

}  // namespace haf

#endif  // HAF_PERSIST_FILE_STORE_HPP

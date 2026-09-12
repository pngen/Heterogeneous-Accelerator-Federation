#include "haf/persist/file_store.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <ios>
#include <system_error>
#include <vector>

#include "haf/core/hash.hpp"
#include "haf/core/limits.hpp"

namespace haf {
namespace {

constexpr char kMagic[8] = {'H', 'A', 'F', 'S', 'T', 'R', '0', '1'};
constexpr char kFooterMagic[8] = {'H', 'A', 'F', 'E', 'N', 'D', '0', '1'};
constexpr std::uint32_t kContainerVersion = 1;
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kFooterBytes = 16;

void put_u32(std::uint8_t* out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

void put_u64(std::uint8_t* out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFULL);
    }
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* in) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(in[i]) << (i * 8);
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(in[i]) << (i * 8);
    }
    return value;
}

[[nodiscard]] std::uint64_t header_crc(const std::uint8_t* header) {
    Fnv1a64 hasher;
    hasher.update(header, kHeaderBytes - 8);
    return hasher.digest();
}

[[nodiscard]] std::string path_text(const std::filesystem::path& path) { return path.string(); }

}  // namespace

FileStore::FileStore(std::filesystem::path path) : path_(std::move(path)) {}

[[nodiscard]] VoidResult FileStore::write(const ByteBuffer& payload) {
    if (payload.size() > Limits::kMaxStorePayloadBytes) {
        return Status(ErrorCode::BoundsExceeded, "payload exceeds the maximum persisted size");
    }
    std::error_code error;
    const std::filesystem::path parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return Status(ErrorCode::PersistenceFailure,
                          "cannot create store directory '" + parent.string() + "': " + error.message());
        }
    }

    ByteBuffer container;
    container.resize(kHeaderBytes + payload.size() + kFooterBytes);
    std::memcpy(container.data(), kMagic, sizeof(kMagic));
    put_u32(container.data() + 8, kContainerVersion);
    put_u32(container.data() + 12, 0);
    put_u64(container.data() + 16, static_cast<std::uint64_t>(payload.size()));
    const Sha256::digest_type digest = Sha256::hash(payload.data(), payload.size());
    std::memcpy(container.data() + 24, digest.data(), digest.size());
    put_u64(container.data() + 56, header_crc(container.data()));
    if (!payload.empty()) {
        std::memcpy(container.data() + kHeaderBytes, payload.data(), payload.size());
    }
    std::memcpy(container.data() + kHeaderBytes + payload.size(), kFooterMagic, sizeof(kFooterMagic));
    put_u64(container.data() + kHeaderBytes + payload.size() + 8, static_cast<std::uint64_t>(payload.size()));

    // Atomic replacement: write a sibling temporary file, flush it, then move
    // it over the target. A crash leaves either the old file or the new one.
    const std::filesystem::path temporary = path_.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Status(ErrorCode::PersistenceFailure, "cannot open temporary store file for writing");
        }
        out.write(reinterpret_cast<const char*>(container.data()), static_cast<std::streamsize>(container.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temporary, error);
            return Status(ErrorCode::PersistenceFailure, "failed while writing the temporary store file");
        }
    }
    std::filesystem::rename(temporary, path_, error);
    if (error) {
        // Windows cannot rename over an existing file; fall back to a
        // backup-and-replace sequence that never leaves the target missing.
        const std::filesystem::path backup = path_.string() + ".bak";
        std::filesystem::remove(backup, error);
        error.clear();
        if (std::filesystem::exists(path_)) {
            std::filesystem::rename(path_, backup, error);
            if (error) {
                std::filesystem::remove(temporary, error);
                return Status(ErrorCode::PersistenceFailure,
                              "cannot move the previous store file aside: " + error.message());
            }
        }
        std::filesystem::rename(temporary, path_, error);
        if (error) {
            std::error_code restore_error;
            std::filesystem::rename(backup, path_, restore_error);
            std::filesystem::remove(temporary, error);
            return Status(ErrorCode::PersistenceFailure,
                          "cannot publish the new store file: " + error.message());
        }
        std::error_code cleanup_error;
        std::filesystem::remove(backup, cleanup_error);
    }
    return VoidResult();
}

Result<ByteBuffer> FileStore::read() const {
    std::error_code error;
    if (!std::filesystem::exists(path_, error)) {
        return Status(ErrorCode::NotFound, "store file does not exist: " + path_text(path_));
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path_, error);
    if (error) {
        return Status(ErrorCode::PersistenceFailure, "cannot size the store file: " + error.message());
    }
    if (file_size < kHeaderBytes + kFooterBytes) {
        return Status(ErrorCode::TruncatedStore, "store file is smaller than an empty container");
    }
    if (file_size > Limits::kMaxStorePayloadBytes + kHeaderBytes + kFooterBytes) {
        return Status(ErrorCode::BoundsExceeded, "store file exceeds the maximum permitted size");
    }
    ByteBuffer file_bytes;
    file_bytes.resize(static_cast<std::size_t>(file_size));
    {
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            return Status(ErrorCode::PersistenceFailure, "cannot open the store file for reading");
        }
        in.read(reinterpret_cast<char*>(file_bytes.data()), static_cast<std::streamsize>(file_bytes.size()));
        if (in.gcount() != static_cast<std::streamsize>(file_bytes.size())) {
            return Status(ErrorCode::TruncatedStore, "store file ended before the declared size");
        }
    }

    if (std::memcmp(file_bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return Status(ErrorCode::CorruptStore, "store file magic is not HAFFederation container magic");
    }
    const std::uint32_t version = get_u32(file_bytes.data() + 8);
    if (version != kContainerVersion) {
        return Status(ErrorCode::UnsupportedStoreVersion, "store container version is not supported");
    }
    if (get_u32(file_bytes.data() + 12) != 0) {
        return Status(ErrorCode::CorruptStore, "store header reserved word is not zero");
    }
    if (get_u64(file_bytes.data() + 56) != header_crc(file_bytes.data())) {
        return Status(ErrorCode::IntegrityFailure, "store header checksum does not match");
    }
    const std::uint64_t declared = get_u64(file_bytes.data() + 16);
    if (declared > Limits::kMaxStorePayloadBytes) {
        return Status(ErrorCode::BoundsExceeded, "store declares a payload larger than the supported maximum");
    }
    const std::uint64_t expected_size = static_cast<std::uint64_t>(kHeaderBytes) + declared + kFooterBytes;
    if (expected_size != static_cast<std::uint64_t>(file_size)) {
        return Status(ErrorCode::TruncatedStore,
                      "store file size does not match the declared payload length");
    }
    const std::size_t payload_offset = kHeaderBytes;
    const std::size_t footer_offset = kHeaderBytes + static_cast<std::size_t>(declared);
    if (std::memcmp(file_bytes.data() + footer_offset, kFooterMagic, sizeof(kFooterMagic)) != 0) {
        return Status(ErrorCode::TruncatedStore, "store footer magic is missing");
    }
    if (get_u64(file_bytes.data() + footer_offset + 8) != declared) {
        return Status(ErrorCode::TruncatedStore, "store footer length does not match the header");
    }
    const Sha256::digest_type digest =
        Sha256::hash(file_bytes.data() + payload_offset, static_cast<std::size_t>(declared));
    if (std::memcmp(digest.data(), file_bytes.data() + 24, digest.size()) != 0) {
        return Status(ErrorCode::IntegrityFailure, "store payload digest does not match");
    }
    ByteBuffer payload;
    payload.assign(file_bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                   file_bytes.begin() + static_cast<std::ptrdiff_t>(footer_offset));
    return payload;
}

VoidResult FileStore::erase() {
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::remove(path_.string() + ".tmp", error);
    std::filesystem::remove(path_.string() + ".bak", error);
    return VoidResult();
}

std::string FileStore::location() const { return path_text(path_); }

VoidResult FileStore::verify() const {
    const Result<ByteBuffer> payload = read();
    if (!payload.ok()) {
        return payload.status();
    }
    return VoidResult();
}

}  // namespace haf

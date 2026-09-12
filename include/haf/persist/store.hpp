// Heterogeneous Accelerator Federation - durable store interface.

#ifndef HAF_PERSIST_STORE_HPP
#define HAF_PERSIST_STORE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/status.hpp"

namespace haf {

/// Abstract durable store for the federation payload.
///
/// Implementations must provide atomic replacement: a reader must observe
/// either the previous complete payload or the next complete payload, never a
/// mixture, and never a truncated file.
class FederationStore {
public:
    FederationStore() = default;
    virtual ~FederationStore() = default;

    FederationStore(const FederationStore&) = delete;
    FederationStore& operator=(const FederationStore&) = delete;

    /// Read the current payload. Returns NotFound when nothing has been stored.
    [[nodiscard]] virtual Result<ByteBuffer> read() const = 0;

    /// Atomically replace the payload.
    [[nodiscard]] virtual VoidResult write(const ByteBuffer& payload) = 0;

    /// Remove all durable state.
    [[nodiscard]] virtual VoidResult erase() = 0;

    /// Human-readable location, for diagnostics. Empty for memory stores.
    [[nodiscard]] virtual std::string location() const = 0;
};

/// In-memory store used by tests and by federations created with no path.
class MemoryStore final : public FederationStore {
public:
    MemoryStore() = default;

    [[nodiscard]] Result<ByteBuffer> read() const override;
    [[nodiscard]] VoidResult write(const ByteBuffer& payload) override;
    [[nodiscard]] VoidResult erase() override;
    [[nodiscard]] std::string location() const override;

private:
    ByteBuffer payload_;
    bool present_{false};
};

}  // namespace haf

#endif  // HAF_PERSIST_STORE_HPP

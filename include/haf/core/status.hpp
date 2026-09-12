// Heterogeneous Accelerator Federation - typed status and error semantics.
//
// Errors are machine-readable first. Human-readable text supplements the code
// but never replaces it. Exceptions are not used as ordinary control flow at
// runtime or protocol boundaries.

#ifndef HAF_CORE_STATUS_HPP
#define HAF_CORE_STATUS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace haf {

/// Stable machine-readable error domain. Values are part of the public
/// contract: they are stable across releases and safe to switch on and to
/// transmit over the control plane.
enum class ErrorCode : std::uint16_t {
    Ok = 0,

    // Input / data shape
    InvalidArgument = 1,
    MalformedData = 2,
    MalformedAdvertisement = 3,
    MalformedVersionRange = 4,
    NonFiniteQuantity = 5,
    ImpossibleQuantity = 6,
    BoundsExceeded = 7,
    Overflow = 8,

    // Capability semantics
    UnsupportedCapability = 20,
    UnknownCapability = 21,
    CapabilityMismatch = 22,
    CapabilityDowngrade = 23,
    ContradictoryCapabilities = 24,
    MissingMandatoryCapability = 25,

    // Compatibility
    CompatibilityRejection = 40,
    VendorMismatch = 41,
    ArchitectureMismatch = 42,
    RuntimeMismatch = 43,
    RuntimeVersionUnsupported = 44,
    IsaIncompatible = 45,
    InsufficientMemory = 46,
    NumericModeUnsupported = 47,
    PolicyMismatch = 48,
    PortabilityConstraintViolated = 49,
    MigrationUnsupported = 50,

    // Authority / staleness
    StaleEpoch = 60,
    StaleAgent = 61,
    StaleBoot = 62,
    StaleDeviceGeneration = 63,
    StaleCapabilityGeneration = 64,
    StalePolicy = 65,
    StaleDecision = 66,
    StaleMigration = 67,
    StaleFederationGeneration = 68,
    StaleEvidence = 69,
    AuthorityRevoked = 70,

    // Identity / lifecycle
    UnknownAccelerator = 80,
    UnknownAgent = 81,
    UnknownEntity = 82,
    DuplicateIdentity = 83,
    DuplicateLiveBoot = 84,
    AlreadyExists = 85,
    NotFound = 86,
    InvalidTransition = 87,
    NotAdmitted = 88,
    Fenced = 89,
    Retired = 90,
    DuplicateMessage = 91,

    // Protocol / transport
    ProtocolViolation = 100,
    UnknownMessageType = 101,
    UnsupportedProtocolVersion = 102,
    FrameTooLarge = 103,
    TruncatedFrame = 104,
    CorruptPayload = 105,
    TransportFailure = 106,
    ConnectionClosed = 107,
    HandshakeFailed = 108,

    // Persistence
    PersistenceFailure = 120,
    IntegrityFailure = 121,
    TruncatedStore = 122,
    UnsupportedStoreVersion = 123,
    CorruptStore = 124,

    // Runtime
    ResourceExhausted = 140,
    ShutdownInProgress = 141,
    Cancelled = 142,
    Timeout = 143,
    InternalInvariantViolation = 144,
    Unsupported = 145,
};

/// Stable symbolic name, safe for logs, CLI output, and wire use.
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/// True when the code describes an authority/generation staleness rejection.
[[nodiscard]] bool is_staleness(ErrorCode code) noexcept;

/// True when the code describes a compatibility outcome rather than a fault.
[[nodiscard]] bool is_compatibility_outcome(ErrorCode code) noexcept;

/// Typed status: machine-readable code plus supplementary human text.
class Status {
public:
    Status() noexcept = default;
    Status(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

    [[nodiscard]] static Status success() noexcept { return Status(); }
    [[nodiscard]] static Status failure(ErrorCode code, std::string message) {
        return Status(code, std::move(message));
    }

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// "code: message" or "Ok".
    [[nodiscard]] std::string describe() const;

private:
    ErrorCode code_{ErrorCode::Ok};
    std::string message_;
};

/// Result carrying a value or a Status. The value is only observable when the
/// status is Ok, so a failed call can never hand back partially valid data.
template <class T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Status status) : value_(std::nullopt), status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Status& status() const noexcept { return status_; }

    [[nodiscard]] const T& value() const noexcept { return *value_; }
    [[nodiscard]] T& value() noexcept { return *value_; }
    [[nodiscard]] const T& operator*() const noexcept { return *value_; }
    [[nodiscard]] T& operator*() noexcept { return *value_; }
    [[nodiscard]] const T* operator->() const noexcept { return &(*value_); }
    [[nodiscard]] T* operator->() noexcept { return &(*value_); }

    /// Value, or a caller-provided fallback when the result is a failure.
    [[nodiscard]] T value_or(T fallback) const {
        return value_.has_value() ? *value_ : std::move(fallback);
    }

private:
    std::optional<T> value_;
    Status status_;
};

/// Result with no payload.
class VoidResult {
public:
    VoidResult() = default;
    VoidResult(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    Status status_;
};

}  // namespace haf

#endif  // HAF_CORE_STATUS_HPP

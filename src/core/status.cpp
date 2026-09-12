#include "haf/core/status.hpp"

namespace haf {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::MalformedData: return "MalformedData";
        case ErrorCode::MalformedAdvertisement: return "MalformedAdvertisement";
        case ErrorCode::MalformedVersionRange: return "MalformedVersionRange";
        case ErrorCode::NonFiniteQuantity: return "NonFiniteQuantity";
        case ErrorCode::ImpossibleQuantity: return "ImpossibleQuantity";
        case ErrorCode::BoundsExceeded: return "BoundsExceeded";
        case ErrorCode::Overflow: return "Overflow";
        case ErrorCode::UnsupportedCapability: return "UnsupportedCapability";
        case ErrorCode::UnknownCapability: return "UnknownCapability";
        case ErrorCode::CapabilityMismatch: return "CapabilityMismatch";
        case ErrorCode::CapabilityDowngrade: return "CapabilityDowngrade";
        case ErrorCode::ContradictoryCapabilities: return "ContradictoryCapabilities";
        case ErrorCode::MissingMandatoryCapability: return "MissingMandatoryCapability";
        case ErrorCode::CompatibilityRejection: return "CompatibilityRejection";
        case ErrorCode::VendorMismatch: return "VendorMismatch";
        case ErrorCode::ArchitectureMismatch: return "ArchitectureMismatch";
        case ErrorCode::RuntimeMismatch: return "RuntimeMismatch";
        case ErrorCode::RuntimeVersionUnsupported: return "RuntimeVersionUnsupported";
        case ErrorCode::IsaIncompatible: return "IsaIncompatible";
        case ErrorCode::InsufficientMemory: return "InsufficientMemory";
        case ErrorCode::NumericModeUnsupported: return "NumericModeUnsupported";
        case ErrorCode::PolicyMismatch: return "PolicyMismatch";
        case ErrorCode::PortabilityConstraintViolated: return "PortabilityConstraintViolated";
        case ErrorCode::MigrationUnsupported: return "MigrationUnsupported";
        case ErrorCode::StaleEpoch: return "StaleEpoch";
        case ErrorCode::StaleAgent: return "StaleAgent";
        case ErrorCode::StaleBoot: return "StaleBoot";
        case ErrorCode::StaleDeviceGeneration: return "StaleDeviceGeneration";
        case ErrorCode::StaleCapabilityGeneration: return "StaleCapabilityGeneration";
        case ErrorCode::StalePolicy: return "StalePolicy";
        case ErrorCode::StaleDecision: return "StaleDecision";
        case ErrorCode::StaleMigration: return "StaleMigration";
        case ErrorCode::StaleFederationGeneration: return "StaleFederationGeneration";
        case ErrorCode::StaleEvidence: return "StaleEvidence";
        case ErrorCode::AuthorityRevoked: return "AuthorityRevoked";
        case ErrorCode::UnknownAccelerator: return "UnknownAccelerator";
        case ErrorCode::UnknownAgent: return "UnknownAgent";
        case ErrorCode::UnknownEntity: return "UnknownEntity";
        case ErrorCode::DuplicateIdentity: return "DuplicateIdentity";
        case ErrorCode::DuplicateLiveBoot: return "DuplicateLiveBoot";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::InvalidTransition: return "InvalidTransition";
        case ErrorCode::NotAdmitted: return "NotAdmitted";
        case ErrorCode::Fenced: return "Fenced";
        case ErrorCode::Retired: return "Retired";
        case ErrorCode::DuplicateMessage: return "DuplicateMessage";
        case ErrorCode::ProtocolViolation: return "ProtocolViolation";
        case ErrorCode::UnknownMessageType: return "UnknownMessageType";
        case ErrorCode::UnsupportedProtocolVersion: return "UnsupportedProtocolVersion";
        case ErrorCode::FrameTooLarge: return "FrameTooLarge";
        case ErrorCode::TruncatedFrame: return "TruncatedFrame";
        case ErrorCode::CorruptPayload: return "CorruptPayload";
        case ErrorCode::TransportFailure: return "TransportFailure";
        case ErrorCode::ConnectionClosed: return "ConnectionClosed";
        case ErrorCode::HandshakeFailed: return "HandshakeFailed";
        case ErrorCode::PersistenceFailure: return "PersistenceFailure";
        case ErrorCode::IntegrityFailure: return "IntegrityFailure";
        case ErrorCode::TruncatedStore: return "TruncatedStore";
        case ErrorCode::UnsupportedStoreVersion: return "UnsupportedStoreVersion";
        case ErrorCode::CorruptStore: return "CorruptStore";
        case ErrorCode::ResourceExhausted: return "ResourceExhausted";
        case ErrorCode::ShutdownInProgress: return "ShutdownInProgress";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::InternalInvariantViolation: return "InternalInvariantViolation";
        case ErrorCode::Unsupported: return "Unsupported";
    }
    return "UnknownErrorCode";
}

bool is_staleness(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::StaleEpoch:
        case ErrorCode::StaleAgent:
        case ErrorCode::StaleBoot:
        case ErrorCode::StaleDeviceGeneration:
        case ErrorCode::StaleCapabilityGeneration:
        case ErrorCode::StalePolicy:
        case ErrorCode::StaleDecision:
        case ErrorCode::StaleMigration:
        case ErrorCode::StaleFederationGeneration:
        case ErrorCode::StaleEvidence:
        case ErrorCode::AuthorityRevoked:
            return true;
        default:
            return false;
    }
}

bool is_compatibility_outcome(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::CompatibilityRejection:
        case ErrorCode::VendorMismatch:
        case ErrorCode::ArchitectureMismatch:
        case ErrorCode::RuntimeMismatch:
        case ErrorCode::RuntimeVersionUnsupported:
        case ErrorCode::IsaIncompatible:
        case ErrorCode::InsufficientMemory:
        case ErrorCode::NumericModeUnsupported:
        case ErrorCode::PolicyMismatch:
        case ErrorCode::PortabilityConstraintViolated:
        case ErrorCode::MigrationUnsupported:
        case ErrorCode::UnsupportedCapability:
        case ErrorCode::UnknownCapability:
        case ErrorCode::CapabilityMismatch:
        case ErrorCode::MissingMandatoryCapability:
            return true;
        default:
            return false;
    }
}

std::string Status::describe() const {
    if (code_ == ErrorCode::Ok) {
        return "Ok";
    }
    std::string out(to_string(code_));
    if (!message_.empty()) {
        out += ": ";
        out += message_;
    }
    return out;
}

}  // namespace haf

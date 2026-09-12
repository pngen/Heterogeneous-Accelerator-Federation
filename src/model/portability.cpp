#include "haf/model/portability.hpp"

namespace haf {

std::string_view to_string(PortabilityClass value) noexcept {
    switch (value) {
        case PortabilityClass::Unsupported: return "UNSUPPORTED";
        case PortabilityClass::Unknown: return "UNKNOWN";
        case PortabilityClass::RepackageAndRestartRequired: return "REPACKAGE_AND_RESTART_REQUIRED";
        case PortabilityClass::StateReconstructionRequired: return "STATE_RECONSTRUCTION_REQUIRED";
        case PortabilityClass::RepackageRequired: return "REPACKAGE_REQUIRED";
        case PortabilityClass::RecompileRequired: return "RECOMPILE_REQUIRED";
        case PortabilityClass::CheckpointRestoreSupported: return "CHECKPOINT_RESTORE_SUPPORTED";
        case PortabilityClass::BinaryCompatible: return "BINARY_COMPATIBLE";
        case PortabilityClass::Native: return "NATIVE";
        case PortabilityClass::LiveMigrationSupported: return "LIVE_MIGRATION_SUPPORTED";
    }
    return "UNKNOWN";
}

bool portability_class_from_token(std::string_view token, PortabilityClass& out) noexcept {
    if (token == "unsupported") { out = PortabilityClass::Unsupported; return true; }
    if (token == "unknown") { out = PortabilityClass::Unknown; return true; }
    if (token == "repackage_and_restart_required") { out = PortabilityClass::RepackageAndRestartRequired; return true; }
    if (token == "state_reconstruction_required") { out = PortabilityClass::StateReconstructionRequired; return true; }
    if (token == "repackage_required") { out = PortabilityClass::RepackageRequired; return true; }
    if (token == "recompile_required") { out = PortabilityClass::RecompileRequired; return true; }
    if (token == "checkpoint_restore_supported") { out = PortabilityClass::CheckpointRestoreSupported; return true; }
    if (token == "binary_compatible") { out = PortabilityClass::BinaryCompatible; return true; }
    if (token == "native") { out = PortabilityClass::Native; return true; }
    if (token == "live_migration_supported") { out = PortabilityClass::LiveMigrationSupported; return true; }
    return false;
}

bool portability_class_from_wire(std::uint8_t raw, PortabilityClass& out) noexcept {
    if (raw > static_cast<std::uint8_t>(PortabilityClass::LiveMigrationSupported)) {
        return false;
    }
    out = static_cast<PortabilityClass>(raw);
    return true;
}

std::uint8_t portability_strength(PortabilityClass value) noexcept { return static_cast<std::uint8_t>(value); }

bool meets_portability_threshold(PortabilityClass value, PortabilityClass minimum) noexcept {
    if (value == PortabilityClass::Unknown) {
        return false;
    }
    if (minimum == PortabilityClass::Unknown) {
        return false;
    }
    return portability_strength(value) >= portability_strength(minimum);
}

std::uint8_t portability_transformation_steps(PortabilityClass value) noexcept {
    switch (value) {
        case PortabilityClass::Native: return 0;
        case PortabilityClass::LiveMigrationSupported: return 0;
        case PortabilityClass::BinaryCompatible: return 0;
        case PortabilityClass::CheckpointRestoreSupported: return 1;
        case PortabilityClass::RecompileRequired: return 2;
        case PortabilityClass::RepackageRequired: return 2;
        case PortabilityClass::StateReconstructionRequired: return 3;
        case PortabilityClass::RepackageAndRestartRequired: return 3;
        case PortabilityClass::Unsupported: return 255;
        case PortabilityClass::Unknown: return 255;
    }
    return 255;
}

bool implies_live_state_transfer(PortabilityClass value) noexcept {
    return value == PortabilityClass::LiveMigrationSupported;
}

bool requires_binary_transformation(PortabilityClass value) noexcept {
    switch (value) {
        case PortabilityClass::RecompileRequired:
        case PortabilityClass::RepackageRequired:
        case PortabilityClass::RepackageAndRestartRequired:
        case PortabilityClass::StateReconstructionRequired:
            return true;
        default:
            return false;
    }
}

bool requires_state_reconstruction(PortabilityClass value) noexcept {
    switch (value) {
        case PortabilityClass::StateReconstructionRequired:
        case PortabilityClass::RepackageAndRestartRequired:
            return true;
        default:
            return false;
    }
}

}  // namespace haf

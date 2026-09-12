// Heterogeneous Accelerator Federation - portability taxonomy.
//
// Federation membership does not imply that work can move. The portability
// class states exactly what must happen before a workload that is valid on
// accelerator A is valid on accelerator B.
//
//   NATIVE                       the same code object and the same execution
//                                environment are valid on both, with no change.
//   BINARY_COMPATIBLE            a code object already present on the
//                                destination is directly loadable.
//   RECOMPILE_REQUIRED           source/kernel must be rebuilt for the
//                                destination ISA.
//   REPACKAGE_REQUIRED           a valid code object exists but the runtime
//                                packaging/launch contract must be rebuilt.
//   STATE_RECONSTRUCTION_REQUIRED live state cannot move; the workload must be
//                                reconstructed from inputs or checkpoints by
//                                workload-owned logic.
//   CHECKPOINT_RESTORE_SUPPORTED  execution state can be exported and restored,
//                                but not while the source keeps running.
//   LIVE_MIGRATION_SUPPORTED      execution state can be transferred while the
//                                source is live, with a single authoritative
//                                execution at all times.
//   UNSUPPORTED                  no known path exists.
//   UNKNOWN                      evidence is missing; treated as failure for
//                                any concrete portability requirement.
//
// The strength order below is used ONLY for threshold comparison against a
// workload's minimum acceptable portability. UNKNOWN is deliberately placed
// just above UNSUPPORTED so that it never satisfies a concrete threshold.

#ifndef HAF_MODEL_PORTABILITY_HPP
#define HAF_MODEL_PORTABILITY_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "haf/core/status.hpp"

namespace haf {

enum class PortabilityClass : std::uint8_t {
    Unsupported = 0,
    Unknown = 1,
    RepackageAndRestartRequired = 2,
    StateReconstructionRequired = 3,
    RepackageRequired = 4,
    RecompileRequired = 5,
    CheckpointRestoreSupported = 6,
    BinaryCompatible = 7,
    Native = 8,
    LiveMigrationSupported = 9,
};

[[nodiscard]] std::string_view to_string(PortabilityClass value) noexcept;
[[nodiscard]] bool portability_class_from_token(std::string_view token, PortabilityClass& out) noexcept;
[[nodiscard]] bool portability_class_from_wire(std::uint8_t raw, PortabilityClass& out) noexcept;

/// Monotonic strength used for threshold comparison. Higher is stronger.
[[nodiscard]] std::uint8_t portability_strength(PortabilityClass value) noexcept;

/// True when \p value satisfies the minimum acceptable portability.
[[nodiscard]] bool meets_portability_threshold(PortabilityClass value, PortabilityClass minimum) noexcept;

/// Number of reconstruction/transformation steps implied by the class. Used by
/// deterministic ranking; lower is better.
[[nodiscard]] std::uint8_t portability_transformation_steps(PortabilityClass value) noexcept;

/// True when live state transfer is genuinely claimed. Cross-vendor claims are
/// rejected elsewhere; this predicate only reports what the class says.
[[nodiscard]] bool implies_live_state_transfer(PortabilityClass value) noexcept;

/// True when the class requires the binary to change before execution.
[[nodiscard]] bool requires_binary_transformation(PortabilityClass value) noexcept;

/// True when the class requires the workload's own state to be rebuilt.
[[nodiscard]] bool requires_state_reconstruction(PortabilityClass value) noexcept;

}  // namespace haf

#endif  // HAF_MODEL_PORTABILITY_HPP

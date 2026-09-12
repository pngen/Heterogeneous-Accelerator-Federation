#include "haf/adapters/adapter.hpp"

namespace haf::adapters {

Result<AcceleratorAdapter::ProofResult> AcceleratorAdapter::prove(int device_index) const {
    static_cast<void>(device_index);
    return Status(ErrorCode::Unsupported, "adapter '" + name() + "' does not implement an execution proof");
}

}  // namespace haf::adapters

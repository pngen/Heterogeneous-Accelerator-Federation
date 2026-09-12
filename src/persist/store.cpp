#include "haf/persist/store.hpp"

#include <utility>

namespace haf {

Result<ByteBuffer> MemoryStore::read() const {
    if (!present_) {
        return Status(ErrorCode::NotFound, "memory store holds no payload");
    }
    return payload_;
}

VoidResult MemoryStore::write(const ByteBuffer& payload) {
    payload_ = payload;
    present_ = true;
    return VoidResult();
}

VoidResult MemoryStore::erase() {
    payload_.clear();
    present_ = false;
    return VoidResult();
}

std::string MemoryStore::location() const { return "memory://"; }

}  // namespace haf

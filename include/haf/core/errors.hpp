// Convenience constructors for the typed error domain.

#ifndef HAF_CORE_ERRORS_HPP
#define HAF_CORE_ERRORS_HPP

#include <string>
#include <string_view>

#include "haf/core/status.hpp"

namespace haf {

[[nodiscard]] inline Status make_error(ErrorCode code, std::string message) {
    return Status(code, std::move(message));
}

[[nodiscard]] inline Status invalid_argument(std::string_view what) {
    return Status(ErrorCode::InvalidArgument, std::string(what));
}

[[nodiscard]] inline Status malformed(std::string_view what) {
    return Status(ErrorCode::MalformedData, std::string(what));
}

[[nodiscard]] inline Status unsupported(std::string_view what) {
    return Status(ErrorCode::Unsupported, std::string(what));
}

[[nodiscard]] inline Status bounds_exceeded(std::string_view what) {
    return Status(ErrorCode::BoundsExceeded, std::string(what));
}

}  // namespace haf

#endif  // HAF_CORE_ERRORS_HPP

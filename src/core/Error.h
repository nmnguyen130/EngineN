#pragma once

#include "core/Types.h"
#include <source_location>
#include <string_view>

namespace EngineN {

struct EngineError {
    std::string_view     message = "";
    i32                  code = 0;
    std::source_location location = std::source_location::current();

    constexpr EngineError() = default;

    constexpr EngineError(
        std::string_view msg,
        i32 errCode = 0,
        std::source_location loc = std::source_location::current()) noexcept
        : message(msg), code(errCode), location(loc) {}
};

} // namespace EngineN

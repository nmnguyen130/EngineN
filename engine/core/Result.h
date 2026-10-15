#pragma once

#include "core/Types.h"

#include <expected>
#include <string_view>

namespace en {

struct Error {
    std::string_view message{};
    i32 code{0};
};

template <typename T = void, typename E = Error>
using Result = std::expected<T, E>;

[[nodiscard]] constexpr std::unexpected<Error> Err(
    std::string_view message,
    i32 code = 0) noexcept {
    return std::unexpected<Error>(Error{message, code});
}

template <typename E>
[[nodiscard]] constexpr std::unexpected<E> Err(E error) noexcept {
    return std::unexpected<E>(error);
}

} // namespace en

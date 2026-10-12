#pragma once

#include "core/Assert.h"
#include "core/Error.h"

#include <type_traits>
#include <utility>
#include <variant>

namespace EngineN {

template <typename E>
struct Unexpected {
    E error;
};

template <typename E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> Err(E&& e) {
    return {std::forward<E>(e)};
}

struct SuccessTag {};
inline constexpr SuccessTag Ok{};

/**
 * Result<T, E>: Lightweight discriminated union holding either a value T or an error E.
 * Zero dynamic allocations, zero C++ exceptions.
 */
template <typename T, typename E = EngineError>
class Result {
public:
    constexpr Result(const T& val) : m_storage(std::in_place_index<0>, val) {}
    constexpr Result(T&& val) noexcept : m_storage(std::in_place_index<0>, std::move(val)) {}

    constexpr Result(const Unexpected<E>& unexp) : m_storage(std::in_place_index<1>, unexp.error) {}
    constexpr Result(Unexpected<E>&& unexp) noexcept : m_storage(std::in_place_index<1>, std::move(unexp.error)) {}

    [[nodiscard]] constexpr bool HasValue() const noexcept { return m_storage.index() == 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return HasValue(); }

    [[nodiscard]] constexpr T& Value() & noexcept {
        ENGINE_ASSERT(HasValue(), "Bad Result::Value() access");
        return std::get<0>(m_storage);
    }
    [[nodiscard]] constexpr const T& Value() const & noexcept {
        ENGINE_ASSERT(HasValue(), "Bad Result::Value() access");
        return std::get<0>(m_storage);
    }
    [[nodiscard]] constexpr T&& Value() && noexcept {
        ENGINE_ASSERT(HasValue(), "Bad Result::Value() access");
        return std::get<0>(std::move(m_storage));
    }

    [[nodiscard]] constexpr T& operator*() & noexcept { return Value(); }
    [[nodiscard]] constexpr const T& operator*() const & noexcept { return Value(); }
    [[nodiscard]] constexpr T&& operator*() && noexcept { return std::move(*this).Value(); }
    [[nodiscard]] constexpr T* operator->() noexcept { return &Value(); }
    [[nodiscard]] constexpr const T* operator->() const noexcept { return &Value(); }

    [[nodiscard]] constexpr E& Error() & noexcept {
        ENGINE_ASSERT(!HasValue(), "Bad Result::Error() access");
        return std::get<1>(m_storage);
    }
    [[nodiscard]] constexpr const E& Error() const & noexcept {
        ENGINE_ASSERT(!HasValue(), "Bad Result::Error() access");
        return std::get<1>(m_storage);
    }
    [[nodiscard]] constexpr E&& Error() && noexcept {
        ENGINE_ASSERT(!HasValue(), "Bad Result::Error() access");
        return std::get<1>(std::move(m_storage));
    }

    template <typename U>
    [[nodiscard]] constexpr T ValueOr(U&& fallback) const& {
        return HasValue() ? Value() : static_cast<T>(std::forward<U>(fallback));
    }
    template <typename U>
    [[nodiscard]] constexpr T ValueOr(U&& fallback) && {
        return HasValue() ? std::move(*this).Value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, E> m_storage;
};

/**
 * Result<void, E>: Specialization for operations returning success or error without payload.
 */
template <typename E>
class Result<void, E> {
public:
    constexpr Result() noexcept = default;
    constexpr Result(SuccessTag) noexcept {}

    constexpr Result(const Unexpected<E>& unexp) : m_hasValue(false), m_error(unexp.error) {}
    constexpr Result(Unexpected<E>&& unexp) noexcept : m_hasValue(false), m_error(std::move(unexp.error)) {}

    [[nodiscard]] constexpr bool HasValue() const noexcept { return m_hasValue; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return m_hasValue; }

    constexpr void Value() const noexcept {
        ENGINE_ASSERT(m_hasValue, "Bad Result<void>::Value() access");
    }

    [[nodiscard]] constexpr E& Error() & noexcept {
        ENGINE_ASSERT(!m_hasValue, "Bad Result<void>::Error() access");
        return m_error;
    }
    [[nodiscard]] constexpr const E& Error() const & noexcept {
        ENGINE_ASSERT(!m_hasValue, "Bad Result<void>::Error() access");
        return m_error;
    }
    [[nodiscard]] constexpr E&& Error() && noexcept {
        ENGINE_ASSERT(!m_hasValue, "Bad Result<void>::Error() access");
        return std::move(m_error);
    }

private:
    bool m_hasValue{true};
    E    m_error{};
};

} // namespace EngineN

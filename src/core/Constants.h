#pragma once

#include "core/Types.h"
#include <numbers>

namespace EngineN {

// Math Constants
inline constexpr f32 kPi        = std::numbers::pi_v<f32>;
inline constexpr f32 kHalfPi    = kPi * 0.5f;
inline constexpr f32 kTwoPi     = kPi * 2.0f;
inline constexpr f32 kDegToRad  = kPi / 180.0f;
inline constexpr f32 kRadToDeg  = 180.0f / kPi;
inline constexpr f32 kEpsilon   = 1e-6f;

// Sentinels and Invalid IDs
inline constexpr u32   kInvalidEntityId = ~0u;
inline constexpr usize kInvalidIndex    = static_cast<usize>(~0ULL);

} // namespace EngineN

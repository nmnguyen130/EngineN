#pragma once

#include "game/core/VeilTypes.h"

namespace en::veil {

// Procedural in-memory '.enpak' package builder for Veil of the Silent.
// Stateless builder generating deterministic textures, atlases, rigs, maps, audio cues, and scenes.
// Invariant: BuildBootPackage writes a valid EnPakHeader + EnPakEntry table into outBuffer.
class VeilAssets final {
public:
    [[nodiscard]] static usize BuildBootPackage(std::span<u8> outBuffer) noexcept;
};

} // namespace en::veil

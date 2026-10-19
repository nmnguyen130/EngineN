#pragma once

#include "game/core/Types.h"

#include <array>
#include <cstddef>

namespace en::veil {

// Frame-data specification for combat animations, cancel windows, and resource gains.
// Static const lookup table; zero runtime memory allocation.
struct ActionFrameSpec {
    ActionId id;
    u16 startupTicks;
    u16 activeTicks;
    u16 recoveryTicks;
    u16 cancelWindowStartTick;
    f32 motionValue;
    f32 breakMultiplier;
    f32 waveEnergyGain;
    f32 harmonyGain;

    [[nodiscard]] constexpr u16 TotalTicks() const noexcept {
        return static_cast<u16>(startupTicks + activeTicks + recoveryTicks);
    }
};

inline constexpr std::array<ActionFrameSpec, static_cast<std::size_t>(ActionId::Count)> kActionTable{{
    {ActionId::Idle,             0,  1,  0,  0, 0.00f, 0.00f,  0.0f,  0.0f},
    {ActionId::LightChain1,      5,  4,  9,  8, 1.00f, 1.00f, 14.0f,  6.0f},
    {ActionId::LightChain2,      6,  4, 10,  9, 1.15f, 1.10f, 16.0f,  7.0f},
    {ActionId::LightChain3,      8,  5, 12, 12, 1.40f, 1.35f, 22.0f, 10.0f},
    {ActionId::Launcher,         7,  5, 11, 10, 1.25f, 1.30f, 18.0f,  8.0f},
    {ActionId::AirChain1,        4,  4,  8,  7, 1.05f, 1.00f, 14.0f,  6.0f},
    {ActionId::AirChain2,        5,  4,  9,  8, 1.20f, 1.15f, 18.0f,  8.0f},
    {ActionId::PlungeFinish,     6,  8, 14, 12, 1.65f, 1.60f, 24.0f, 12.0f},
    {ActionId::WeaveShiftStrike, 4,  5,  8,  7, 1.35f, 1.40f, 15.0f, 12.0f},
    {ActionId::CrescendoBurst,   3,  6,  9,  6, 1.85f, 1.75f,  0.0f, 20.0f},
    {ActionId::DefensiveParry,   2, 10, 12,  4, 0.00f, 0.50f, 10.0f, 15.0f},
    {ActionId::PhaseCounter,     1,  8,  8,  4, 2.20f, 3.50f, 50.0f, 50.0f},
    {ActionId::DualUnisonCast,   6, 12, 12, 16, 2.60f, 2.40f, 25.0f,  0.0f},
    {ActionId::TriadUnisonCast,  8, 18, 14, 24, 4.20f, 3.80f, 40.0f,  0.0f},
    {ActionId::SacredChordCast,  8, 24, 14, 30, 5.80f, 5.00f, 60.0f,  0.0f}
}};

[[nodiscard]] constexpr const ActionFrameSpec& GetActionSpec(ActionId id) noexcept {
    const auto idx = static_cast<std::size_t>(id);
    return kActionTable[idx < kActionTable.size() ? idx : 0];
}

} // namespace en::veil

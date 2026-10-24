#pragma once

#include "game/core/GameTypes.h"

namespace vots {

// Centralized Kaelen locomotion, Wall Slide/Jump, Double Jump, Dash, Parry, Nail Recoil & Hitstun tuning (1 wu = 1 m = 64 px, 120 Hz).
// Player collider (0.68 x 1.85 wu) and 96x96 Sprite2D quad (2.40 x 2.40 wu, boots at y=85/96 -> -0.925 wu) align 1:1 with floor tiles.
inline constexpr f32 kPlayerColliderWidthWu          = 0.68f;
inline constexpr f32 kPlayerColliderHeightWu         = 1.85f;
inline constexpr f32 kPlayerSpriteSizeWu             = 2.40f;
inline constexpr f32 kPlayerMaxGroundSpeedWu         = 7.6f;
inline constexpr f32 kPlayerGroundAccelWu            = 60.0f;
inline constexpr f32 kPlayerGroundBrakingWu          = 80.0f;
inline constexpr f32 kPlayerMaxAirSpeedWu            = 7.2f;
inline constexpr f32 kPlayerAirAccelWu               = 38.0f;
inline constexpr f32 kPlayerMaxFallSpeedWu           = 22.0f;
inline constexpr f32 kPlayerWallSlideSpeedWu         = 3.8f;  // Smooth downward glide speed when holding into a wall
inline constexpr f32 kPlayerWallJumpHorizontalWu     = 7.5f;  // Horizontal push away from wall on Wall Jump
inline constexpr f32 kPlayerJumpVelocityWu           = 13.6f; // Apex = 13.6^2 / (2*25.0) = 3.70 wu (clears 3.0 wu platform tiers cleanly)
inline constexpr f32 kPlayerDoubleJumpVelocityWu     = 12.2f; // Additional 2.98 wu air boost (~6.68 wu combined two-jump reach)
inline constexpr f32 kPlayerGravityWu                = 25.0f;
inline constexpr f32 kPlayerJumpCutMultiplier        = 0.55f;
inline constexpr u8  kPlayerCoyoteTicks              = 10U;
inline constexpr u8  kPlayerJumpBufferTicks          = 12U;

// Extended Dash tuning: 21.0 wu/s for 20 ticks (166 ms) = 3.50 wu direct dash + 0.48x exit momentum (~4.25 wu total span).
inline constexpr f32 kPlayerDashSpeedWu              = 21.0f;
inline constexpr u8  kPlayerDashDurationTicks        = 20U;
inline constexpr f32 kPlayerAirDashExitSpeedRatio    = 0.48f;
inline constexpr u8  kPlayerDashRecoveryLockoutTicks = 3U;
inline constexpr u16 kPlayerChainDashCooldownTicks   = 6U;
inline constexpr u16 kPlayerGroundDashCooldownTicks  = 14U;
inline constexpr u8  kMaxAirDashCharges              = 1U;
inline constexpr u8  kPlayerDefaultMaxDashCharges    = 2U;

// Combo & Attack Cancel Windows (120 Hz).
inline constexpr u8  kLightChainBufferTicks          = 18U;
inline constexpr u16 kLightComboGraceTicks           = 28U;
inline constexpr u8  kRecoveryCancelWindowTicks      = 8U;
inline constexpr u8  kLightDashParryCancelMinTicks   = 3U;

// Combat Feel: Attacker Nail Recoil, Hit Knockback Arc, Hurt Flash, I-Frame Blink & Micro-Flinch Escape.
inline constexpr u16 kPlayerHitProtectionTicks       = 42U;
inline constexpr u16 kPlayerHitstunTicks             = 18U;
inline constexpr u16 kPlayerHardFlinchTicks          = 4U;  // 33 ms micro-flinch; Dash/Parry/Jump cancel immediately
inline constexpr u16 kPlayerHurtFlashDurationTicks   = 12U; // Bright crimson-white flash & screen vignette on impact
inline constexpr f32 kPlayerHitKnockbackAwayVx       = 5.0f;
inline constexpr f32 kPlayerHitKnockbackUpVy         = 3.8f;
inline constexpr f32 kPlayerAttackHitRecoilVx        = 2.1f; // Hollow Knight-style backward nail recoil on sword hit
inline constexpr f32 kEnemyHitKnockbackVx            = 3.4f;
inline constexpr i32 kPlayerBaseMaxHp                = 100;
inline constexpr f32 kHuskMaxHpPenaltyMultiplier     = 0.75f;

// Parry Window & Generous Strike-Window Interception (120 Hz).
inline constexpr u16 kParryWindowTicks               = 16U;
inline constexpr u16 kParryStrikeInterceptLeadTicks  = 12U;
inline constexpr f32 kParryMeleeReachBonusWu         = 0.95f;
inline constexpr f32 kParryProjectileCatchRadiusWu   = 1.35f;

} // namespace vots

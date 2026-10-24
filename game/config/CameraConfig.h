#pragma once

#include "game/core/GameTypes.h"

namespace vots {

// 2D Metroidvania Room Camera Parameters (1280x720 virtual canvas, 1 wu = 1 m = 64 px).
// At 64 px/wu, the 1280x720 viewport spans 20.00 x 11.25 wu, framing a 12-tile room floor-to-ceiling
// with zero outside void and giving Kaelen (1.85 wu collider / 2.40 wu 96x96 sprite) crisp indie-anime presence.
inline constexpr f32 kCameraPixelsPerMeter       = 64.0f;
inline constexpr f32 kCameraWalkLookaheadWu      = 2.15f;
inline constexpr f32 kCameraDashLookaheadWu      = 3.25f;
inline constexpr f32 kCameraLookaheadDamping     = 8.0f;
inline constexpr f32 kCameraVerticalBiasWu       = 1.55f;
inline constexpr f32 kCameraAirRiseDeadbandWu    = 2.45f;
inline constexpr f32 kCameraAirFallDeadbandWu    = 1.15f;
inline constexpr u8  kCameraDeadzoneHalfWQ4      = 10U;
inline constexpr u8  kCameraDeadzoneHalfHQ4      = 8U;
inline constexpr u8  kCameraBossWeightQ8         = 72U;
inline constexpr f32 kCameraCombatThreatRangeWu  = 11.5f;

// Exact half-viewport extent (640px / 64ppm = 10.0 wu) so the camera stops flush with room outer walls.
inline constexpr f32 kCameraHalfViewportMarginX  = 640.0f / kCameraPixelsPerMeter;
inline constexpr f32 kCameraHalfViewportMarginY  = 5.4f;

// Baseline confiner bounds for a standard 36x12 wu room (Room 0: [-12..24] x [-1..11]).
inline constexpr f32 kRoomCamCenterMinX          = -12.0f + kCameraHalfViewportMarginX;
inline constexpr f32 kRoomCamCenterMaxX          = 24.0f - kCameraHalfViewportMarginX;
inline constexpr f32 kRoomCamCenterMinY          = 3.4f;
inline constexpr f32 kRoomCamCenterMaxY          = 5.6f;

} // namespace vots

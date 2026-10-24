#pragma once

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "framework/FrameworkTypes.h"
#include "game/core/GameTypes.h"

namespace vots {

inline constexpr u16 kSpriteSheetWidthPx  = 768U;
inline constexpr u16 kSpriteSheetHeightPx = 1056U;
inline constexpr u16 kSpriteSheetCols     = 8U;
inline constexpr u16 kSpriteSheetRows     = 11U;
inline constexpr u16 kPixelFontWidthPx    = 512U;
inline constexpr u16 kPixelFontHeightPx   = 192U;

// Holds pre-registered AssetBank and AudioSystem handles for Veil of the Silent runtime entities.
// Value type initialized once during application boot in persistent scope.
// Invariant: all handles reference valid slots in AssetBank so hot-path rendering and FSM steps never branch.
struct GameAssetHandles {
    en::TextureAssetHandle characterSheetTex{};
    en::TextureAssetHandle pixelFontTex{};
    en::AtlasHandle characterAtlas{};
    en::BlobHandle playerRigBlob{};
    en::BlobHandle enemyRigBlob{};
    en::BlobHandle bossRigBlob{};
    std::array<en::AssetId, kRoomCount> roomManifestIds{};
    std::array<en::SoundCueId, 10> soundCues{};
};

// Enumeration of game-specific states inside the cooked "ERIG" v1 player/enemy state machine tables.
enum class ActorFsmState : u8 {
    Idle         = 0,
    Run          = 1,
    JumpRise     = 2,
    Fall         = 3,
    Dash         = 4,
    Light1       = 5,
    Light2       = 6,
    Light3       = 7,
    Air1         = 8,
    Air2         = 9,
    HeavyWindup  = 10,
    HeavyActive  = 11,
    ParryStance  = 12,
    ParrySuccess = 13,
    UnisonCast   = 14,
    SacredCast   = 15,
    HurtStagger  = 16,
    BreakExposed = 17,
    StateCount   = 18,
};

enum class GameSoundCue : u8 {
    BladeSlash       = 0,
    HeavyImpact      = 1,
    ParryClang       = 2,
    PhaseCounterBell = 3,
    ReactionBurst    = 4,
    BreakShatter     = 5,
    UnisonPulse      = 6,
    SacredChordBoom  = 7,
    ResonatorHum     = 8,
    HuskRecover      = 9,
};

// Loads external RGBA8 spritesheets and pixel-art fonts from disk and commits cooked rigs, scene manifests, and audio clips.
class GameAssetPipeline {
public:
    [[nodiscard]] static en::Result<GameAssetHandles, en::FrameworkErrc> BuildAndCommitPersistentAssets(
        en::AssetBank& bank,
        en::AudioSystem& audio,
        u8 persistentScope) noexcept;
};

} // namespace vots

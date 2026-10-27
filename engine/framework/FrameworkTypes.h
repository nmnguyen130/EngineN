#pragma once

#include "assets/AssetTypes.h"
#include "core/Math.h"
#include "core/Memory.h"
#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "input/InputTypes.h"
#include "time/TimePipeline.h"
#include "world/WorldTypes.h"

#include <algorithm>
#include <array>
#include <span>

namespace en {

class World;
class PhysicsSystem;
class GameplayPipeline;
class AssetBank;
class AudioSystem;
class DebugSystem;
class RenderList;
class Renderer;
class Platform;
class Window;
class SceneDirector;

inline constexpr u8 kInvalidStateId = 0xFFU;
inline constexpr u32 kMaxStateStackDepth = 8U;
inline constexpr u32 kMaxPendingStateCmds = 4U;

// Category bit on Tag2D::categoryMask marking an entity as room-transient (destroyed on SceneTransitionMode::RoomSwap).
inline constexpr u32 kRoomTransientCategoryBit = 0x40000000U;

enum class FrameworkErrc : u8 {
    None = 0,
    InvalidConfig,
    PlatformInitFailed,
    WindowCreateFailed,
    RendererInitFailed,
    AssetInitFailed,
    AudioInitFailed,
    WorldInitFailed,
    PhysicsInitFailed,
    GameplayInitFailed,
    DebugInitFailed,
    ClientBootFailed,
    StateStackOverflow,
    StateStackUnderflow,
    SceneTransitionBusy,
    ScopeAllocationFailed,
    CorruptedSceneManifest,
    SimAssetWaitFailed,
    ReplayDesyncOrExhausted,
};

enum class SubSystemStage : u8 {
    None          = 0,
    Platform      = 1,
    Window        = 2,
    Renderer      = 3,
    Assets        = 4,
    Audio         = 5,
    World         = 6,
    Physics       = 7,
    Gameplay      = 8,
    Debug         = 9,
    SceneDirector = 10,
    ClientApp     = 11,
};

enum class ReplayMode : u8 {
    None     = 0,
    Record   = 1,
    Playback = 2,
};

struct alignas(4) FrameworkError {
    FrameworkErrc code{FrameworkErrc::None};
    SubSystemStage stage{SubSystemStage::None};
    u16 subSystemErrc{0U};

    [[nodiscard]] constexpr bool operator==(const FrameworkError&) const noexcept = default;
};
static_assert(sizeof(FrameworkError) == 4);

enum class StateLayerFlags : u8 {
    None               = 0U,
    BlockLowerSimTicks = 1U << 0U,
    BlockLowerInput    = 1U << 1U,
    BlockLowerRender   = 1U << 2U,
    PauseSimClock      = 1U << 3U,
};

consteval bool enable_bitmask_operators(StateLayerFlags) noexcept { return true; }

enum class StateCmdKind : u8 {
    Push         = 0,
    Pop          = 1,
    Switch       = 2,
    ClearAndPush = 3,
};

enum class SceneTransitionMode : u8 {
    FullReset = 0,
    RoomSwap  = 1,
};

enum class TransitionWipeKind : u8 {
    None              = 0,
    FadeToBlack       = 1,
    HorizontalCurtain = 2,
    VerticalCurtain   = 3,
    DiamondIris       = 4,
};

enum class TransitionPhase : u8 {
    Idle                 = 0,
    FadingOut            = 1,
    StreamingSceneAssets = 2,
    FadingIn             = 3,
};

struct alignas(16) CookedSceneManifestHeader {
    static constexpr u32 kMagic = 0x4E435345U; // "ESCN"
    static constexpr u16 kVersion = 1U;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 assetEntryCount{0U};
    u32 musicTrackId{0U};
    u32 checksum{0U};
};
static_assert(sizeof(CookedSceneManifestHeader) == 16);

struct alignas(16) CookedSceneAssetEntry {
    AssetId id{};
    u32 byteSizeHint{0U};
    AssetKind kind{AssetKind::Blob};
    AssetResidency residency{AssetResidency::Sim};
    u16 flags{0U};

    [[nodiscard]] constexpr u32 MixChecksum(u32 h) const noexcept {
        for (u32 b = 0U; b < 64U; b += 8U) {
            h = (h ^ static_cast<u32>((id.value >> b) & 0xFFU)) * 16777619U;
        }
        h = (h ^ static_cast<u32>(kind)) * 16777619U;
        return (h ^ static_cast<u32>(residency)) * 16777619U;
    }
};
static_assert(sizeof(CookedSceneAssetEntry) == 16);

[[nodiscard]] constexpr u32 ComputeSceneManifestChecksum(
    std::span<const CookedSceneAssetEntry> entries) noexcept {
    u32 h = 2166136261U;
    for (const CookedSceneAssetEntry& e : entries) {
        h = e.MixChecksum(h);
    }
    return (h == 0U) ? 1U : h;
}

struct alignas(64) SimContext {
    World* world{nullptr};
    PhysicsSystem* physics{nullptr};
    GameplayPipeline* gameplay{nullptr};
    AssetBank* assetBank{nullptr};
    AudioSystem* audio{nullptr};
    ActionBuffer* actionBuffer{nullptr};
    const TickInput* tickInput{nullptr};
    SceneDirector* director{nullptr};
};
static_assert(sizeof(SimContext) == 64);

struct alignas(64) RenderContext {
    const World* world{nullptr};
    const PhysicsSystem* physics{nullptr};
    const GameplayPipeline* gameplay{nullptr};
    const AssetBank* assetBank{nullptr};
    RenderList* renderList{nullptr};
    const FrameInput* frameInput{nullptr};
    const RenderTimeView* timeView{nullptr};
    SceneDirector* director{nullptr};
};
static_assert(sizeof(RenderContext) == 64);

struct alignas(64) SceneLoadContext {
    World* world{nullptr};
    PhysicsSystem* physics{nullptr};
    GameplayPipeline* gameplay{nullptr};
    AssetBank* assetBank{nullptr};
    AudioSystem* audio{nullptr};
    SceneDirector* director{nullptr};
    void* userAppState{nullptr};
    u32 frameIndex{0U};
    u16 transitionTicks{0U};
    u8 sceneScope{0U};
    u8 persistentScope{0U};
};
static_assert(sizeof(SceneLoadContext) == 64);

struct alignas(16) StateLayerDesc {
    void (*onSimTick)(SimContext& ctx, const SimStepView& step, void* stateData) noexcept{nullptr};
    void (*onRenderExtract)(RenderContext& ctx, void* stateData) noexcept{nullptr};
    void (*onEnter)(SceneLoadContext& ctx, void* stateData) noexcept{nullptr};
    void (*onExit)(SceneLoadContext& ctx, void* stateData) noexcept{nullptr};
    void* stateData{nullptr};
    u16 inputActionMask{0xFFFFU};
    u8 id{kInvalidStateId};
    StateLayerFlags flags{StateLayerFlags::None};
    StateCmdKind cmdKind{StateCmdKind::Push};
    u8 layerPriority{0U};
    u16 userTag{0U};
};
static_assert(sizeof(StateLayerDesc) == 48);

struct alignas(64) SceneTransitionDesc {
    AssetId sceneId{};
    AssetId manifestBlobId{};
    AssetId collisionMapId{};
    AssetId rootPrefabId{};
    AssetId cueBankId{};
    Result<void, FrameworkErrc> (*onActivated)(SceneLoadContext& ctx) noexcept{nullptr};
    void (*onDeactivating)(SceneLoadContext& ctx) noexcept{nullptr};
    u32 musicTrackId{0U};
    u8 musicCrossfadeBlocks{48U};
    u8 transitionDurationTicks{12U};
    SceneTransitionMode mode{SceneTransitionMode::FullReset};
    TransitionWipeKind wipe{TransitionWipeKind::FadeToBlack};
};
static_assert(sizeof(SceneTransitionDesc) == 64);

struct alignas(64) FrameAuditDigest {
    u64 tickIndex{0ULL};
    u64 worldHash{0ULL};
    u64 physicsHash{0ULL};
    u64 gameplayHash{0ULL};
    u64 audioHash{0ULL};
    u64 renderListHash{0ULL};
    u64 directorHash{0ULL};
    u64 combinedHash{0ULL};

    [[nodiscard]] constexpr bool operator==(const FrameAuditDigest&) const noexcept = default;
};
static_assert(sizeof(FrameAuditDigest) == 64);

struct alignas(64) AppHooks {
    void* userAppState{nullptr};
    GameplayPolicyTable gameplayPolicies{};
    Result<void, FrameworkErrc> (*onRegisterComponents)(World& world, void* userAppState) noexcept{nullptr};
    Result<void, FrameworkErrc> (*onBoot)(SceneLoadContext& ctx) noexcept{nullptr};
    void (*onPreShutdown)(SceneLoadContext& ctx) noexcept{nullptr};
    void (*onConsumeSimCue)(const SimCue& cue, SimContext& ctx, void* userAppState) noexcept{nullptr};
};
static_assert(sizeof(AppHooks) == 64);

} // namespace en

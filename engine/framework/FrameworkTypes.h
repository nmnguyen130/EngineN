#pragma once

#include "assets/AssetTypes.h"
#include "core/Result.h"
#include "core/Types.h"
#include "core/time/TimePipeline.h"
#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "input/InputTypes.h"
#include "platform/Platform.h"
#include "world/WorldTypes.h"

namespace en::assets {
class AssetBank;
class AssetSystem;
} // namespace en::assets

namespace en::audio {
class AudioSystem;
} // namespace en::audio

namespace en::debug {
class DebugSystem;
} // namespace en::debug

namespace en::gameplay {
class GameplayPipeline;
} // namespace en::gameplay

namespace en::input {
class InputPipeline;
} // namespace en::input

namespace en::renderer {
class RenderList;
class Renderer;
} // namespace en::renderer

namespace en::world {
class World;
} // namespace en::world

namespace en::framework {

class SceneDirector;

inline constexpr usize kMaxStateStackDepth = 8;
inline constexpr usize kMaxPendingStateCmds = 8;
inline constexpr u32 kSceneManifestMagic = 0x4E435345u; // 'ESCN'
inline constexpr u16 kSceneManifestVersion = 1;

enum class FrameworkErrc : u8 {
    None = 0,
    InvalidConfig,
    PlatformInitFailed,
    WindowCreateFailed,
    RendererInitFailed,
    AssetInitFailed,
    AudioInitFailed,
    WorldInitFailed,
    GameplayInitFailed,
    DebugInitFailed,
    ClientBootFailed,
    StateStackOverflow,
    StateStackUnderflow,
    SceneTransitionBusy,
    ScopeAllocationFailed,
    CorruptedSceneManifest,
    SimAssetWaitFailed
};

enum class SubSystemStage : u8 {
    None = 0,
    Platform,
    Window,
    Renderer,
    Assets,
    Audio,
    World,
    Gameplay,
    Debug,
    SceneDirector,
    ClientApp
};

// Compact 4-byte error payload returned by Application lifecycle APIs.
struct alignas(4) FrameworkError {
    FrameworkErrc code{FrameworkErrc::None};
    SubSystemStage stage{SubSystemStage::None};
    u8 subSystemErrc{0};
    u8 reserved{0};
};
static_assert(sizeof(FrameworkError) == 4);

enum class StateLayerFlags : u8 {
    None               = 0,
    BlockLowerSimTicks = 1u << 0,
    BlockLowerInput    = 1u << 1,
    BlockLowerRender   = 1u << 2,
    PauseSimClock      = 1u << 3
};

[[nodiscard]] constexpr StateLayerFlags operator|(StateLayerFlags a, StateLayerFlags b) noexcept {
    return static_cast<StateLayerFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}

[[nodiscard]] constexpr StateLayerFlags operator&(StateLayerFlags a, StateLayerFlags b) noexcept {
    return static_cast<StateLayerFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}

[[nodiscard]] constexpr bool HasStateFlag(StateLayerFlags val, StateLayerFlags mask) noexcept {
    return (static_cast<u8>(val) & static_cast<u8>(mask)) != 0;
}

using StateId = u16;
inline constexpr StateId kInvalidStateId = 0xFFFF;

// Non-owning view passed to client state callbacks during 120 Hz fixed simulation ticks.
// Ownership: Borrowed subsystem references; valid strictly during OnSimTick.
// Invariant: step is active and all Residency::Sim blobs in the active scene scope are committed.
struct alignas(64) SimContext {
    world::World& world;
    gameplay::GameplayPipeline& gameplay;
    const assets::AssetBank& assetBank;
    audio::AudioSystem& audio;
    platform::Platform& platform;
    input::ActionBuffer& actionBuffer;
    const input::TickInput* tickInput{nullptr};
    void* userState{nullptr};
    time::SimStepView step{};
    SceneDirector* director{nullptr};
};
static_assert(sizeof(SimContext) == 128);

// Non-owning view passed to client state callbacks during presentation extraction.
// Ownership: Borrowed subsystem references; valid strictly during OnRenderExtract.
// Invariant: renderList is open between Renderer::BeginFrame() and Renderer::Submit().
struct alignas(64) RenderContext {
    const world::World& world;
    const gameplay::GameplayPipeline& gameplay;
    const assets::AssetBank& assetBank;
    renderer::RenderList& renderList;
    const input::FrameInput& frameInput;
    const platform::WindowState& windowState;
    void* userState{nullptr};
    SceneDirector* director{nullptr};
    time::RenderTimeView timeView{};
};
static_assert(sizeof(RenderContext) == 128);

// Non-owning view passed to state/scene load, activation, and teardown callbacks.
// Ownership: Borrowed subsystem references; valid during safe frame mutation points and boot/shutdown.
// Invariant: Fits in a single 64-byte L1 cache line with zero padding.
struct alignas(64) SceneLoadContext {
    world::World& world;
    gameplay::GameplayPipeline& gameplay;
    assets::AssetSystem& assets;
    audio::AudioSystem& audio;
    renderer::Renderer* renderer{nullptr};
    void* userAppState{nullptr};
    u32 frameIndex{0};
    assets::ScopeId sceneScope{assets::kInvalidScope};
    assets::ScopeId persistentScope{assets::kInvalidScope};
    u16 reserved{0};
    SceneDirector* director{nullptr};
};
static_assert(sizeof(SceneLoadContext) == 64);

using StateLifecycleFn = void (*)(SceneLoadContext& ctx, void* stateData) noexcept;
using StateSimTickFn   = void (*)(SimContext& ctx) noexcept;
using StateRenderFn    = void (*)(RenderContext& ctx) noexcept;

// Zero-padding 48-byte descriptor for pushing or switching a state stack layer.
struct alignas(16) StateLayerDesc {
    StateSimTickFn onSimTick{nullptr};
    StateRenderFn onRenderExtract{nullptr};
    StateLifecycleFn onEnter{nullptr};
    StateLifecycleFn onExit{nullptr};
    void* stateData{nullptr};
    StateId id{kInvalidStateId};
    u16 inputActionMask{0xFFFF};
    StateLayerFlags flags{StateLayerFlags::None};
    u8 cmdKind{0};
    u16 reserved{0};
};
static_assert(sizeof(StateLayerDesc) == 48);

enum class SceneTransitionMode : u8 {
    FullReset = 0,
    RoomSwap  = 1
};

enum class SceneWaitPolicy : u8 {
    StreamNonBlocking = 0,
    ImmediateBarrier  = 1
};

// Cooked binary scene manifest header ('ESCN', Residency::Sim BlobRecord).
struct alignas(16) CookedSceneManifestHeader {
    u32 magic{kSceneManifestMagic};
    u16 version{kSceneManifestVersion};
    u16 assetCount{0};
    assets::AssetId collisionMapId{};
    assets::AssetId rootPrefabId{};
    assets::AssetId cueBankId{};
    u32 musicTrackId{0};
    u8 musicCrossfadeBlocks{94};
    u8 reserved[3]{0, 0, 0};
};
static_assert(sizeof(CookedSceneManifestHeader) == 48);

struct alignas(16) CookedSceneAssetEntry {
    assets::AssetId id{};
    assets::AssetKind kind{assets::AssetKind::None};
    assets::Residency residency{assets::Residency::Presentation};
    u16 reserved0{0};
    u32 reserved1{0};
};
static_assert(sizeof(CookedSceneAssetEntry) == 16);

using SceneActivateFn   = bool (*)(SceneLoadContext& ctx, assets::AssetId sceneId) noexcept;
using SceneDeactivateFn = void (*)(SceneLoadContext& ctx, assets::AssetId sceneId) noexcept;

// Zero-padding 64-byte self-contained scene transition request descriptor (1 cache line).
struct alignas(64) SceneTransitionDesc {
    assets::AssetId sceneId{};
    assets::AssetId manifestBlobId{};
    assets::AssetId collisionMapId{};
    assets::AssetId rootPrefabId{};
    assets::AssetId cueBankId{};
    SceneActivateFn onActivated{nullptr};
    SceneDeactivateFn onDeactivating{nullptr};
    u32 musicTrackId{0};
    u8 musicCrossfadeBlocks{94};
    SceneTransitionMode mode{SceneTransitionMode::FullReset};
    SceneWaitPolicy waitPolicy{SceneWaitPolicy::StreamNonBlocking};
    u8 reserved{0};
};
static_assert(sizeof(SceneTransitionDesc) == 64);

// Single cache-line deterministic audit digest across all EngineN subsystems.
struct alignas(64) FrameAuditDigest {
    u64 tickIndex{0};
    u64 worldHash{0};
    u64 gameplayHash{0};
    u64 assetBankHash{0};
    u64 audioHash{0};
    u64 renderListHash{0};
    u64 directorHash{0};
    u64 combinedHash{0};
};
static_assert(sizeof(FrameAuditDigest) == 64);

struct alignas(64) AppHooks {
    using RegisterComponentsFn = Result<void, u8> (*)(world::World& world, void* appState) noexcept;
    using BootFn = Result<void, u8> (*)(SceneLoadContext& ctx, SceneDirector& director) noexcept;
    using PreShutdownFn = void (*)(SceneLoadContext& ctx) noexcept;
    using ConsumeSimCueFn = void (*)(const gameplay::SimCue& cue, world::World& world,
                                     const assets::AssetBank& bank, void* appState) noexcept;

    void* userAppState{nullptr};
    platform::EventTap devToolsTap{};
    gameplay::GameplayPolicyTable gameplayPolicies{};
    RegisterComponentsFn OnRegisterComponents{nullptr};
    BootFn OnBoot{nullptr};
    PreShutdownFn OnPreShutdown{nullptr};
    ConsumeSimCueFn OnConsumeSimCue{nullptr};
};

} // namespace en::framework

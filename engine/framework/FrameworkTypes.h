#pragma once

#include "assets/AssetSystem.h"
#include "audio/AudioSystem.h"
#include "core/Result.h"
#include "core/Types.h"
#include "core/time/SdlClock.h"
#include "gameplay/GameplayPipeline.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "input/ActionBuffer.h"
#include "input/InputPipeline.h"
#include "renderer/Renderer.h"
#include "world/World.h"

#include <array>
#include <span>
#include <string_view>

namespace en::framework {

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
};

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
    time::RenderTimeView timeView{};
};

// Non-owning view passed to state/scene load, activation, and teardown callbacks.
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
    u64 userPadding{0};
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
    u8 reserved0{0};
    u16 reserved1{0};
};
static_assert(sizeof(StateLayerDesc) == 48);

enum class SceneTransitionMode : u8 {
    FullReset = 0, // Invokes World::Clear(), binds new collision map, instantiates root prefab.
    RoomSwap  = 1  // Preserves surviving entities, swaps collision blob & retires old asset scope.
};

enum class SceneWaitPolicy : u8 {
    StreamNonBlocking = 0, // Streams across frames while current state/loading overlay renders; activates when ready.
    ImmediateBarrier  = 1  // Blocks inside CommitSafeFramePoint via AssetSystem::WaitSimReady on next frame.
};

// Cooked binary scene manifest header ('ESCN', Residency::Sim BlobRecord).
// Eliminates heap vectors and dangling spans during asynchronous scene transitions.
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
    assets::AssetId manifestBlobId{};   // Optional 'ESCN' blob; if 0, uses inline fields below.
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
    void* userAppState{nullptr};
    platform::EventTap devToolsTap{};
    gameplay::GameplayPolicyTable gameplayPolicies{};

    Result<void, u8> (*OnRegisterComponents)(world::World& world, void* appState) noexcept = nullptr;
    Result<void, u8> (*OnBoot)(SceneLoadContext& ctx, class SceneDirector& director) noexcept = nullptr;
    void (*OnPreShutdown)(SceneLoadContext& ctx) noexcept = nullptr;
    void (*OnConsumeSimCue)(const gameplay::SimCue& cue, world::World& world,
                            const assets::AssetBank& bank, void* appState) noexcept = nullptr;
};

struct ApplicationConfig {
    std::string_view appName{"EngineN"};
    std::string_view orgName{"EngineN"};
    std::string_view bootPakPath{"base.enpak"};
    bool headless{false};
    assets::PumpBudget assetPumpBudget{};
    platform::WindowConfig window{};
    renderer::RendererConfig renderer{};
    assets::AssetSystemConfig assets{};
    audio::AudioSystemConfig audio{};
    world::WorldConfig world{};
    gameplay::GameplayPipelineConfig gameplay{};
};

} // namespace en::framework
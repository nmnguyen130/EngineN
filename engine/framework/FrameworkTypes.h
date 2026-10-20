#pragma once

#include "assets/AssetSystem.h"
#include "audio/AudioSystem.h"
#include "debug/DebugProfilerAndConsole.h"
#include "gameplay/GameplayPipeline.h"
#include "input/InputPipeline.h"
#include "platform/Platform.h"
#include "renderer/Renderer2D.h"

namespace en {

using StateId = u16;
inline constexpr StateId kInvalidStateId = 0xFFFFU;
inline constexpr usize kMaxStateStackDepth = 8;

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
    CorruptedSceneManifest,
    SaveGameCorrupted,
};

enum class StateLayerFlags : u8 {
    None = 0,
    BlockLowerSimTicks = 1U << 0,
    BlockLowerInput = 1U << 1,
    BlockLowerRender = 1U << 2,
    PauseSimClock = 1U << 3,
};
consteval bool enable_bitmask_operators(StateLayerFlags) { return true; }

enum class SceneTransitionMode : u8 {
    FullReset = 0,
    RoomSwap = 1,
};

class SceneDirector;

struct alignas(64) SceneLoadContext {
    World& world;
    GameplayPipeline& gameplay;
    AssetSystem& assets;
    AudioEngine& audio;
    Renderer2D& renderer;
    void* userAppState{nullptr};
    SceneDirector* director{nullptr};
    ScopeId sceneScope{0};
    ScopeId persistentScope{0};
    u16 transitionFlags{0};
    u32 userTag{0};
};
static_assert(sizeof(SceneLoadContext) == 64);

struct alignas(32) SimContext {
    World& world;
    GameplayPipeline& gameplay;
    const AssetBank& assetBank;
    AudioEngine& audio;
    Platform& platform;
    ActionBuffer& actionBuffer;
    const TickInput* tickInput{nullptr};
    void* userState{nullptr};
    SceneDirector* director{nullptr};
    SimStepView step{};
};
static_assert(sizeof(SimContext) == 128);

struct alignas(64) RenderContext {
    const World& world;
    const GameplayPipeline& gameplay;
    const AssetBank& assetBank;
    RenderList& renderList;
    const FrameInput& frameInput;
    const WindowState& windowState;
    void* userState{nullptr};
    SceneDirector* director{nullptr};
    RenderTimeView timeView{};
};
static_assert(sizeof(RenderContext) == 128);

struct alignas(16) StateLayerDesc {
    void (*onSimTick)(SimContext& ctx) noexcept {nullptr};
    void (*onRenderExtract)(RenderContext& ctx) noexcept {nullptr};
    void (*onEnter)(SceneLoadContext& ctx, void* stateData) noexcept {nullptr};
    void (*onExit)(SceneLoadContext& ctx, void* stateData) noexcept {nullptr};
    void* stateData{nullptr};
    StateId id{0};
    u16 inputActionMask{0xFFFFU};
    StateLayerFlags flags{StateLayerFlags::None};
    u8 priority{0};
    u16 layerTag{0};
};
static_assert(sizeof(StateLayerDesc) == 48);

struct SceneTransitionDesc {
    AssetId sceneId{};
    AssetId manifestBlobId{};
    AssetId collisionMapId{};
    AssetId rootPrefabId{};
    AssetId cueBankId{};
    void (*onActivated)(SceneLoadContext& ctx) noexcept {nullptr};
    void (*onDeactivating)(SceneLoadContext& ctx) noexcept {nullptr};
    SceneTransitionMode mode{SceneTransitionMode::FullReset};
};

inline constexpr u32 kCookedSceneManifestMagic = 0x4E435345U; // 'ESCN'

struct alignas(16) CookedSceneManifestHeader {
    u32 magic{kCookedSceneManifestMagic};
    u16 version{1};
    u16 assetCount{0};
    u64 collisionMapId{0};
    u64 rootPrefabId{0};
    u64 cueBankId{0};
    u64 checksumCrc64{0};
};
static_assert(sizeof(CookedSceneManifestHeader) == 48);

struct alignas(16) CookedSceneAssetEntry {
    u64 assetId{0};
    AssetKind kind{AssetKind::Blob};
    AssetResidency residency{AssetResidency::Sim};
    u16 version{1};
    u32 flags{0};
};
static_assert(sizeof(CookedSceneAssetEntry) == 16);

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

struct AppHooks {
    void* userAppState{nullptr};
    GameplayPolicyTable gameplayPolicies{};
    Result<void, WorldErrc> (*onRegisterComponents)(World& world) noexcept {nullptr};
    Result<void, FrameworkErrc> (*onBoot)(SceneLoadContext& ctx) noexcept {nullptr};
    void (*onPreShutdown)(SceneLoadContext& ctx) noexcept {nullptr};
    void (*onConsumeSimCue)(const SimCue& cue, SimContext& ctx) noexcept {nullptr};
};

inline constexpr u32 kSaveSlotMagic = 0x56415345U; // 'ESAV'

struct alignas(16) SaveSlotHeader {
    u32 magic{kSaveSlotMagic};
    u32 version{1};
    u32 payloadCrc32{0};
    u32 checkpointRoomId{0};
    Vec2 spawnPos{};
    i16 playerHp{100};
    i16 playerMaxHp{100};
    u32 currency{0};
};
static_assert(sizeof(SaveSlotHeader) == 32);

} // namespace en

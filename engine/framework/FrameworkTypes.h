#pragma once

#include "assets/AssetTypes.h"
#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "input/InputTypes.h"
#include "time/TimeTypes.h"
#include "world/Components.h"

namespace en {

class World;
class PhysicsSystem;
class GameplaySystem;
class AssetBank;
class AudioSystem;
class RenderList;
class SceneDirector;

inline constexpr u8  kNoState      = 0xFFU;
inline constexpr u32 kMaxStates    = 4U;
inline constexpr u32 kMaxStateCmds = 4U;

enum class FwErrc : u8 {
    None = 0,
    InvalidConfig,
    PlatformFailed,
    WindowFailed,
    RendererFailed,
    AssetsFailed,
    AudioFailed,
    WorldFailed,
    PhysicsFailed,
    GameplayFailed,
    DebugFailed,
    ClientFailed,
    StackFull,
    StackEmpty,
    Busy,
    ScopeExhausted,
    InvalidRoom,
};

enum class StateFlags : u8 {
    None     = 0,
    PauseSim = 1U << 0,
    NoInput  = 1U << 1,
    NoDraw   = 1U << 2,
    Freeze   = 1U << 3,
};
consteval bool enable_bitmask_operators(StateFlags) noexcept { return true; }

enum class StateOp : u8 {
    Push  = 0,
    Pop   = 1,
    Swap  = 2,
    Reset = 3,
};

enum class WipeKind : u8 {
    None   = 0,
    Fade   = 1,
    HSlide = 2,
    VSlide = 3,
};

enum class TransitionPhase : u8 {
    Idle = 0,
    Out  = 1,
    Load = 2,
    In   = 3,
};

// 64-byte non-owning subsystem bundle passed to state simulation callbacks each 120 Hz tick.
struct SimCtx {
    World*          world{nullptr};
    PhysicsSystem*  physics{nullptr};
    GameplaySystem* gameplay{nullptr};
    AssetBank*      assets{nullptr};
    AudioSystem*    audio{nullptr};
    ActionBuffer*   actions{nullptr};
    const TickInput* input{nullptr};
    SceneDirector*  scenes{nullptr};
};
static_assert(sizeof(SimCtx) == 64);

// 64-byte non-owning subsystem bundle passed to state render callbacks each frame.
struct DrawCtx {
    const World*          world{nullptr};
    const PhysicsSystem*  physics{nullptr};
    const GameplaySystem* gameplay{nullptr};
    const AssetBank*      assets{nullptr};
    RenderList*           list{nullptr};
    const FrameInput*     input{nullptr};
    const RenderStep*     renderStep{nullptr};
    SceneDirector*        scenes{nullptr};
};
static_assert(sizeof(DrawCtx) == 64);

// 64-byte non-owning subsystem bundle passed to state and room lifecycle load/unload callbacks.
struct LoadCtx {
    World*          world{nullptr};
    PhysicsSystem*  physics{nullptr};
    GameplaySystem* gameplay{nullptr};
    AssetBank*      assets{nullptr};
    AudioSystem*    audio{nullptr};
    SceneDirector*  scenes{nullptr};
    void*           userData{nullptr};
    u32             frame{0};
    u8              roomScope{0xFF};
    u8              persistentScope{0xFF};
    u16             pad{0};
};
static_assert(sizeof(LoadCtx) == 64);

// 48-byte scene state descriptor for the 4-layer SceneDirector state stack.
struct SceneState {
    void (*onTick)(SimCtx&, const TickStep&, void*) noexcept{nullptr};
    void (*onDraw)(DrawCtx&, void*) noexcept{nullptr};
    void (*onEnter)(LoadCtx&, void*) noexcept{nullptr};
    void (*onExit)(LoadCtx&, void*) noexcept{nullptr};
    void*      userData{nullptr};
    u16        inputMask{0xFFFF};
    u8         id{kNoState};
    StateFlags flags{StateFlags::None};
    u32        pad{0};
};
static_assert(sizeof(SceneState) == 48);

// 56-byte Metroidvania room transition descriptor with collision map, prefab, music, and wipe settings.
struct RoomDesc {
    AssetId roomId{};
    AssetId collisionMap{};
    AssetId prefab{};
    AssetId musicTrack{};
    Result<void, FwErrc> (*onEnter)(LoadCtx&) noexcept{nullptr};
    void (*onExit)(LoadCtx&) noexcept{nullptr};
    u8       musicFadeBlocks{48};
    u8       wipeTicks{12};
    WipeKind wipe{WipeKind::Fade};
    bool     clearWorld{true};
    u32      pad{0};
};
static_assert(sizeof(RoomDesc) == 56);

// 64-byte application lifecycle and gameplay hook table.
struct AppHooks {
    void*             userData{nullptr};
    GameplayCallbacks gameplay{};
    Result<void, FwErrc> (*onRegisterComponents)(World&, void*) noexcept{nullptr};
    Result<void, FwErrc> (*onBoot)(LoadCtx&) noexcept{nullptr};
    void (*onShutdown)(LoadCtx&) noexcept{nullptr};
    void (*onCue)(const GameplayCue&, SimCtx&, void*) noexcept{nullptr};
};
static_assert(sizeof(AppHooks) == 64);

} // namespace en

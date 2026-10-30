#pragma once

#include "assets/AssetTypes.h"
#include "core/Types.h"
#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "input/InputTypes.h"
#include "time/TimeTypes.h"
#include "world/WorldTypes.h"

#include <array>

namespace en {

class World;
class PhysicsSystem;
class GameplayPipeline;
class AssetBank;
class AudioSystem;
class RenderList;
class SceneDirector;

inline constexpr u8  kNoState     = 0xFFU;
inline constexpr u32 kMaxStates   = 4U;
inline constexpr u32 kMaxStateCmd = 4U;
inline constexpr u32 kRoomTempBit = 0x40000000U;

enum class FwErr : u8 {
    Ok = 0,
    BadConfig,
    Platform,
    Window,
    Renderer,
    Assets,
    Audio,
    World,
    Physics,
    Gameplay,
    Debug,
    Boot,
    StackFull,
    StackEmpty,
    Busy,
    Scope,
    Manifest,
    Asset,
};

enum class FwStage : u8 {
    None = 0, Platform, Window, Renderer, Assets, Audio,
    World, Physics, Gameplay, Debug, Director, Client
};

struct alignas(4) FwError {
    FwErr   code{FwErr::Ok};
    FwStage stage{FwStage::None};
    u16     sub{0};
    [[nodiscard]] constexpr bool operator==(const FwError&) const noexcept = default;
};
static_assert(sizeof(FwError) == 4);

enum class StateFlag : u8 {
    None   = 0,
    NoSim  = 1 << 0,
    NoIn   = 1 << 1,
    NoDraw = 1 << 2,
    Freeze = 1 << 3,
};
consteval bool enable_bitmask_operators(StateFlag) noexcept { return true; }

enum class StateOp : u8 { Push, Pop, Swap, Reset };

enum class RoomMode : u8 { Reset, Swap };

enum class Wipe : u8 { None, Fade, HSlide, VSlide };

enum class Phase : u8 { Idle, Out, Load, In };

// Non-owning bundles filled by Application each frame / tick.
struct alignas(64) SimCtx {
    World*            world{};
    PhysicsSystem*    phys{};
    GameplayPipeline* game{};
    AssetBank*        assets{};
    AudioSystem*      audio{};
    ActionBuffer*     acts{};
    const TickInput*  input{};
    SceneDirector*    dir{};
};
static_assert(sizeof(SimCtx) == 64);

struct alignas(64) DrawCtx {
    const World*            world{};
    const PhysicsSystem*    phys{};
    const GameplayPipeline* game{};
    const AssetBank*        assets{};
    RenderList*             list{};
    const FrameInput*       frame{};
    const RenderStep*       time{};
    SceneDirector*          dir{};
};
static_assert(sizeof(DrawCtx) == 64);

struct alignas(64) LoadCtx {
    World*            world{};
    PhysicsSystem*    phys{};
    GameplayPipeline* game{};
    AssetBank*        assets{};
    AudioSystem*      audio{};
    SceneDirector*    dir{};
    void*             user{};
    u32               frame{0};
    u8                roomScope{0xFF};
    u8                keepScope{0xFF};
};
static_assert(sizeof(LoadCtx) == 64);

struct alignas(32) State {
    void (*tick)(SimCtx&, const SimStepView&, void*) noexcept{};
    void (*draw)(DrawCtx&, void*) noexcept{};
    void (*enter)(LoadCtx&, void*) noexcept{};
    void (*exit)(LoadCtx&, void*) noexcept{};
    void*     data{};
    u16       inMask{0xFFFF};
    u8        id{kNoState};
    StateFlag flags{StateFlag::None};
};
static_assert(sizeof(State) == 48);

struct alignas(64) Room {
    AssetId scene{};
    AssetId manifest{};
    AssetId collMap{};
    AssetId prefab{};
    AssetId cues{};
    Result<void, FwErr> (*enter)(LoadCtx&) noexcept{};
    void (*exit)(LoadCtx&) noexcept{};
    u32     music{0};
    u8      fadeBlocks{48};
    u8      wipeTicks{12};
    RoomMode mode{RoomMode::Reset};
    Wipe     wipe{Wipe::Fade};
};
static_assert(sizeof(Room) == 64);

struct alignas(64) Hooks {
    void* user{};
    GameplayPolicyTable policies{};
    Result<void, FwErr> (*registerComponents)(World&, void*) noexcept{};
    Result<void, FwErr> (*boot)(LoadCtx&) noexcept{};
    void (*preShutdown)(LoadCtx&) noexcept{};
    void (*cue)(const SimCue&, SimCtx&, void*) noexcept{};
};
static_assert(sizeof(Hooks) == 64);

} // namespace en

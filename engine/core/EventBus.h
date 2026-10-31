#pragma once

#include "audio/AudioTypes.h"
#include "world/Components.h"

namespace en {

// Fixed-capacity, zero-allocation typed event ring/span channel for decoupled subsystem communication.
// Value type; producers call Publish() and stage consumers call Read() or Drain() with zero heap allocations.
template <typename T, u32 Capacity>
    requires std::is_trivially_copyable_v<T>
class EventChannel {
public:
    static constexpr u32 kCapacity = Capacity;

    constexpr void Clear() noexcept { m_count = 0U; }

    constexpr bool Publish(const T& ev) noexcept {
        if (m_count >= Capacity) return false;
        m_items[m_count++] = ev;
        return true;
    }

    [[nodiscard]] constexpr std::span<const T> Read() const noexcept {
        return {m_items.data(), m_count};
    }

    [[nodiscard]] constexpr std::span<const T> Drain() noexcept {
        const u32 n = m_count;
        m_count     = 0U;
        return {m_items.data(), n};
    }

    [[nodiscard]] constexpr u32 Count() const noexcept { return m_count; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_count == 0U; }

private:
    std::array<T, Capacity> m_items{};
    u32                     m_count{0};
};

using AudioEvent = AudioTrigger;

// 24-byte particle burst event published to EventBus::vfxBursts.
struct VfxBurstEvent {
    Vec2  origin{};
    f32   magnitude{1.0f};
    u16   durationTicks{14};
    u16   count{6};
    Rgba8 color{255, 230, 175, 235};
    u32   pad{0};
};
static_assert(sizeof(VfxBurstEvent) == 24);

// 24-byte dash after-image ghost event published to EventBus::dashGhosts.
struct DashGhostEvent {
    Vec2     position{};
    EntityId emitter{kNullEntity};
    u16      durationTicks{8};
    u16      flags{0};
    u64      pad{0};
};
static_assert(sizeof(DashGhostEvent) == 24);

// 16-byte camera trauma shake event published to EventBus::cameraTrauma.
struct CameraTraumaEvent {
    Vec2 origin{};
    f32  trauma{0.0f};
    u32  pad{0};
};
static_assert(sizeof(CameraTraumaEvent) == 16);

// 8-byte time-channel hitstop freeze event published to EventBus::hitstops.
struct HitstopEvent {
    u16         durationTicks{0};
    TimeChannel channel{TimeChannel::World};
    u8          pad0{0};
    u32         pad1{0};
};
static_assert(sizeof(HitstopEvent) == 8);

// 8-byte gamepad rumble event published to EventBus::rumbles.
struct RumbleEvent {
    f32 intensity{0.0f};
    u16 durationTicks{0};
    u16 pad{0};
};
static_assert(sizeof(RumbleEvent) == 8);

// 40-byte room doorway transition event published to EventBus::roomTransitions.
struct RoomTransitionEvent {
    AssetId targetRoom{};
    AssetId collisionMap{};
    AssetId prefab{};
    Vec2    spawnPos{};
    u8      wipeTicks{12};
    u8      wipeKind{1};
    u8      clearWorld{1};
    u8      pad0{0};
    u32     pad1{0};
};
static_assert(sizeof(RoomTransitionEvent) == 40);

// Zero-allocation, type-safe deferred event bus decoupling gameplay, physics, VFX, audio, time, and scene subsystems.
// Value type owned by Application::Runtime; replaces monolithic switch-case routing in EnginePipeline.
struct EventBus {
    EventChannel<AudioTrigger, 64>        audio{};
    EventChannel<VfxBurstEvent, 64>       vfxBursts{};
    EventChannel<DashGhostEvent, 64>      dashGhosts{};
    EventChannel<CameraTraumaEvent, 32>   cameraTrauma{};
    EventChannel<HitstopEvent, 32>        hitstops{};
    EventChannel<RumbleEvent, 16>         rumbles{};
    EventChannel<RoomTransitionEvent, 8>  roomTransitions{};

    constexpr void ClearAll() noexcept {
        audio.Clear();
        vfxBursts.Clear();
        dashGhosts.Clear();
        cameraTrauma.Clear();
        hitstops.Clear();
        rumbles.Clear();
        roomTransitions.Clear();
    }
};

} // namespace en

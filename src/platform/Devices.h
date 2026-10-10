#pragma once

#include "platform/PlatformTypes.h"
#include <optional>

struct SDL_Gamepad;

namespace EngineN {

inline constexpr usize kMaxGamepads = 8;

struct GamepadSlot {
    bool occupied{false};
    GamepadId id{};
    SDL_Gamepad* handle{nullptr};
    FixedString<64> name{};
};

/**
 * GamepadRegistry: owns hot-plugged gamepad lifecycle and player slot assignment.
 * Up to 8 gamepads tracked in zero-allocation inline storage.
 */
class GamepadRegistry {
public:
    GamepadRegistry() noexcept = default;
    ~GamepadRegistry();

    GamepadRegistry(const GamepadRegistry&) = delete;
    GamepadRegistry& operator=(const GamepadRegistry&) = delete;

    [[nodiscard]] std::span<const GamepadSlot> Slots() const noexcept { return m_slots; }
    [[nodiscard]] u32 ConnectedCount() const noexcept { return m_connected; }
    [[nodiscard]] const GamepadSlot* FindById(GamepadId id) const noexcept;

    [[nodiscard]] Result<u8, EngineError> OnAdded(GamepadId id) noexcept;
    [[nodiscard]] std::optional<u8> OnRemoved(GamepadId id) noexcept;

    [[nodiscard]] Result<void, EngineError> Rumble(u8 slot, u16 low, u16 high, u32 durationMs) noexcept;
    void StopAllRumble() noexcept;
    void CloseAll() noexcept;

private:
    std::array<GamepadSlot, kMaxGamepads> m_slots{};
    u32 m_connected{0};
};

inline constexpr usize kMaxAudioDevices = 16;

struct AudioDeviceInfo {
    AudioDeviceId id{};
    FixedString<96> name{};
    bool isRecording{false};
};

/**
 * AudioDeviceDirectory: device tracking directory for available audio endpoints.
 * Operates purely as an inventory, independent of audio subsystem initialization.
 */
class AudioDeviceDirectory {
public:
    [[nodiscard]] Result<void, EngineError> Seed() noexcept;

    bool OnAdded(AudioDeviceId id, bool recording) noexcept;
    bool OnRemoved(AudioDeviceId id, bool recording) noexcept;

    [[nodiscard]] std::span<const AudioDeviceInfo> Playback() const noexcept { return m_playback.View(); }
    [[nodiscard]] std::span<const AudioDeviceInfo> Recording() const noexcept { return m_recording.View(); }
    [[nodiscard]] u32 Revision() const noexcept { return m_revision; }

private:
    FixedVector<AudioDeviceInfo, kMaxAudioDevices> m_playback{};
    FixedVector<AudioDeviceInfo, kMaxAudioDevices> m_recording{};
    u32 m_revision{0};
};

} // namespace EngineN

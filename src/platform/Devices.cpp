#include "platform/Devices.h"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_stdinc.h>

namespace EngineN {

GamepadRegistry::~GamepadRegistry() {
    CloseAll();
}

const GamepadSlot* GamepadRegistry::FindById(GamepadId id) const noexcept {
    for (const auto& slot : m_slots) {
        if (slot.occupied && slot.id == id) {
            return &slot;
        }
    }
    return nullptr;
}

Result<u8, EngineError> GamepadRegistry::OnAdded(GamepadId id) noexcept {
    for (u8 i = 0; i < static_cast<u8>(kMaxGamepads); ++i) {
        if (m_slots[i].occupied && m_slots[i].id == id) {
            return i;
        }
    }

    u8 freeSlot = 0xFF;
    for (u8 i = 0; i < static_cast<u8>(kMaxGamepads); ++i) {
        if (!m_slots[i].occupied) {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot == 0xFF) {
        return Unexpected(MakePlatformError(PlatformErrc::CapacityExceeded, "All gamepad slots are occupied"));
    }

    SDL_Gamepad* handle = SDL_OpenGamepad(id.value);
    if (!handle) {
        return Unexpected(MakePlatformError(PlatformErrc::DeviceOpenFailed));
    }

    GamepadSlot slot{};
    slot.occupied = true;
    slot.id = id;
    slot.handle = handle;

    const char* name = SDL_GetGamepadName(handle);
    if (name) {
        slot.name.Assign(name);
    }

    m_slots[freeSlot] = slot;
    ++m_connected;

    return freeSlot;
}

std::optional<u8> GamepadRegistry::OnRemoved(GamepadId id) noexcept {
    for (u8 i = 0; i < static_cast<u8>(kMaxGamepads); ++i) {
        if (m_slots[i].occupied && m_slots[i].id == id) {
            if (m_slots[i].handle) {
                SDL_CloseGamepad(m_slots[i].handle);
            }
            m_slots[i] = GamepadSlot{};
            if (m_connected > 0) {
                --m_connected;
            }
            return i;
        }
    }
    return std::nullopt;
}

Result<void, EngineError> GamepadRegistry::Rumble(
    u8 slot,
    u16 low,
    u16 high,
    u32 durationMs) noexcept
{
    if (slot >= kMaxGamepads || !m_slots[slot].occupied || !m_slots[slot].handle) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState, "Slot not occupied or handle is null"));
    }

    if (!SDL_RumbleGamepad(m_slots[slot].handle, low, high, durationMs)) {
        return Unexpected(MakePlatformError(PlatformErrc::DeviceOpenFailed, "Gamepad rumble failed"));
    }
    return Ok;
}

void GamepadRegistry::StopAllRumble() noexcept {
    for (auto& slot : m_slots) {
        if (slot.occupied && slot.handle) {
            (void)SDL_RumbleGamepad(slot.handle, 0, 0, 0);
        }
    }
}

void GamepadRegistry::CloseAll() noexcept {
    for (auto& slot : m_slots) {
        if (slot.occupied && slot.handle) {
            SDL_CloseGamepad(slot.handle);
        }
        slot = GamepadSlot{};
    }
    m_connected = 0;
}

Result<void, EngineError> AudioDeviceDirectory::Seed() noexcept {
    m_playback.Clear();
    m_recording.Clear();

    int playbackCount = 0;
    SDL_AudioDeviceID* playbackIds = SDL_GetAudioPlaybackDevices(&playbackCount);
    if (playbackIds) {
        for (int i = 0; i < playbackCount && !m_playback.Full(); ++i) {
            AudioDeviceInfo info{};
            info.id = AudioDeviceId{playbackIds[i]};
            info.isRecording = false;
            const char* name = SDL_GetAudioDeviceName(playbackIds[i]);
            if (name) info.name.Assign(name);
            (void)m_playback.TryPush(info);
        }
        SDL_free(playbackIds);
    }

    int recordingCount = 0;
    SDL_AudioDeviceID* recordingIds = SDL_GetAudioRecordingDevices(&recordingCount);
    if (recordingIds) {
        for (int i = 0; i < recordingCount && !m_recording.Full(); ++i) {
            AudioDeviceInfo info{};
            info.id = AudioDeviceId{recordingIds[i]};
            info.isRecording = true;
            const char* name = SDL_GetAudioDeviceName(recordingIds[i]);
            if (name) info.name.Assign(name);
            (void)m_recording.TryPush(info);
        }
        SDL_free(recordingIds);
    }

    ++m_revision;
    return Ok;
}

bool AudioDeviceDirectory::OnAdded(AudioDeviceId id, bool recording) noexcept {
    auto& target = recording ? m_recording : m_playback;
    for (const auto& dev : target.View()) {
        if (dev.id == id) return false;
    }

    if (target.Full()) return false;

    AudioDeviceInfo info{};
    info.id = id;
    info.isRecording = recording;
    const char* name = SDL_GetAudioDeviceName(id.value);
    if (name) info.name.Assign(name);

    (void)target.TryPush(info);
    ++m_revision;
    return true;
}

bool AudioDeviceDirectory::OnRemoved(AudioDeviceId id, bool recording) noexcept {
    auto& target = recording ? m_recording : m_playback;
    for (usize i = 0; i < target.Size(); ++i) {
        if (target[i].id == id) {
            (void)target.EraseAt(i);
            ++m_revision;
            return true;
        }
    }
    return false;
}

} // namespace EngineN

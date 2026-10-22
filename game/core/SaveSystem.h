#pragma once

#include "core/Types.h"
#include "game/core/GameTypes.h"
#include "platform/Platform.h"

#include <array>
#include <span>

namespace vots {

enum class SaveErrc : u8 {
    None = 0,
    InvalidBufferSize,
    BadMagicOrVersion,
    ChecksumMismatch,
    InvalidStateData,
    FileIoFailed,
};

// Fixed-size binary save record ("VOTS" v1) storing checkpoint, Triad Deck, Husk, map pins, and world flags.
// Trivially copyable with zero compiler padding holes; validated via FNV-1a 64-bit checksum.
// Invariant: magic == kMagic, version == kVersion, and checksum matches ComputeChecksum().
struct alignas(8) SaveData {
    static constexpr u32 kMagic = 0x53544F56U; // "VOTS"
    static constexpr u16 kVersion = 1U;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 unlockedEchoMask{0x000FU};
    u64 checksum{0ULL};

    f32 checkpointX{0.0f};
    f32 checkpointY{0.0f};
    u32 carriedTuningShards{0U};
    i32 playerCurrentHp{100};
    i32 playerBaseMaxHp{100};
    TriadDeck triadDeck{};

    u8 checkpointRoomId{0U};
    u8 harmonyGauge{100U};
    u8 discoveredRoomsMask{0x01U};
    u8 equippedSigilMask{0U};
    u8 achievedEnding{0U};
    u8 pinCount{0U};
    u16 worldFlagsWord0{0x0011U}; // AwakenedIgnisLantern | ResonatorAshSumpActive

    LostWickHuskRecord husk{};
    std::array<MapPinRecord, kMaxMapPins> mapPins{};

    [[nodiscard]] constexpr u64 ComputePayloadChecksum() const noexcept {
        u64 h = 14695981039346656037ULL;
        const auto mixU64 = [&](u64 v) constexpr noexcept {
            h ^= v;
            h *= 1099511628211ULL;
        };
        mixU64(static_cast<u64>(magic));
        mixU64(static_cast<u64>(version));
        mixU64(static_cast<u64>(unlockedEchoMask));
        mixU64(static_cast<u64>(carriedTuningShards));
        mixU64(static_cast<u64>(static_cast<u32>(playerCurrentHp)));
        mixU64(static_cast<u64>(static_cast<u32>(playerBaseMaxHp)));
        mixU64(static_cast<u64>(triadDeck.EquippedMask()));
        mixU64(static_cast<u64>(triadDeck.activeSlot));
        mixU64(static_cast<u64>(checkpointRoomId));
        mixU64(static_cast<u64>(harmonyGauge));
        mixU64(static_cast<u64>(discoveredRoomsMask));
        mixU64(static_cast<u64>(equippedSigilMask));
        mixU64(static_cast<u64>(achievedEnding));
        mixU64(static_cast<u64>(pinCount));
        mixU64(static_cast<u64>(worldFlagsWord0));
        mixU64(static_cast<u64>(husk.bankedTuningShards));
        mixU64(static_cast<u64>(husk.roomId));
        mixU64(static_cast<u64>(husk.active));
        for (u32 i = 0U; i < kMaxMapPins; ++i) {
            mixU64(static_cast<u64>(mapPins[i].roomId));
            mixU64(static_cast<u64>(mapPins[i].kind));
            mixU64(static_cast<u64>(mapPins[i].noteTag));
        }
        return (h == 0ULL) ? 1ULL : h;
    }
};
static_assert(sizeof(SaveData) == 448);

// Stateless binary save/load serializer and atomic disk persistence manager.
class SaveSystem {
public:
    [[nodiscard]] static en::Result<usize, SaveErrc> SerializeToBuffer(
        const SaveData& data,
        std::span<u8> outBytes) noexcept;

    [[nodiscard]] static en::Result<SaveData, SaveErrc> DeserializeFromBuffer(
        std::span<const u8> inBytes) noexcept;

    [[nodiscard]] static en::Result<void, SaveErrc> SaveToFileAtomic(
        const en::FilePath& path,
        const SaveData& data) noexcept;

    [[nodiscard]] static en::Result<SaveData, SaveErrc> LoadFromFile(
        const en::FilePath& path) noexcept;
};

} // namespace vots

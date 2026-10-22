#include "game/core/SaveSystem.h"
#include "game/core/GameConfig.h"

#include <cstring>

namespace vots {

en::Result<usize, SaveErrc> SaveSystem::SerializeToBuffer(
    const SaveData& data,
    std::span<u8> outBytes) noexcept {
    if (outBytes.size() < sizeof(SaveData)) {
        return en::Err(SaveErrc::InvalidBufferSize);
    }
    if (!IsValidEcho(data.triadDeck.slots[0]) ||
        !IsValidEcho(data.triadDeck.slots[1]) ||
        !IsValidEcho(data.triadDeck.slots[2]) ||
        data.harmonyGauge > kMaxHarmonyGauge ||
        data.pinCount > kMaxMapPins) {
        return en::Err(SaveErrc::InvalidStateData);
    }

    SaveData staged = data;
    staged.magic = SaveData::kMagic;
    staged.version = SaveData::kVersion;
    staged.checksum = staged.ComputePayloadChecksum();

    std::memcpy(outBytes.data(), &staged, sizeof(SaveData));
    return sizeof(SaveData);
}

en::Result<SaveData, SaveErrc> SaveSystem::DeserializeFromBuffer(
    std::span<const u8> inBytes) noexcept {
    if (inBytes.size() < sizeof(SaveData)) {
        return en::Err(SaveErrc::InvalidBufferSize);
    }

    SaveData out{};
    std::memcpy(&out, inBytes.data(), sizeof(SaveData));

    if (out.magic != SaveData::kMagic || out.version != SaveData::kVersion) {
        return en::Err(SaveErrc::BadMagicOrVersion);
    }
    if (out.checksum != out.ComputePayloadChecksum()) {
        return en::Err(SaveErrc::ChecksumMismatch);
    }
    if (!IsValidEcho(out.triadDeck.slots[0]) ||
        !IsValidEcho(out.triadDeck.slots[1]) ||
        !IsValidEcho(out.triadDeck.slots[2]) ||
        out.harmonyGauge > kMaxHarmonyGauge ||
        out.pinCount > kMaxMapPins ||
        out.playerBaseMaxHp <= 0) {
        return en::Err(SaveErrc::InvalidStateData);
    }
    return out;
}

en::Result<void, SaveErrc> SaveSystem::SaveToFileAtomic(
    const en::FilePath& path,
    const SaveData& data) noexcept {
    std::array<u8, sizeof(SaveData)> buf{};
    auto ser = SerializeToBuffer(data, buf);
    if (!ser.has_value()) {
        return en::Err(ser.error());
    }
    auto wr = en::Platform::WriteFileAtomic(path, buf);
    if (!wr.has_value()) {
        return en::Err(SaveErrc::FileIoFailed);
    }
    return {};
}

en::Result<SaveData, SaveErrc> SaveSystem::LoadFromFile(
    const en::FilePath& path) noexcept {
    std::array<u8, sizeof(SaveData)> buf{};
    auto rd = en::Platform::ReadFileBuffer(path, buf);
    if (!rd.has_value() || rd.value() != sizeof(SaveData)) {
        return en::Err(SaveErrc::FileIoFailed);
    }
    return DeserializeFromBuffer(buf);
}

} // namespace vots

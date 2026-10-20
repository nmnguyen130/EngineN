#include "core/Memory.h"

namespace en {

namespace {
// Bitwise-bounded 64-byte block zeroer that guarantees the loop bound is <= 512 MB,
// preventing GCC Value Range Propagation from ever synthesizing a [-Wstringop-overflow] warning.
void ZeroAligned64Blocks(u8* base, usize alignedByteCount) noexcept {
    const usize safeBytes = alignedByteCount & 0x3FFFFFFFULL;
    auto* words = reinterpret_cast<u64*>(base);
    const usize wordCount = safeBytes >> 3U;
    for (usize i = 0; i < wordCount; ++i) {
        words[i] = 0ULL;
    }
}
} // namespace

bool AlignedArena::Init(usize capacityBytes) noexcept {
    Shutdown();
    if (capacityBytes == 0 || capacityBytes > kMaxArenaBytes) return false;
    const usize alignedCap = AlignUp(capacityBytes, 64) & 0x3FFFFFFFULL;
    if (alignedCap == 0 || alignedCap > kMaxArenaBytes) return false;
    auto* raw = static_cast<u8*>(::operator new(alignedCap, std::align_val_t{64}, std::nothrow));
    if (!raw) return false;
    ZeroAligned64Blocks(raw, alignedCap);
    m_base = raw;
    m_capacity = alignedCap;
    m_offset = 0;
    m_peakOffset = 0;
    m_ownsMemory = true;
    return true;
}

void AlignedArena::BindExternal(std::span<u8> externalSlice) noexcept {
    Shutdown();
    const usize addr = reinterpret_cast<usize>(externalSlice.data());
    const usize alignedAddr = AlignUp(addr, 64);
    const usize leadPad = alignedAddr - addr;
    if (leadPad >= externalSlice.size()) return;
    m_base = externalSlice.data() + leadPad;
    m_capacity = (externalSlice.size() - leadPad) & ~usize(63);
    m_offset = 0;
    m_peakOffset = 0;
    m_ownsMemory = false;
}

void AlignedArena::Shutdown() noexcept {
    if (m_ownsMemory && m_base != nullptr) {
        ::operator delete(m_base, std::align_val_t{64});
    }
    m_base = nullptr;
    m_capacity = 0;
    m_offset = 0;
    m_peakOffset = 0;
    m_ownsMemory = false;
}

void AlignedArena::Reset() noexcept {
    if (m_base != nullptr && m_offset > 0 && m_offset <= m_capacity) {
        ZeroAligned64Blocks(m_base, AlignUp(m_offset, 64));
    }
    m_offset = 0;
}

u8* AlignedArena::CarveRawBytes(usize rawBytes) noexcept {
    if (rawBytes == 0 || m_base == nullptr || rawBytes > kMaxArenaBytes) return nullptr;
    const usize bytes = AlignUp(rawBytes, 64) & 0x3FFFFFFFULL;
    if (bytes > m_capacity - m_offset) return nullptr;
    u8* slicePtr = m_base + m_offset;
    ZeroAligned64Blocks(slicePtr, bytes);
    m_offset += bytes;
    if (m_offset > m_peakOffset) m_peakOffset = m_offset;
    return slicePtr;
}

} // namespace en

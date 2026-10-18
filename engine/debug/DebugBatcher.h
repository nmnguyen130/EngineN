#pragma once

#include "debug/DebugTypes.h"

namespace en::renderer { class RenderList; }

namespace en::debug {

// Emits lines, boxes, circles, and monospace ASCII text using a single shared debug atlas texture.
// Non-owning wrapper around renderer::RenderList.
// Invariant: All emitted QuadCmds share identical SortKey upper bytes (pass, layer, depth, blend,
// viewSlot, texture), allowing RenderList::Compile() to bypass radix sorting in O(1).
class DebugBatcher {
public:
    static constexpr f32 kGlyphWidth  = 8.0f;
    static constexpr f32 kGlyphHeight = 8.0f;

    DebugBatcher(renderer::RenderList& list,
                 renderer::TextureHandle debugAtlas,
                 u8 passId,
                 u8 viewSlot) noexcept;

    void PushSolidRect(renderer::FRect rect, renderer::Color color) noexcept;
    void PushBoxOutline(renderer::FRect rect, f32 thickness, renderer::Color color) noexcept;
    void PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, renderer::Color color) noexcept;
    void PushArrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 headSize, f32 thickness, renderer::Color color) noexcept;
    void PushCircleOutline(f32 cx, f32 cy, f32 radius, u8 segments, f32 thickness, renderer::Color color) noexcept;
    void PushText(f32 x, f32 y, std::string_view text, renderer::Color color, f32 scale = 1.0f) noexcept;

    static char* FormatI64(char* out, char* end, i64 val) noexcept;
    static char* FormatU64Hex(char* out, char* end, u64 val, u8 digits = 16) noexcept;
    static char* FormatF32Fixed2(char* out, char* end, f32 val) noexcept;

private:
    renderer::RenderList& m_list;
    renderer::QuadCmd m_templateQuad{};
};

// Zero-allocation stack string builder for HUD text formatting without repetitive pointer arithmetic.
class DebugTextLine {
public:
    constexpr void Clear() noexcept { m_len = 0; }

    DebugTextLine& Str(std::string_view sv) noexcept {
        const usize rem = m_buf.size() - m_len;
        const usize n   = sv.size() < rem ? sv.size() : rem;
        for (usize i = 0; i < n; ++i) {
            m_buf[m_len + i] = sv[i];
        }
        m_len += n;
        return *this;
    }

    DebugTextLine& Char(char c) noexcept {
        if (m_len < m_buf.size()) {
            m_buf[m_len++] = c;
        }
        return *this;
    }

    DebugTextLine& I64(i64 val) noexcept {
        char* end = m_buf.data() + m_buf.size();
        char* cur = DebugBatcher::FormatI64(m_buf.data() + m_len, end, val);
        m_len     = static_cast<usize>(cur - m_buf.data());
        return *this;
    }

    DebugTextLine& U64Hex(u64 val, u8 digits = 8) noexcept {
        char* end = m_buf.data() + m_buf.size();
        char* cur = DebugBatcher::FormatU64Hex(m_buf.data() + m_len, end, val, digits);
        m_len     = static_cast<usize>(cur - m_buf.data());
        return *this;
    }

    DebugTextLine& F32(f32 val) noexcept {
        char* end = m_buf.data() + m_buf.size();
        char* cur = DebugBatcher::FormatF32Fixed2(m_buf.data() + m_len, end, val);
        m_len     = static_cast<usize>(cur - m_buf.data());
        return *this;
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept {
        return {m_buf.data(), m_len};
    }

private:
    std::array<char, 128> m_buf{};
    usize m_len{0};
};

// Fixed 2048-slot transient shape pool managed by 64-bit occupancy bitwords.
// Non-owning view over the 64 KB shape slice in DebugSystem's arena.
// Invariant: m_activeCount == sum(popcount(m_activeWords)); active slots are scanned via countr_zero.
class DebugShapePool {
public:
    static constexpr usize kMaxShapes = 2048;
    static constexpr usize kWordCount = kMaxShapes / 64;

    void BindArena(std::span<DebugShapeCmd, kMaxShapes> pool) noexcept;
    void Clear() noexcept;
    constexpr void SetCurrentTick(u64 tickIndex) noexcept { m_currentTickLow = static_cast<u32>(tickIndex); }

    void AddLine(f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba8 = 0xFFFFFFFFu,
                 f32 thickness = 1.0f, u32 durationTicks = 1) noexcept;
    void AddBox(f32 cx, f32 cy, f32 halfW, f32 halfH, u32 rgba8 = 0x00FF00FFu,
                bool filled = false, f32 thickness = 1.0f, u32 durationTicks = 1) noexcept;
    void AddArrow(f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba8 = 0xFFFF00FFu,
                  f32 headSize = 4.0f, f32 thickness = 1.0f, u32 durationTicks = 1) noexcept;
    void AddCircle(f32 cx, f32 cy, f32 radius, u32 rgba8 = 0x00FFFFFFu,
                   u8 segments = 16, f32 thickness = 1.0f, u32 durationTicks = 1) noexcept;
    void AddTextWorld(f32 wx, f32 wy, std::string_view text12,
                      u32 rgba8 = 0xFFFFFFFFu, u32 durationTicks = 1) noexcept;
    void AddTextScreen(f32 sx, f32 sy, std::string_view text12,
                       u32 rgba8 = 0xFFFFFFFFu, u32 durationTicks = 1) noexcept;

    void EmitAndExpire(u64 currentTickIndex,
                       DebugBatcher& worldBatcher,
                       DebugBatcher& hudBatcher) noexcept;

    void EmitAndExpire(u64 currentTickIndex,
                       renderer::TextureHandle debugAtlas,
                       renderer::RenderList& outList) noexcept;

    [[nodiscard]] constexpr u32 ActiveCount() const noexcept { return m_activeCount; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    [[nodiscard]] DebugShapeCmd* AllocateSlot(DebugShapeKind kind, f32 x0, f32 y0,
                                              u32 rgba8, f32 thickness, u32 durationTicks) noexcept;

    std::span<DebugShapeCmd> m_pool{};
    std::array<u64, kWordCount> m_activeWords{};
    u32 m_allocCursor{0};
    u32 m_currentTickLow{0};
    u32 m_activeCount{0};
};

} // namespace en::debug

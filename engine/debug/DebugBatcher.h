#pragma once

#include "debug/DebugTypes.h"

namespace en {

// Immediate-mode 2D primitive and bitmap text quad emitter writing into RenderList.
class alignas(16) DebugBatcher {
public:
    constexpr DebugBatcher(
        RenderList& list,
        u8 passId = kDebugWorldPassId,
        u8 viewSlot = kDebugWorldViewSlot,
        u16 depth = kDebugFixedDepth) noexcept
        : m_list(&list),
          m_depth(depth),
          m_passId(passId),
          m_viewSlot(viewSlot) {}

    void PushSolidRect(Rect rect, Color color) noexcept;
    void PushBoxOutline(Rect rect, f32 thickness, Color color) noexcept;
    void PushObbOutline(Vec2 center, Vec2 halfExtents, f32 cosRot, f32 sinRot, f32 thickness, Color color) noexcept;
    void PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Color color) noexcept;
    void PushArrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 headSize, f32 thickness, Color color) noexcept;
    void PushCircleOutline(f32 cx, f32 cy, f32 radius, u8 segments, f32 thickness, Color color) noexcept;
    void PushCapsuleYOutline(f32 cx, f32 cy, f32 halfW, f32 halfH, f32 thickness, Color color) noexcept;
    void PushCrosshair(f32 cx, f32 cy, f32 halfSize, f32 thickness, Color color) noexcept;
    void PushText(f32 x, f32 y, std::string_view text, Color color, f32 scale = 1.0f) noexcept;

    [[nodiscard]] static char* AppendStr(char* out, char* end, std::string_view sv) noexcept;
    [[nodiscard]] static char* FormatI64(char* out, char* end, i64 val) noexcept;
    [[nodiscard]] static char* FormatU64Hex(char* out, char* end, u64 val, u8 digits = 8U) noexcept;
    [[nodiscard]] static char* FormatF32Fixed2(char* out, char* end, f32 val) noexcept;

    [[nodiscard]] static constexpr Color UnpackRgba8(u32 rgba8) noexcept {
        constexpr f32 kInv = 1.0f / 255.0f;
        return {
            static_cast<f32>((rgba8 >> 24U) & 0xFFU) * kInv,
            static_cast<f32>((rgba8 >> 16U) & 0xFFU) * kInv,
            static_cast<f32>((rgba8 >>  8U) & 0xFFU) * kInv,
            static_cast<f32>((rgba8 >>  0U) & 0xFFU) * kInv,
        };
    }

private:
    RenderList* m_list{nullptr};
    u16 m_depth{kDebugFixedDepth};
    u8 m_passId{kDebugWorldPassId};
    u8 m_viewSlot{kDebugWorldViewSlot};
};
static_assert(sizeof(DebugBatcher) == 16);

// Two-level bitmask transient/persistent 2D debug shape pool.
class alignas(32) DebugShapePool {
public:
    constexpr void Init(std::span<DebugShapeCmd, kMaxDebugShapes> storage) noexcept {
        m_pool = storage.data();
        Clear();
    }

    constexpr void Clear() noexcept {
        m_activeWords.fill(0ULL);
        m_nonEmptyWordMask = 0U;
        m_nonFullWordMask = 0xFFFFFFFFU;
        m_currentTick = 0U;
        m_activeCount = 0U;
        m_evictCursor = 0U;
    }

    constexpr void SetCurrentTick(u32 tickIndex) noexcept { m_currentTick = tickIndex; }

    void AddLine(f32 x0, f32 y0, f32 x1, f32 y1, Color color, f32 thickness = 0.05f, u32 durationTicks = 0U) noexcept;
    void AddBox(Rect rect, Color color, f32 thickness = 0.05f, bool solid = false, u32 durationTicks = 0U) noexcept;
    void AddArrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 headSize, Color color, f32 thickness = 0.05f, u32 durationTicks = 0U) noexcept;
    void AddCircle(f32 cx, f32 cy, f32 radius, Color color, u8 segments = 16U, f32 thickness = 0.05f, u32 durationTicks = 0U) noexcept;
    void AddCapsuleY(f32 cx, f32 cy, f32 halfW, f32 halfH, Color color, f32 thickness = 0.05f, u32 durationTicks = 0U) noexcept;
    void AddTextWorld(f32 x, f32 y, std::string_view text, Color color, f32 scale = 0.05f, u32 durationTicks = 0U) noexcept;
    void AddTextScreen(f32 x, f32 y, std::string_view text, Color color, f32 scale = 1.0f, u32 durationTicks = 0U) noexcept;

    void EmitAndExpire(u32 currentTickIndex, DebugBatcher& worldBatcher, DebugBatcher& hudBatcher) noexcept;

    [[nodiscard]] constexpr u32 ActiveCount() const noexcept { return m_activeCount; }

private:
    [[nodiscard]] DebugShapeCmd* AllocateSlot(u32 durationTicks) noexcept;
    [[nodiscard]] DebugShapeCmd* AllocateShape(
        DebugShapeCmd::Kind kind,
        f32 x0,
        f32 y0,
        Color color,
        f32 thicknessOrScale,
        u32 durationTicks) noexcept;

    std::array<u64, 32> m_activeWords{};
    DebugShapeCmd* m_pool{nullptr};
    u32 m_nonEmptyWordMask{0U};
    u32 m_nonFullWordMask{0xFFFFFFFFU};
    u32 m_currentTick{0U};
    u16 m_activeCount{0U};
    u16 m_evictCursor{0U};
};
static_assert(sizeof(DebugShapePool) == 288);

} // namespace en

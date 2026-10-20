#pragma once

#include "core/Memory.h"
#include "debug/DebugTypes.h"
#include "renderer/RenderList.h"

namespace en {

// Immediate-mode debug primitive and zero-allocation text quad emitter.
// Value type wrapping a RenderList reference.
// Invariant: Format* helpers never allocate heap memory or write past buffer end.
class DebugBatcher {
public:
    constexpr DebugBatcher(
        RenderList& list,
        u8 passId = 0,
        u8 viewSlot = 0,
        GpuTextureHandle debugAtlas = kWhiteGpuTexture) noexcept
        : m_list(list), m_atlas(debugAtlas), m_passId(passId), m_viewSlot(viewSlot) {}

    void PushSolidRect(Rect rect, Color color) noexcept;
    void PushBoxOutline(Rect rect, f32 thickness, Color color) noexcept;
    void PushLine(Vec2 a, Vec2 b, f32 thickness, Color color) noexcept;
    void PushArrow(Vec2 start, Vec2 end, f32 headSize, f32 thickness, Color color) noexcept;
    void PushCircleOutline(Vec2 center, f32 radius, u32 segments, f32 thickness, Color color) noexcept;
    void PushText(Vec2 pos, std::string_view text, Color color, f32 scale = 1.0f) noexcept;

    static char* FormatI64(char* out, char* end, i64 val) noexcept;
    static char* FormatU64Hex(char* out, char* end, u64 val, u8 digits = 16) noexcept;
    static char* FormatF32Fixed2(char* out, char* end, f32 val) noexcept;

private:
    RenderList& m_list;
    GpuTextureHandle m_atlas{kWhiteGpuTexture};
    u8 m_passId{0};
    u8 m_viewSlot{0};
};

// Persistent tick-expiring debug shape pool for multi-frame diagnostic gizmos.
class alignas(64) DebugShapePool {
public:
    enum class ShapeKind : u8 { Line = 0, Box = 1, Arrow = 2, Circle = 3 };

    struct alignas(8) ShapeCmd {
        Vec2 a{};
        Vec2 b{};
        Color color{};
        f32 thickness{0.04f};
        u32 expireTick{0};
        ShapeKind kind{ShapeKind::Line};
        bool active{false};
        u16 category{0};
    };

    [[nodiscard]] bool Init(AlignedArena& arena, u32 maxShapes = 512) noexcept;
    void Clear() noexcept;

    void AddLine(Vec2 a, Vec2 b, Color color, u32 durationTicks = 1, f32 thickness = 0.04f) noexcept;
    void AddBox(Rect rect, Color color, u32 durationTicks = 1, f32 thickness = 0.04f) noexcept;
    void AddArrow(Vec2 start, Vec2 end, Color color, u32 durationTicks = 1, f32 thickness = 0.04f) noexcept;
    void AddCircle(Vec2 center, f32 radius, Color color, u32 durationTicks = 1, f32 thickness = 0.04f) noexcept;

    void EmitAndExpire(u64 currentTick, DebugBatcher& worldBatcher) noexcept;
    [[nodiscard]] constexpr u32 ActiveCount() const noexcept { return m_activeCount; }

private:
    void PushShape(const ShapeCmd& cmd, u32 durationTicks) noexcept;

    std::span<ShapeCmd> m_shapes{};
    u32 m_currentTick{0};
    u32 m_activeCount{0};
};

} // namespace en

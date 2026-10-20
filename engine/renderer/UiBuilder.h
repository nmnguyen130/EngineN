#pragma once

#include "input/InputTypes.h"
#include "renderer/RenderList.h"

namespace en {

struct UiTheme {
    Color panelBg{Color::FromRGBA8(18, 22, 34, 220)};
    Color panelBorder{Color::FromRGBA8(90, 120, 175, 230)};
    Color buttonNormal{Color::FromRGBA8(36, 46, 68, 240)};
    Color buttonHover{Color::FromRGBA8(62, 84, 128, 250)};
    Color buttonActive{Color::FromRGBA8(95, 135, 205, 255)};
    Color textPrimary{Color::FromRGBA8(240, 245, 255, 255)};
    Color accent{Color::FromRGBA8(80, 215, 190, 255)};
    f32 padding{10.0f};
    f32 itemSpacing{8.0f};
};

// Immediate-mode data-oriented UI layout builder emitting quads directly into a RenderList.
// Value type; zero heap allocation.
// Invariant: cursor advances monotonically inside each BeginVerticalPanel/EndPanel scope.
class UiBuilder {
public:
    constexpr UiBuilder(
        RenderList& list,
        const FrameInput& input,
        u8 passId = 0,
        u8 viewSlot = 15,
        const UiTheme& theme = {}) noexcept
        : m_list(list), m_input(input), m_theme(theme), m_passId(passId), m_viewSlot(viewSlot) {}

    void BeginVerticalPanel(Rect bounds, std::string_view title = {}) noexcept;
    void EndPanel() noexcept;

    void Label(std::string_view text, f32 scale = 1.0f) noexcept;
    [[nodiscard]] bool Button(std::string_view label, f32 height = 28.0f) noexcept;
    void ProgressBar(std::string_view label, f32 ratio, Color fillColor, f32 height = 20.0f) noexcept;
    [[nodiscard]] bool SliderFloat(std::string_view label, f32& ioValue, f32 minVal, f32 maxVal, f32 height = 22.0f) noexcept;

    [[nodiscard]] constexpr Vec2 Cursor() const noexcept { return m_cursor; }

private:
    RenderList& m_list;
    const FrameInput& m_input;
    UiTheme m_theme{};
    Rect m_panelBounds{};
    Vec2 m_cursor{};
    u8 m_passId{0};
    u8 m_viewSlot{15};
};

} // namespace en

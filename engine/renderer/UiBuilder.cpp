#include "renderer/UiBuilder.h"
#include <algorithm>

namespace en {

void UiBuilder::BeginVerticalPanel(Rect bounds, std::string_view title) noexcept {
    m_panelBounds = bounds;
    m_cursor = {bounds.x + m_theme.padding, bounds.y + m_theme.padding};
    NineSliceDesc ns{};
    ns.outerRect = bounds;
    ns.color = m_theme.panelBg;
    m_list.PushNineSlice(ns, m_passId, 200, m_viewSlot);

    if (!title.empty()) {
        m_list.PushText(m_cursor, title, m_theme.accent, 1.15f, m_passId, 205, m_viewSlot);
        m_cursor.y += 20.0f + m_theme.itemSpacing;
    }
}

void UiBuilder::EndPanel() noexcept {}

void UiBuilder::Label(std::string_view text, f32 scale) noexcept {
    m_list.PushText(m_cursor, text, m_theme.textPrimary, scale, m_passId, 205, m_viewSlot);
    m_cursor.y += 14.0f * scale + m_theme.itemSpacing;
}

bool UiBuilder::Button(std::string_view label, f32 height) noexcept {
    const f32 w = std::max(40.0f, m_panelBounds.w - 2.0f * m_theme.padding);
    const Rect r{m_cursor.x, m_cursor.y, w, height};
    const bool hovered = r.Contains(m_input.mousePos);
    const bool clicked = hovered && (m_input.pressed != 0);

    const Color bg = clicked ? m_theme.buttonActive : (hovered ? m_theme.buttonHover : m_theme.buttonNormal);
    m_list.PushRect(r, bg, m_passId, 202, 32768, m_viewSlot);
    m_list.PushText({r.x + 8.0f, r.y + (height - 12.0f) * 0.5f}, label, m_theme.textPrimary, 1.0f, m_passId, 205, m_viewSlot);
    m_cursor.y += height + m_theme.itemSpacing;
    return clicked;
}

void UiBuilder::ProgressBar(std::string_view label, f32 ratio, Color fillColor, f32 height) noexcept {
    const f32 w = std::max(40.0f, m_panelBounds.w - 2.0f * m_theme.padding);
    const Rect r{m_cursor.x, m_cursor.y, w, height};
    m_list.PushProgressBar(r, ratio, fillColor, m_theme.buttonNormal, m_theme.panelBorder, m_passId, 202, m_viewSlot);
    if (!label.empty()) {
        m_list.PushText({r.x + 6.0f, r.y + (height - 12.0f) * 0.5f}, label, m_theme.textPrimary, 0.9f, m_passId, 206, m_viewSlot);
    }
    m_cursor.y += height + m_theme.itemSpacing;
}

bool UiBuilder::SliderFloat(
    std::string_view label, f32& ioValue, f32 minVal, f32 maxVal, f32 height) noexcept {
    const f32 w = std::max(40.0f, m_panelBounds.w - 2.0f * m_theme.padding);
    const Rect r{m_cursor.x, m_cursor.y, w, height};
    bool changed = false;
    if (r.Contains(m_input.mousePos) && m_input.held != 0 && maxVal > minVal) {
        const f32 t = std::clamp((m_input.mousePos.x - r.x) / r.w, 0.0f, 1.0f);
        ioValue = minVal + (maxVal - minVal) * t;
        changed = true;
    }
    const f32 ratio = (maxVal > minVal) ? (ioValue - minVal) / (maxVal - minVal) : 0.0f;
    ProgressBar(label, ratio, m_theme.accent, height);
    return changed;
}

} // namespace en

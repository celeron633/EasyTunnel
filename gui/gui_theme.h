#pragma once

#include "imgui.h"

#include "../tunnel_engine.h"

// Shared visual language for the GUI client. The connection state pill, the
// TX/RX activity dots and the chart accents all come from here so the three
// tabs and the status bar stay consistent.
namespace gui_theme {

inline const ImVec4 kTx(0.88f, 0.16f, 0.18f, 1.0f);
inline const ImVec4 kRx(0.16f, 0.78f, 0.24f, 1.0f);
inline const ImVec4 kLatency(0.95f, 0.76f, 0.12f, 1.0f);

struct StateStyle {
    const char* label;
    ImVec4 color;
};

inline StateStyle StyleFor(TunnelState state) {
    switch (state) {
        case TunnelState::Connected:
            return {"CONNECTED", ImVec4(0.24f, 0.85f, 0.38f, 1.0f)};
        case TunnelState::Connecting:
            return {"CONNECTING", ImVec4(1.00f, 0.80f, 0.20f, 1.0f)};
        case TunnelState::Waiting:
            return {"WAITING", ImVec4(0.36f, 0.68f, 1.00f, 1.0f)};
        case TunnelState::Error:
            return {"ERROR", ImVec4(1.00f, 0.38f, 0.38f, 1.0f)};
        default:
            return {"DISCONNECTED", ImVec4(0.58f, 0.58f, 0.58f, 1.0f)};
    }
}

// A rounded pill carrying the connection state. Drawn directly so it cannot be
// mistaken for a button the user is supposed to press.
inline void Badge(const char* label, const ImVec4& color) {
    const ImVec2 padding(9.0f, 2.0f);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end(start.x + textSize.x + padding.x * 2.0f,
                     start.y + textSize.y + padding.y * 2.0f);
    ImVec4 background = color;
    background.w = 0.20f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, end, ImGui::GetColorU32(background),
                        (end.y - start.y) * 0.5f);
    draw->AddText(ImVec2(start.x + padding.x, start.y + padding.y),
                  ImGui::GetColorU32(color), label);
    ImGui::Dummy(ImVec2(end.x - start.x, end.y - start.y));
}

// Traffic activity indicator: full colour while packets are moving, heavily
// dimmed otherwise.
inline void ActivityDot(bool active, const ImVec4& color) {
    ImVec4 shade = color;
    if (!active) {
        shade.x *= 0.30f;
        shade.y *= 0.30f;
        shade.z *= 0.30f;
    }
    const float lineHeight = ImGui::GetTextLineHeight();
    const float radius = lineHeight * 0.26f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(start.x + radius, start.y + lineHeight * 0.5f), radius,
        ImGui::GetColorU32(shade));
    ImGui::Dummy(ImVec2(radius * 2.0f, lineHeight));
}

// Right-aligns the next text item on the current line.
inline void TextRightAligned(const char* text) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
        - ImGui::CalcTextSize(text).x;
    if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);
    ImGui::TextDisabled("%s", text);
}

}  // namespace gui_theme

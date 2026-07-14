#include "gui_app.h"

#include "imgui.h"

#include "../log.h"

namespace {
int ScrollLogToEnd(ImGuiInputTextCallbackData* data) {
    auto* scrollPending = static_cast<bool*>(data->UserData);
    if (!*scrollPending) return 0;
    data->CursorPos = data->BufTextLen;
    data->SelectionStart = data->BufTextLen;
    data->SelectionEnd = data->BufTextLen;
    *scrollPending = false;
    return 0;
}
}  // namespace

void GuiApp::RenderLogTab() {
    ImGui::Spacing();
    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lock(logMutex_);
        logLines_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy all")) {
        std::string text;
        {
            std::lock_guard<std::mutex> lock(logMutex_);
            for (size_t i = 0; i < logLines_.size(); ++i) {
                if (i != 0) text.push_back('\n');
                text += logLines_[i];
            }
        }
        ImGui::SetClipboardText(text.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy all in-memory log lines to the clipboard");
    }
    ImGui::SameLine();
    const bool autoScrollChanged = ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
    ImGui::SameLine();
    const std::string filePath = GetLogFilePath();
    ImGui::TextDisabled("File: %s", filePath.empty() ? "unavailable" : filePath.c_str());
    ImGui::Separator();

    std::string text;
    size_t lineCount = 0;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        lineCount = logLines_.size();
        for (size_t i = 0; i < logLines_.size(); ++i) {
            if (i != 0) text.push_back('\n');
            text += logLines_[i];
        }
    }

    bool scrollPending = logAutoScroll_
        && (autoScrollChanged || lineCount != renderedLogLineCount_);
    if (scrollPending) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextMultiline(
        "##LogText", text.data(), text.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN),
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_CallbackAlways,
        ScrollLogToEnd, &scrollPending);
    renderedLogLineCount_ = lineCount;
}

void GuiApp::OnLog(LogLevel /*level*/, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex_);
    logLines_.push_back(message);
    constexpr size_t kMaxLogLines = 2000;
    if (logLines_.size() > kMaxLogLines) {
        logLines_.erase(logLines_.begin(), logLines_.begin() + 500);
    }
}

#include "gui_app.h"

#include <cfloat>

#include "imgui.h"

#include "../log.h"
#include "gui_theme.h"

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
    ImGui::SeparatorText("Live log");
    if (ImGui::Button("Copy all", ImVec2(90, 0))) {
        ImGui::SetClipboardText(logText_.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy all in-memory log lines to the clipboard");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(90, 0))) {
        std::lock_guard<std::mutex> lock(logMutex_);
        logLines_.clear();
    }
    ImGui::SameLine();
    const bool autoScrollChanged = ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
    ImGui::SameLine();
    const std::string lineLabel = std::to_string(renderedLogLineCount_) + " lines";
    gui_theme::TextRightAligned(lineLabel.c_str());

    size_t lineCount = 0;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        lineCount = logLines_.size();
        // Joining 2000 lines on every frame was pure waste; the text only
        // changes when a line is appended or the buffer is trimmed.
        if (lineCount != renderedLogLineCount_) {
            logText_.clear();
            for (size_t i = 0; i < lineCount; ++i) {
                if (i != 0) logText_.push_back('\n');
                logText_ += logLines_[i];
            }
        }
    }

    bool scrollPending = logAutoScroll_
        && (autoScrollChanged || lineCount != renderedLogLineCount_);
    if (scrollPending) {
        ImGui::SetKeyboardFocusHere();
    }
    // A negative height fills the remaining space; the reserved line keeps the
    // log file path visible below the box.
    ImGui::InputTextMultiline(
        "##LogText", logText_.data(), logText_.size() + 1,
        ImVec2(-FLT_MIN, -ImGui::GetTextLineHeightWithSpacing()),
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_CallbackAlways,
        ScrollLogToEnd, &scrollPending);
    renderedLogLineCount_ = lineCount;
    const std::string filePath = GetLogFilePath();
    ImGui::TextDisabled("File: %s", filePath.empty() ? "unavailable" : filePath.c_str());
}

void GuiApp::OnLog(LogLevel /*level*/, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex_);
    logLines_.push_back(message);
    constexpr size_t kMaxLogLines = 2000;
    if (logLines_.size() > kMaxLogLines) {
        logLines_.erase(logLines_.begin(), logLines_.begin() + 500);
    }
}

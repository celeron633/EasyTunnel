#include "tui_app.h"

#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace {
ftxui::Color LogColor(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return ftxui::Color::GrayDark;
        case LogLevel::Info: return ftxui::Color::White;
        case LogLevel::Warn: return ftxui::Color::Yellow;
        case LogLevel::Error: return ftxui::Color::Red;
        default: return ftxui::Color::White;
    }
}
}  // namespace

ftxui::Component RendezvousTuiApp::BuildLogTab() {
    using namespace ftxui;
    auto history = Checkbox("Show 500 lines", &showLogHistory_);
    auto clear = Button("Clear", [this] {
        std::lock_guard<std::mutex> lock(logMutex_);
        logLines_.clear();
    });
    auto controls = Container::Horizontal({history, clear});
    return Renderer(controls, [this, history, clear] {
        Elements lines;
        {
            std::lock_guard<std::mutex> lock(logMutex_);
            const size_t visible = showLogHistory_ ? 500 : 200;
            const size_t begin = logLines_.size() > visible
                ? logLines_.size() - visible : 0;
            for (size_t i = begin; i < logLines_.size(); ++i) {
                lines.push_back(text(logLines_[i].second) | color(LogColor(logLines_[i].first)));
            }
        }
        if (lines.empty()) lines.push_back(text("No log messages") | dim);
        return vbox({
            hbox({text("Server log") | bold, filler(), history->Render(),
                  text("  "), clear->Render()}),
            separator(),
            vbox(std::move(lines)) | focusPositionRelative(0.0f, 1.0f)
                | vscroll_indicator | frame | flex,
            text("Full log file: " + GetLogFilePath()) | dim,
        }) | border | flex;
    });
}

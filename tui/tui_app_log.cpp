#include "tui_app.h"

#include <cstring>
#include <iostream>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace {
#ifndef _WIN32
std::string Base64Encode(const std::string& input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (size_t i = 0; i < input.size(); i += 3) {
        const size_t remaining = input.size() - i;
        const unsigned int value =
            (static_cast<unsigned char>(input[i]) << 16)
            | (remaining > 1 ? static_cast<unsigned char>(input[i + 1]) << 8 : 0)
            | (remaining > 2 ? static_cast<unsigned char>(input[i + 2]) : 0);
        output.push_back(kAlphabet[(value >> 18) & 0x3f]);
        output.push_back(kAlphabet[(value >> 12) & 0x3f]);
        output.push_back(remaining > 1 ? kAlphabet[(value >> 6) & 0x3f] : '=');
        output.push_back(remaining > 2 ? kAlphabet[value & 0x3f] : '=');
    }
    return output;
}
#endif

bool CopyToClipboard(const std::string& text, std::string* error) {
#ifdef _WIN32
    const int wideLength = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!text.empty() && wideLength == 0) {
        *error = "Cannot convert the selected text";
        return false;
    }

    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    if (wideLength > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            wide.data(), wideLength);
    }
    if (!OpenClipboard(nullptr)) {
        *error = "Cannot open the clipboard";
        return false;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        *error = "Cannot clear the clipboard";
        return false;
    }

    const size_t byteCount = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (memory == nullptr) {
        CloseClipboard();
        *error = "Cannot allocate clipboard memory";
        return false;
    }
    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        *error = "Cannot lock clipboard memory";
        return false;
    }
    std::memcpy(destination, wide.c_str(), byteCount);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        *error = "Cannot update the clipboard";
        return false;
    }
    CloseClipboard();
    return true;
#else
    // OSC 52 lets the local terminal own the clipboard, including over SSH.
    std::cout << "\033]52;c;" << Base64Encode(text) << '\a' << std::flush;
    if (!std::cout) {
        *error = "Cannot send clipboard data to the terminal";
        return false;
    }
    return true;
#endif
}
}  // namespace

ftxui::Component TuiApp::BuildLogTab() {
    using namespace ftxui;

    auto clearLog = Button("Clear log", [this] {
        std::lock_guard<std::mutex> lock(logMutex_);
        logLines_.clear();
    });
    auto copySelection = Button("Copy selection", [this] { CopySelectedText(); });
    auto copyAllLogs = Button("Copy all", [this] { CopyAllLogs(); });
    auto controls = Container::Horizontal({copySelection, copyAllLogs, clearLog});
    return Renderer(controls, [this, copySelection, copyAllLogs, clearLog] {
        Elements lines;
        {
            std::lock_guard<std::mutex> lock(logMutex_);
            const size_t begin = logLines_.size() > 24 ? logLines_.size() - 24 : 0;
            for (size_t i = begin; i < logLines_.size(); ++i) {
                lines.push_back(text(logLines_[i]));
            }
        }
        if (lines.empty()) lines.push_back(text("No log messages"));
        Elements content{
            hbox({text("Log") | bold, filler(), copySelection->Render(),
                  copyAllLogs->Render(), clearLog->Render()}),
            separator(),
            vbox(std::move(lines)) | frame | flex,
        };
        if (!logCopyMessage_.empty()) {
            content.push_back(text(logCopyMessage_)
                              | color(logCopyOk_ ? Color::Green : Color::Red));
        }
        return vbox(std::move(content)) | border | flex;
    });
}

void TuiApp::OnLog(LogLevel /*level*/, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex_);
    logLines_.push_back(message);
    if (logLines_.size() > 2000) logLines_.erase(logLines_.begin(), logLines_.begin() + 500);
    screen_.PostEvent(ftxui::Event::Custom);
}

void TuiApp::CopyAllLogs() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        for (size_t i = 0; i < logLines_.size(); ++i) {
            if (i != 0) text.push_back('\n');
            text += logLines_[i];
        }
    }
    if (text.empty()) {
        logCopyMessage_ = "No log messages to copy";
        logCopyOk_ = false;
        return;
    }

    std::string error;
    logCopyOk_ = CopyToClipboard(text, &error);
    logCopyMessage_ = logCopyOk_ ? "Copied all log messages" : error;
}

void TuiApp::CopySelectedText() {
    const std::string selection = screen_.GetSelection();
    if (selection.empty()) {
        logCopyMessage_ = "Select log text with the mouse first";
        logCopyOk_ = false;
        return;
    }

    std::string error;
    logCopyOk_ = CopyToClipboard(selection, &error);
    logCopyMessage_ = logCopyOk_ ? "Copied selected text" : error;
}

#include "tui_app.h"

#include <filesystem>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../log.h"

TuiApp::TuiApp() : screen_(ftxui::ScreenInteractive::Fullscreen()) {}

TuiApp::~TuiApp() {
    exiting_.store(true);
    StopTicker();
    SetLogCallback({});
    engine_.Stop();
}

bool TuiApp::Init() {
    try {
        configPath_ = std::filesystem::absolute("EasyTunnel_tui.json").string();
    } catch (...) {
        configPath_ = "EasyTunnel_tui.json";
    }
    bool existed = false;
    std::string error;
    if (!LoadTuiConfig(configPath_, &config_, &existed, &error)) {
        configMessage_ = error;
        configSaveOk_ = false;
    }
    SyncTextFromConfig();
    retryDelaySeconds_.store(config_.rendezvousRetryDelaySeconds);
    if (!existed) {
        if (SaveTuiConfig(configPath_, config_, &error)) {
            configMessage_ = "Created " + configPath_;
            configSaveOk_ = true;
        } else {
            configMessage_ = error;
            configSaveOk_ = false;
        }
    } else if (configSaveOk_) {
        configMessage_ = "Loaded " + configPath_;
    }
    savedSignature_ = ConfigSignature();
    SetLogCallback([this](LogLevel level, const std::string& message) {
        OnLog(level, message);
    });
    engine_.SetStateCallback([this](TunnelState state, const std::string& message) {
        OnStateChanged(state, message);
    });
    UpdateDisplayLabels();
    return true;
}

int TuiApp::Run() {
    using namespace ftxui;

    // Ctrl-C copies from the Log tab. It keeps its normal exit behavior when
    // another tab is active and the event is left unhandled below.
    screen_.ForceHandleCtrlC(false);

    auto connection = BuildConnectionTab();
    auto settings = BuildSettingsTab();
    auto logs = BuildLogTab();
    auto tabs = Toggle(&tabs_, &selectedTab_);
    auto tabContent = Container::Tab({connection, settings, logs}, &selectedTab_);
    auto quit = Button("Quit", screen_.ExitLoopClosure());
    auto rootContainer = Container::Vertical({tabs, tabContent, quit});
    auto root = Renderer(rootContainer, [tabs, tabContent, quit] {
        return vbox({
            hbox({text(" EasyTunnel TUI ") | bold, filler(), tabs->Render(),
                  filler(), quit->Render()}),
            separator(),
            tabContent->Render() | flex,
        });
    });
    root |= CatchEvent([this](Event event) {
        if (event == Event::Custom) {
            UpdateStats();
            ProcessAutoWait();
            SaveIfChanged();
            UpdateDisplayLabels();
            return true;
        }
        if (selectedTab_ == 2 && event == Event::CtrlC) {
            if (screen_.GetSelection().empty()) CopyAllLogs();
            else CopySelectedText();
            return true;
        }
        return false;
    });

    tickerRunning_.store(true);
    tickerThread_ = std::thread([this] {
        while (tickerRunning_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (tickerRunning_.load()) screen_.PostEvent(Event::Custom);
        }
    });
    screen_.Loop(root);
    exiting_.store(true);
    StopTicker();
    engine_.Stop();
    SaveIfChanged();
    return 0;
}

void TuiApp::StopTicker() {
    tickerRunning_.store(false);
    if (tickerThread_.joinable()) tickerThread_.join();
}

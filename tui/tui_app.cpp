#include "tui_app.h"

#include <filesystem>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../log.h"
#include "tui_theme.h"

#ifdef _WIN32
#include "windows_tray.h"
#endif

TuiApp::TuiApp() : screen_(ftxui::ScreenInteractive::Fullscreen()) {}

TuiApp::~TuiApp() {
    exiting_.store(true);
    StopTicker();
    SetLogCallback({});
    engine_.Stop();
}

bool TuiApp::Init() {
    try {
        configPath_ = std::filesystem::absolute(kClientConfigFileName).string();
    } catch (...) {
        configPath_ = kClientConfigFileName;
    }
    bool existed = false;
    std::string error;
    if (!LoadClientConfig(configPath_, &config_, &existed, &error)) {
        configMessage_ = error;
        configSaveOk_ = false;
    }
    SyncTextFromConfig();
    retryDelaySeconds_.store(config_.rendezvousRetryDelaySeconds);
    if (!existed) {
        if (SaveClientConfig(configPath_, config_, &error)) {
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
    auto quit = Button("Quit", screen_.ExitLoopClosure(), tui_theme::FlatButton());
    auto rootContainer = Container::Vertical({
        Container::Horizontal({tabs, quit}), tabContent});
    auto root = Renderer(rootContainer, [this, tabs, tabContent, quit] {
        std::string status;
        { std::lock_guard<std::mutex> lock(statusMutex_); status = status_; }
        return vbox({
            hbox({text(" EasyTunnel Client ") | bold, RenderStateBadge(),
                  filler(), quit->Render(), text(" ")}),
            hbox({filler(), tabs->Render(), filler()}),
            separator(),
            tabContent->Render() | flex,
            separator(),
            hbox({text(" " + status) | flex,
                  text(selectedTab_ == 2
                       ? "Tab: focus  |  Ctrl+C: copy log  |  Quit: exit "
                       : "Tab: focus  |  Arrows: select  |  Enter: activate ")
                      | dim}),
        });
    });
    root |= CatchEvent([this](Event event) {
        if (event == Event::Custom) {
            UpdateStats();
            UpdateStatisticsHistory();
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
            // Every statistic on screen has one-second resolution. Redrawing
            // more often only repaints the whole terminal, which is what made
            // Windows Terminal flicker.
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (tickerRunning_.load()) screen_.PostEvent(Event::Custom);
        }
    });
#ifdef _WIN32
    // The tray is optional; without it the TUI simply lacks the icon.
    windowsTray_ = std::make_unique<TuiWindowsTray>();
    if (!windowsTray_->Init(screen_.ExitLoopClosure())) windowsTray_.reset();
#endif
    screen_.Loop(root);
    exiting_.store(true);
#ifdef _WIN32
    windowsTray_.reset();
#endif
    StopTicker();
    engine_.Stop();
    SaveIfChanged();
    return 0;
}

ftxui::Element TuiApp::RenderStateBadge() const {
    using namespace ftxui;
    switch (state_.load()) {
        case TunnelState::Connected:
            return text(" CONNECTED ") | bold | color(Color::Green);
        case TunnelState::Connecting:
            return text(" CONNECTING ") | bold | color(Color::Yellow);
        case TunnelState::Error:
            return text(" ERROR ") | bold | color(Color::Red);
        default:
            return text(" DISCONNECTED ") | bold | color(Color::GrayDark);
    }
}

void TuiApp::StopTicker() {
    tickerRunning_.store(false);
    if (tickerThread_.joinable()) tickerThread_.join();
}

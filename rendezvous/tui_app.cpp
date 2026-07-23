#include "tui_app.h"

#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../log.h"

RendezvousTuiApp::RendezvousTuiApp(std::string configPath)
    : screen_(ftxui::ScreenInteractive::Fullscreen()),
      configPath_(std::move(configPath)) {}

RendezvousTuiApp::~RendezvousTuiApp() {
    StopTicker();
    StopServer();
    SetLogCallback({});
}

bool RendezvousTuiApp::Init(std::string* error) {
    bool created = false;
    if (!LoadOrCreateRendezvousConfig(configPath_, &config_, &created, error)) return false;
    serverConfig_ = config_;
    SetLogFilePath(config_.logFile);
    SetLogLevel(config_.logLevel);
    SetLogCallback([this](LogLevel level, const std::string& message) {
        OnLog(level, message);
    });
    SyncConfigEditor();
    configMessage_ = (created ? "Created " : "Loaded ") + configPath_;
    Log(LogLevel::Info, configMessage_);
    StartServer();
    return true;
}

int RendezvousTuiApp::Run() {
    using namespace ftxui;
    auto dashboard = BuildDashboardTab();
    auto relay = BuildRelayTab();
    auto config = BuildConfigTab();
    auto logs = BuildLogTab();
    auto tabs = Toggle(&tabs_, &selectedTab_);
    auto content = Container::Tab({dashboard, relay, config, logs}, &selectedTab_);
    auto restart = Button("Restart", [this] { RestartServer(); });
    auto quit = Button("Quit", screen_.ExitLoopClosure());
    auto controls = Container::Horizontal({tabs, restart, quit});
    auto rootContainer = Container::Vertical({controls, content});
    auto root = Renderer(rootContainer, [this, tabs, restart, quit, content] {
        const bool online = snapshot_.listening;
        return vbox({
            hbox({text(" EasyTunnel Rendezvous ") | bold,
                  text(online ? " RUNNING " : " STOPPED ") | bold
                      | color(online ? Color::Green : Color::Red),
                  filler(), restart->Render(),
                  text(" "), quit->Render()}),
            hbox({filler(), tabs->Render(), filler()}),
            separator(),
            content->Render() | flex,
            separator(),
            text(" Tab: focus  |  Arrows: select/scroll  |  Ctrl+C: quit ") | dim,
        });
    });
    root |= CatchEvent([this](Event event) {
        if (event == Event::Custom) {
            RefreshSnapshot();
            return true;
        }
        return false;
    });

    tickerRunning_.store(true);
    tickerThread_ = std::thread([this] {
        while (tickerRunning_.load()) {
            // Dashboard values have one-second resolution. Redrawing four
            // times per second only creates redundant full-screen terminal IO.
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (tickerRunning_.load()) screen_.PostEvent(ftxui::Event::Custom);
        }
    });
    screen_.Loop(root);
    StopTicker();
    StopServer();
    return 0;
}

void RendezvousTuiApp::StartServer() {
    if (serverThread_.joinable()) StopServer();
    serverRunning_.store(true);
    serverExitCode_.store(0);
    server_ = std::make_unique<RendezvousServer>(serverConfig_, serverRunning_);
    serverStarted_ = std::chrono::steady_clock::now();
    RendezvousServer* instance = server_.get();
    serverThread_ = std::thread([this, instance] {
        serverExitCode_.store(instance->Run());
        serverRunning_.store(false);
        screen_.PostEvent(ftxui::Event::Custom);
    });
}

void RendezvousTuiApp::StopServer() {
    serverRunning_.store(false);
    if (serverThread_.joinable()) serverThread_.join();
    if (server_) snapshot_ = server_->Snapshot();
    server_.reset();
    snapshot_.listening = false;
}

void RendezvousTuiApp::RestartServer() {
    StopServer();
    StartServer();
}

void RendezvousTuiApp::RefreshSnapshot() {
    if (server_) snapshot_ = server_->Snapshot();
}

void RendezvousTuiApp::OnLog(LogLevel level, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        logLines_.emplace_back(level, message);
        if (logLines_.size() > 2000) {
            logLines_.erase(logLines_.begin(), logLines_.begin() + 500);
        }
    }
    screen_.PostEvent(ftxui::Event::Custom);
}

void RendezvousTuiApp::StopTicker() {
    tickerRunning_.store(false);
    if (tickerThread_.joinable()) tickerThread_.join();
}

#include "tui_app.h"

#include <chrono>
#include <string>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

ftxui::Component RendezvousTuiApp::BuildDashboardTab() {
    using namespace ftxui;
    auto scroll = Slider("Scroll", &dashboardScroll_, 0, 100, 1);
    return Renderer(scroll, [this, scroll] {
        size_t clients = 0;
        for (const auto& room : snapshot_.rooms) clients += room.clients.size();
        const auto uptime = snapshot_.listening
            ? std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - serverStarted_).count()
            : 0;

        Elements rooms;
        for (const auto& room : snapshot_.rooms) {
            Elements rows;
            rows.push_back(hbox({
                text("Room  " + room.roomId) | bold | color(Color::Cyan),
                filler(),
                text(std::to_string(room.clients.size()) + " client(s)") | dim,
            }));
            rows.push_back(separator());
            rows.push_back(hbox({
                text("Client") | bold | size(WIDTH, EQUAL, 20),
                text("TUN IPv4") | bold | size(WIDTH, EQUAL, 16),
                text("Public endpoint") | bold | size(WIDTH, EQUAL, 23),
                text("State") | bold | size(WIDTH, EQUAL, 20),
                text("Idle") | bold,
            }));
            for (const auto& client : room.clients) {
                std::string state = "available";
                Color stateColor = Color::Green;
                if (client.nat4Joined) {
                    state = "NAT4 #" + std::to_string(client.nat4Round);
                    if (!client.pairedWith.empty()) state += " -> " + client.pairedWith;
                    stateColor = Color::Blue;
                } else if (!client.pairedWith.empty()) {
                    state = "paired: " + client.pairedWith;
                    stateColor = Color::Yellow;
                }
                rows.push_back(hbox({
                    text(client.nodeId) | size(WIDTH, EQUAL, 20),
                    text(client.tunIp) | size(WIDTH, EQUAL, 16),
                    text(client.endpoint) | size(WIDTH, EQUAL, 23),
                    text(state) | color(stateColor) | size(WIDTH, EQUAL, 20),
                    text(std::to_string(client.idleSeconds) + "s"),
                }));
            }
            rooms.push_back(vbox(std::move(rows)) | border);
        }
        if (rooms.empty()) {
            rooms.push_back(vbox({
                filler(),
                hbox({filler(), text(snapshot_.listening
                    ? "No active rooms. Waiting for clients..."
                    : "Server is not listening.") | dim, filler()}),
                filler(),
            }) | flex);
        }

        Elements header{
            hbox({
                text("Endpoint: ") | bold, text(snapshot_.endpoint),
                text("   Uptime: ") | bold, text(std::to_string(uptime) + "s"),
                filler(),
                text("Rooms ") | bold, text(std::to_string(snapshot_.rooms.size())),
                text("   Clients ") | bold, text(std::to_string(clients)),
                text("   Relays ") | bold,
                text(std::to_string(snapshot_.relay.activeSessions)),
            }),
            hbox({
                text("Datagrams ") | bold,
                text(std::to_string(snapshot_.receivedDatagrams)),
                text("   Messages ") | bold,
                text(std::to_string(snapshot_.controlMessages)),
                text("   Relay packets ") | bold,
                text(std::to_string(snapshot_.relay.forwardedDatagrams)),
                text("   Relay bytes ") | bold,
                text(std::to_string(snapshot_.relay.forwardedBytes)),
                filler(), scroll->Render() | size(WIDTH, EQUAL, 32),
            }),
        };
        if (!snapshot_.lastError.empty()) {
            header.push_back(text(snapshot_.lastError) | color(Color::Red));
        }
        header.push_back(separator());
        header.push_back(vbox(std::move(rooms))
            | focusPositionRelative(0.0f, static_cast<float>(dashboardScroll_) / 100.0f)
            | vscroll_indicator | frame | flex);
        return vbox(std::move(header)) | border | flex;
    });
}

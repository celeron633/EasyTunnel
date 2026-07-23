#include "tui_app.h"

#include <string>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

ftxui::Component RendezvousTuiApp::BuildRelayTab() {
    using namespace ftxui;
    auto scroll = Slider("Scroll", &relayScroll_, 0, 100, 1);
    return Renderer(scroll, [this, scroll] {
        Elements rows;
        rows.push_back(hbox({
            text("Active sessions ") | bold,
            text(std::to_string(snapshot_.relay.activeSessions)),
            text("   Received ") | bold,
            text(std::to_string(snapshot_.relay.receivedDatagrams)),
            text("   Forwarded ") | bold,
            text(std::to_string(snapshot_.relay.forwardedDatagrams)),
            text("   Bytes ") | bold,
            text(std::to_string(snapshot_.relay.forwardedBytes)),
            filler(),
            scroll->Render() | size(WIDTH, EQUAL, 32),
        }));
        rows.push_back(separator());

        if (snapshot_.relay.sessions.empty()) {
            rows.push_back(vbox({
                filler(),
                hbox({
                    filler(),
                    text(snapshot_.listening
                        ? "No active IPv4 relay sessions."
                        : "Server is not listening.") | dim,
                    filler(),
                }),
                filler(),
            }) | flex);
        } else {
            rows.push_back(hbox({
                text("Room") | bold | size(WIDTH, EQUAL, 16),
                text("Client") | bold | size(WIDTH, EQUAL, 20),
                text("Public endpoint") | bold | size(WIDTH, EQUAL, 23),
                text("Peer") | bold | size(WIDTH, EQUAL, 20),
                text("Port") | bold | size(WIDTH, EQUAL, 8),
                text("State") | bold | size(WIDTH, EQUAL, 12),
                text("Idle") | bold,
            }));
            for (const auto& session : snapshot_.relay.sessions) {
                for (int side = 0; side < 2; ++side) {
                    const auto& peer = session.peers[side];
                    const std::string state = session.ready
                        ? "relaying" : (peer.connected ? "joined" : "waiting");
                    const Color stateColor = session.ready
                        ? Color::Green
                        : (peer.connected ? Color::Yellow : Color::GrayDark);
                    rows.push_back(hbox({
                        text(session.roomId) | size(WIDTH, EQUAL, 16),
                        text(peer.nodeId) | size(WIDTH, EQUAL, 20),
                        text(peer.connected ? peer.endpoint : "N/A")
                            | size(WIDTH, EQUAL, 23),
                        text(session.peers[1 - side].nodeId)
                            | size(WIDTH, EQUAL, 20),
                        text(std::to_string(session.port))
                            | size(WIDTH, EQUAL, 8),
                        text(state) | color(stateColor)
                            | size(WIDTH, EQUAL, 12),
                        text(peer.connected
                            ? std::to_string(peer.idleSeconds) + "s" : "N/A"),
                    }));
                }
            }
        }

        return vbox(std::move(rows))
            | focusPositionRelative(
                0.0f, static_cast<float>(relayScroll_) / 100.0f)
            | vscroll_indicator | frame | border | flex;
    });
}

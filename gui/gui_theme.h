#pragma once

#include <QColor>
#include <QString>

#include "../tunnel_engine.h"

class QApplication;
class QWidget;

// Shared visual language for the GUI client: a flat light theme with one blue
// accent. The connection state pill, the TX/RX activity dots and the chart
// accents all come from here so the tabs and the status bar stay consistent.
namespace gui_theme {

// Surfaces: a light grey window behind white cards and fields.
inline const QColor kBackground(0xf4, 0xf5, 0xf7);
inline const QColor kSurface(0xff, 0xff, 0xff);
inline const QColor kSurfaceRaised(0xf1, 0xf3, 0xf6);
inline const QColor kBorder(0xdd, 0xe1, 0xe7);
inline const QColor kText(0x1f, 0x23, 0x28);
inline const QColor kMuted(0x6b, 0x72, 0x80);
inline const QColor kAccent(0x25, 0x63, 0xeb);

// Accents are one step darker than on a dark theme to stay legible on white.
inline const QColor kTx(0xdc, 0x26, 0x26);
inline const QColor kRx(0x16, 0xa3, 0x4a);
inline const QColor kLatency(0xd9, 0x77, 0x06);
inline const QColor kSuccess(0x16, 0xa3, 0x4a);
inline const QColor kFailure(0xdc, 0x26, 0x26);
inline const QColor kWarning(0xd9, 0x77, 0x06);

struct StateStyle {
    const char* label;
    QColor color;
};

inline StateStyle StyleFor(TunnelState state) {
    switch (state) {
        case TunnelState::Connected: return {"CONNECTED", kSuccess};
        case TunnelState::Connecting: return {"CONNECTING", kWarning};
        case TunnelState::Waiting: return {"WAITING", kAccent};
        case TunnelState::Error: return {"ERROR", kFailure};
        default: return {"DISCONNECTED", kMuted};
    }
}

// "color: #rrggbb;" fragment for label style sheets.
inline QString TextColorStyle(const QColor& color) {
    return QStringLiteral("color: %1;").arg(color.name());
}

// Fusion base, a flat proxy style for check boxes and the application style
// sheet.
void Apply(QApplication& app);

// Buttons carry a "variant" property the style sheet keys on: "primary" (accent
// fill), "danger" (red fill), "compact" (small padding) or empty for default.
void SetButtonVariant(QWidget* button, const char* variant);

}  // namespace gui_theme

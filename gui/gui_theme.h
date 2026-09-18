#pragma once

#include <QColor>
#include <QString>

#include "../tunnel_engine.h"

class QApplication;
class QWidget;

// Shared visual language for the GUI client: a flat dark theme with one blue
// accent. The connection state pill, the TX/RX activity dots and the chart
// accents all come from here so the tabs and the status bar stay consistent.
namespace gui_theme {

// Surfaces, from the window background up to raised controls.
inline const QColor kBackground(0x16, 0x18, 0x1d);
inline const QColor kSurface(0x1e, 0x21, 0x28);
inline const QColor kSurfaceRaised(0x26, 0x2a, 0x33);
inline const QColor kBorder(0x2e, 0x33, 0x3d);
inline const QColor kText(0xe6, 0xe8, 0xeb);
inline const QColor kMuted(0x8b, 0x93, 0xa1);
inline const QColor kAccent(0x3b, 0x82, 0xf6);

inline const QColor kTx(0xf0, 0x52, 0x52);
inline const QColor kRx(0x34, 0xd3, 0x74);
inline const QColor kLatency(0xf5, 0xb7, 0x2b);
inline const QColor kSuccess(0x34, 0xd3, 0x74);
inline const QColor kFailure(0xef, 0x44, 0x44);
inline const QColor kWarning(0xf5, 0xb7, 0x2b);

struct StateStyle {
    const char* label;
    QColor color;
};

inline StateStyle StyleFor(TunnelState state) {
    switch (state) {
        case TunnelState::Connected: return {"CONNECTED", kSuccess};
        case TunnelState::Connecting: return {"CONNECTING", kWarning};
        case TunnelState::Waiting: return {"WAITING", QColor(0x60, 0xa5, 0xfa)};
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

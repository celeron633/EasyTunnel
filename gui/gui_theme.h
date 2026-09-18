#pragma once

#include <QColor>
#include <QString>

#include "../tunnel_engine.h"

class QApplication;
class QWidget;

// Shared visual language for the GUI client: a restrained light desktop theme
// in the spirit of Windows 11 (neutral greys, one blue accent, small radii).
// The connection state chip, the TX/RX activity dots and the chart accents all
// come from here so the tabs and the status bar stay consistent.
namespace gui_theme {

// Neutral surfaces and lines.
inline const QColor kWindow(0xf3, 0xf3, 0xf3);
inline const QColor kSurface(0xff, 0xff, 0xff);
inline const QColor kSurfaceAlt(0xf9, 0xf9, 0xf9);
inline const QColor kBorder(0xe0, 0xe0, 0xe0);
inline const QColor kBorderStrong(0xc4, 0xc4, 0xc4);
inline const QColor kText(0x1b, 0x1b, 0x1b);
inline const QColor kMuted(0x60, 0x60, 0x60);

// Single accent (Windows blue) and its tint for selections.
inline const QColor kAccent(0x00, 0x67, 0xc0);
inline const QColor kAccentTint(0xdb, 0xea, 0xf8);

// Data and status colours from the Windows palette.
inline const QColor kTx(0xd1, 0x34, 0x38);
inline const QColor kRx(0x10, 0x7c, 0x10);
inline const QColor kLatency(0xca, 0x50, 0x10);
inline const QColor kSuccess(0x10, 0x7c, 0x10);
inline const QColor kFailure(0xc4, 0x2b, 0x1c);
inline const QColor kWarning(0x9d, 0x5d, 0x00);

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

// Fusion base with flat check boxes and roomier input controls, plus the
// application style sheet. Input controls are deliberately left out of the
// style sheet so Fusion keeps drawing their spin arrows and drop-down buttons.
void Apply(QApplication& app);

// Buttons carry a "variant" property the style sheet keys on: "primary" (accent
// fill), "danger" (red fill), "compact" (small, for table rows) or empty.
void SetButtonVariant(QWidget* button, const char* variant);

// Lets a QMenu show the style sheet's rounded corners.
void PrepareMenu(QWidget* menu);

}  // namespace gui_theme

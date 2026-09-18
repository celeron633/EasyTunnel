#pragma once

#include <QColor>
#include <QString>

#include "../tunnel_engine.h"

class QApplication;
class QWidget;

// Shared visual language for the GUI client: Material Design 3, light scheme
// generated from a blue seed (#0061A4). The connection state chip, the TX/RX
// activity dots and the chart accents all come from here so the tabs and the
// status bar stay consistent.
namespace gui_theme {

// MD3 colour roles.
inline const QColor kPrimary(0x00, 0x61, 0xa4);
inline const QColor kOnPrimary(0xff, 0xff, 0xff);
inline const QColor kPrimaryContainer(0xd1, 0xe4, 0xff);
inline const QColor kSecondaryContainer(0xd7, 0xe3, 0xf7);
inline const QColor kOnSecondaryContainer(0x10, 0x1c, 0x2b);
inline const QColor kError(0xba, 0x1a, 0x1a);
inline const QColor kSurface(0xf8, 0xf9, 0xff);
inline const QColor kSurfaceContainerLowest(0xff, 0xff, 0xff);
inline const QColor kSurfaceContainerLow(0xf2, 0xf3, 0xfa);
inline const QColor kSurfaceContainer(0xec, 0xee, 0xf4);
inline const QColor kSurfaceContainerHigh(0xe6, 0xe8, 0xee);
inline const QColor kSurfaceContainerHighest(0xe1, 0xe2, 0xe8);
inline const QColor kOnSurface(0x19, 0x1c, 0x20);
inline const QColor kOnSurfaceVariant(0x42, 0x47, 0x4e);
inline const QColor kOutline(0x73, 0x77, 0x7f);
inline const QColor kOutlineVariant(0xc3, 0xc7, 0xcf);
inline const QColor kInverseSurface(0x2e, 0x31, 0x35);
inline const QColor kInverseOnSurface(0xef, 0xf0, 0xf7);

// Short names used by the widgets.
inline const QColor& kAccent = kPrimary;
inline const QColor& kText = kOnSurface;
inline const QColor& kMuted = kOnSurfaceVariant;
inline const QColor& kBorder = kOutlineVariant;

// Data and status accents, picked to stay legible on the light surfaces.
inline const QColor kTx(0xc6, 0x28, 0x28);
inline const QColor kRx(0x1b, 0x87, 0x3f);
inline const QColor kLatency(0xb2, 0x6a, 0x00);
inline const QColor kSuccess(0x1b, 0x87, 0x3f);
inline const QColor kFailure = kError;
inline const QColor kWarning(0xb2, 0x6a, 0x00);

struct StateStyle {
    const char* label;
    QColor color;
};

inline StateStyle StyleFor(TunnelState state) {
    switch (state) {
        case TunnelState::Connected: return {"CONNECTED", kSuccess};
        case TunnelState::Connecting: return {"CONNECTING", kWarning};
        case TunnelState::Waiting: return {"WAITING", kPrimary};
        case TunnelState::Error: return {"ERROR", kError};
        default: return {"DISCONNECTED", kOnSurfaceVariant};
    }
}

// "color: #rrggbb;" fragment for label style sheets.
inline QString TextColorStyle(const QColor& color) {
    return QStringLiteral("color: %1;").arg(color.name());
}

// Fusion base with MD3 check boxes and switches, plus the application style
// sheet.
void Apply(QApplication& app);

// Buttons carry a "variant" property the style sheet keys on: "primary" (filled),
// "danger" (filled error), "text" (text button) or empty for a tonal button.
void SetButtonVariant(QWidget* button, const char* variant);

// Lets a QMenu show the style sheet's rounded corners.
void PrepareMenu(QWidget* menu);

// Renders a QCheckBox as an MD3 switch; use for on/off settings.
void MakeSwitch(QWidget* checkBox);

}  // namespace gui_theme

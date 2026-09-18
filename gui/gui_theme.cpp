#include "gui_theme.h"

#include <algorithm>
#include <utility>

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QWidget>

namespace gui_theme {
namespace {
constexpr int kInputHeight = 28;

// Fusion does the drawing; this adjusts only what the style sheet cannot
// without taking over the whole control: flat check boxes, taller inputs and
// list-style combo popups.
class DesktopStyle : public QProxyStyle {
public:
    DesktopStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override {
        if (metric == PM_IndicatorWidth || metric == PM_IndicatorHeight) return 16;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption* option,
                           const QSize& contentsSize, const QWidget* widget) const override {
        QSize size = QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
        if (type == CT_LineEdit || type == CT_SpinBox || type == CT_ComboBox) {
            size.setHeight(std::max(size.height(), kInputHeight));
        }
        return size;
    }

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* returnData) const override {
        // A plain list popup instead of Fusion's menu-style popup, which is only
        // a few rows tall and scrolls with arrow buttons.
        if (hint == SH_ComboBox_Popup) return 0;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget) const override {
        if (element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck
            || element == PE_IndicatorMenuCheckMark) {
            DrawCheckBox(option, painter);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

private:
    // 16 px, 3 px corners: white with a grey outline, or accent with a tick.
    static void DrawCheckBox(const QStyleOption* option, QPainter* painter) {
        const bool checked = option->state & State_On;
        const bool hovered = option->state & State_MouseOver;
        const QRectF box = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (!(option->state & State_Enabled)) painter->setOpacity(0.4);
        if (checked) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(hovered ? kAccent.lighter(112) : kAccent);
            painter->drawRoundedRect(box, 3.0, 3.0);
            QPainterPath tick;
            tick.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.52);
            tick.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.70);
            tick.lineTo(box.left() + box.width() * 0.77, box.top() + box.height() * 0.32);
            painter->setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(tick);
        } else {
            painter->setPen(QPen(hovered ? kAccent : QColor(0x8a, 0x8a, 0x8a), 1.0));
            painter->setBrush(kSurface);
            painter->drawRoundedRect(box, 3.0, 3.0);
        }
        painter->restore();
    }
};

// Line edits, spin boxes, combo boxes, check boxes and the log view have no
// rules here on purpose: styling their frame makes Qt drop the native spin
// arrows and drop-down button.
QString StyleSheet() {
    QString sheet = QStringLiteral(R"(
QToolTip {
    color: @text; background: @surface; border: 1px solid @borderStrong;
    padding: 4px 6px;
}

QTabWidget::pane { border: none; border-top: 1px solid @border; top: -1px; }
QTabBar::tab {
    background: transparent; color: @muted; border: none;
    border-bottom: 2px solid transparent; padding: 8px 16px; margin-right: 4px;
}
QTabBar::tab:hover { color: @text; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; font-weight: 600; }

QGroupBox {
    background: @surface; border: 1px solid @border; border-radius: 6px;
    margin-top: 4px; padding: 30px 4px 2px 4px; font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: border; subcontrol-position: top left;
    left: 12px; top: 9px; color: @text;
}

QPushButton {
    background: @surface; color: @text; border: 1px solid @borderStrong;
    border-radius: 4px; padding: 5px 16px; min-height: 18px;
}
QPushButton:hover { background: #f5f5f5; }
QPushButton:pressed { background: #ebebeb; }
QPushButton:disabled { color: #a0a0a0; background: #f5f5f5; border-color: @border; }
QPushButton[variant="primary"] { background: @accent; border-color: @accent; color: white; }
QPushButton[variant="primary"]:hover { background: #1975c5; border-color: #1975c5; }
QPushButton[variant="primary"]:pressed { background: #005ba8; }
QPushButton[variant="danger"] { background: #c42b1c; border-color: #c42b1c; color: white; }
QPushButton[variant="danger"]:hover { background: #b02719; border-color: #b02719; }
QPushButton[variant="primary"]:disabled, QPushButton[variant="danger"]:disabled {
    color: #a0a0a0; background: #f5f5f5; border-color: @border;
}
QPushButton[variant="compact"] { padding: 2px 10px; min-height: 0; }

QTableView {
    background: @surface; alternate-background-color: @surfaceAlt;
    border: 1px solid @border; border-radius: 4px; gridline-color: transparent;
    selection-background-color: @accentTint; selection-color: @text; outline: none;
}
QTableView::item { padding: 0 6px; border: none; }
QTableView::item:hover { background: #f0f0f0; }
QTableView::item:selected { background: @accentTint; color: @text; }
QHeaderView { background: transparent; }
QHeaderView::section {
    background: @surfaceAlt; color: @muted; border: none;
    border-bottom: 1px solid @border; padding: 6px; font-weight: 600;
}
QTableCornerButton::section { background: @surfaceAlt; border: none; }

QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: @borderStrong; border-radius: 3px; }
QScrollBar::handle:vertical { min-height: 24px; }
QScrollBar::handle:horizontal { min-width: 24px; }
QScrollBar::handle:hover { background: #a8a8a8; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QStatusBar { background: @window; border-top: 1px solid @border; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 2px 6px; }

/* Rounded corners need a translucent frameless popup; see PrepareMenu(). */
QMenu {
    background: @surface; border: 1px solid @borderStrong; border-radius: 6px;
    padding: 4px;
}
QMenu::item {
    background: transparent; padding: 6px 28px 6px 10px; min-width: 150px;
    border-radius: 4px;
}
QMenu::item:selected { background: #f0f0f0; color: @text; }
QMenu::item:disabled { color: #a0a0a0; }
QMenu::separator { height: 1px; background: @border; margin: 4px 6px; }
)");
    // Longest names first so no token is a prefix of one replaced later.
    const std::pair<const char*, QColor> tokens[] = {
        {"@borderStrong", kBorderStrong},
        {"@border", kBorder},
        {"@surfaceAlt", kSurfaceAlt},
        {"@surface", kSurface},
        {"@accentTint", kAccentTint},
        {"@accent", kAccent},
        {"@window", kWindow},
        {"@muted", kMuted},
        {"@text", kText},
    };
    for (const auto& [token, color] : tokens) {
        sheet.replace(QLatin1String(token), color.name());
    }
    return sheet;
}

}  // namespace

void Apply(QApplication& app) {
    app.setStyle(new DesktopStyle());

    QPalette palette;
    palette.setColor(QPalette::Window, kWindow);
    palette.setColor(QPalette::WindowText, kText);
    palette.setColor(QPalette::Base, kSurface);
    palette.setColor(QPalette::AlternateBase, kSurfaceAlt);
    palette.setColor(QPalette::ToolTipBase, kSurface);
    palette.setColor(QPalette::ToolTipText, kText);
    palette.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8a, 0x8a));
    palette.setColor(QPalette::Text, kText);
    palette.setColor(QPalette::Button, kSurface);
    palette.setColor(QPalette::ButtonText, kText);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Highlight, kAccent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, kAccent);
    palette.setColor(QPalette::Light, kSurface);
    palette.setColor(QPalette::Midlight, kSurfaceAlt);
    palette.setColor(QPalette::Mid, kBorder);
    palette.setColor(QPalette::Dark, kBorderStrong);
    const QColor disabledText(0xa0, 0xa0, 0xa0);
    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, disabledText);
    }
    palette.setColor(QPalette::Disabled, QPalette::Base, kSurfaceAlt);
    app.setPalette(palette);
    app.setStyleSheet(StyleSheet());
}

void SetButtonVariant(QWidget* button, const char* variant) {
    if (button->property("variant").toString() == QLatin1String(variant)) return;
    button->setProperty("variant", QString::fromLatin1(variant));
    // Dynamic properties only take effect in the style sheet after a re-polish.
    button->style()->unpolish(button);
    button->style()->polish(button);
}

void PrepareMenu(QWidget* menu) {
    // Without a translucent frameless window the rounded corners from the
    // style sheet are painted over a square, shadowed rectangle.
    menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint
                         | Qt::NoDropShadowWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground);
}

}  // namespace gui_theme

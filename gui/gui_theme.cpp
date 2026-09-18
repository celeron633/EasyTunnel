#include "gui_theme.h"

#include <algorithm>

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

// Check boxes are painted here rather than in the style sheet: a style sheet
// indicator needs image files for the check mark, while this stays vector.
class FlatStyle : public QProxyStyle {
public:
    FlatStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override {
        if (metric == PM_IndicatorWidth || metric == PM_IndicatorHeight) return 16;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget) const override {
        if (element == PE_FrameFocusRect) return;
        if (element != PE_IndicatorCheckBox && element != PE_IndicatorItemViewItemCheck) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        const bool checked = option->state & State_On;
        const bool enabled = option->state & State_Enabled;
        const bool hovered = option->state & State_MouseOver;
        const QRectF box = QRectF(option->rect).adjusted(1.0, 1.0, -1.0, -1.0);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (!enabled) painter->setOpacity(0.45);
        if (checked) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(hovered ? kAccent.lighter(115) : kAccent);
        } else {
            painter->setPen(QPen(hovered ? kAccent : QColor(0x4a, 0x51, 0x60), 1.3));
            painter->setBrush(kSurfaceRaised);
        }
        painter->drawRoundedRect(box, 4.0, 4.0);
        if (checked) {
            QPainterPath mark;
            mark.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.52);
            mark.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.70);
            mark.lineTo(box.left() + box.width() * 0.77, box.top() + box.height() * 0.32);
            painter->setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(mark);
        }
        painter->restore();
    }
};

QString StyleSheet() {
    QString sheet = QStringLiteral(R"(
QWidget { color: @text; }
QMainWindow, QDialog, QMessageBox { background: @bg; }
QToolTip {
    color: @text; background: @raised; border: 1px solid @border;
    border-radius: 4px; padding: 4px 6px;
}

QTabWidget::pane { border: none; border-top: 1px solid @border; top: -1px; }
QTabBar::tab {
    background: transparent; color: @muted; border: none;
    border-bottom: 2px solid transparent; padding: 8px 18px; margin-right: 2px;
    font-weight: 600;
}
QTabBar::tab:hover { color: @text; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }

QGroupBox {
    background: @surface; border: 1px solid @border; border-radius: 8px;
    margin-top: 4px; padding: 22px 2px 0 2px;
}
QGroupBox::title {
    subcontrol-origin: border; subcontrol-position: top left;
    left: 12px; top: 8px; color: @muted; font-weight: 600;
}

QPushButton {
    background: @raised; color: @text; border: 1px solid @border;
    border-radius: 6px; padding: 6px 14px;
}
QPushButton:hover { background: #2f3440; border-color: #3a404c; }
QPushButton:pressed { background: #232730; }
QPushButton:disabled { color: #5c6370; background: @surface; border-color: @border; }
QPushButton[variant="primary"] { background: @accent; border-color: @accent; color: white; }
QPushButton[variant="primary"]:hover { background: #4c8df7; border-color: #4c8df7; }
QPushButton[variant="primary"]:pressed { background: #2f6fd8; }
QPushButton[variant="danger"] { background: #dc3c3c; border-color: #dc3c3c; color: white; }
QPushButton[variant="danger"]:hover { background: #e85555; border-color: #e85555; }
QPushButton[variant="primary"]:disabled, QPushButton[variant="danger"]:disabled {
    background: @surface; border-color: @border; color: #5c6370;
}
QPushButton[variant="compact"] { padding: 2px 10px; border-radius: 4px; }

QLineEdit, QAbstractSpinBox, QComboBox {
    background: @raised; border: 1px solid @border; border-radius: 6px;
    padding: 5px 8px; selection-background-color: @accent;
}
QLineEdit:hover, QAbstractSpinBox:hover, QComboBox:hover { border-color: #3a404c; }
QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus { border-color: @accent; }
QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled {
    color: #5c6370; background: @surface;
}
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background: @raised; border: 1px solid @border; outline: none;
    selection-background-color: @accent; padding: 2px;
}

QCheckBox { spacing: 8px; }

QTableView {
    background: @surface; alternate-background-color: #22262e;
    border: 1px solid @border; border-radius: 6px; gridline-color: transparent;
    selection-background-color: rgba(59, 130, 246, 0.28); selection-color: @text;
    outline: none;
}
QTableView::item { padding: 0 6px; border: none; }
QHeaderView::section {
    background: @surface; color: @muted; border: none;
    border-bottom: 1px solid @border; padding: 6px 8px; font-weight: 600;
}
QTableCornerButton::section { background: @surface; border: none; }

QPlainTextEdit {
    background: #121418; border: 1px solid @border; border-radius: 6px;
    padding: 4px; selection-background-color: @accent;
}

QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: #3a404c; border-radius: 3px; }
QScrollBar::handle:vertical { min-height: 24px; }
QScrollBar::handle:horizontal { min-width: 24px; }
QScrollBar::handle:hover { background: #4a5160; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QStatusBar { background: @surface; border-top: 1px solid @border; }
QStatusBar::item { border: none; }

QMenu { background: @surface; border: 1px solid @border; border-radius: 6px; padding: 4px; }
QMenu::item { padding: 6px 22px; border-radius: 4px; }
QMenu::item:selected { background: @accent; color: white; }
QMenu::separator { height: 1px; background: @border; margin: 4px 6px; }
)");
    sheet.replace(QStringLiteral("@bg"), kBackground.name());
    sheet.replace(QStringLiteral("@surface"), kSurface.name());
    sheet.replace(QStringLiteral("@raised"), kSurfaceRaised.name());
    sheet.replace(QStringLiteral("@border"), kBorder.name());
    sheet.replace(QStringLiteral("@text"), kText.name());
    sheet.replace(QStringLiteral("@muted"), kMuted.name());
    sheet.replace(QStringLiteral("@accent"), kAccent.name());
    return sheet;
}

}  // namespace

void Apply(QApplication& app) {
    app.setStyle(new FlatStyle());

    QFont font = QApplication::font();
    font.setPointSizeF(std::max(font.pointSizeF(), 9.5));
    app.setFont(font);

    const QColor disabledText(0x5c, 0x63, 0x70);
    QPalette palette;
    palette.setColor(QPalette::Window, kBackground);
    palette.setColor(QPalette::WindowText, kText);
    palette.setColor(QPalette::Base, kSurfaceRaised);
    palette.setColor(QPalette::AlternateBase, kSurface);
    palette.setColor(QPalette::ToolTipBase, kSurfaceRaised);
    palette.setColor(QPalette::ToolTipText, kText);
    palette.setColor(QPalette::PlaceholderText, disabledText);
    palette.setColor(QPalette::Text, kText);
    palette.setColor(QPalette::Button, kSurfaceRaised);
    palette.setColor(QPalette::ButtonText, kText);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Highlight, kAccent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, kAccent);
    palette.setColor(QPalette::Mid, kBorder);
    palette.setColor(QPalette::Dark, QColor(0x10, 0x12, 0x16));
    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, disabledText);
    }
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

}  // namespace gui_theme

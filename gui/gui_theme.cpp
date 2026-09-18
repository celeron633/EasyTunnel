#include "gui_theme.h"

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
constexpr const char* kSwitchProperty = "md3Switch";

bool IsSwitch(const QWidget* widget) {
    return widget && widget->property(kSwitchProperty).toBool();
}

// MD3 check boxes and switches are painted here rather than in the style
// sheet: style sheet indicators need image files, while this stays vector.
class MaterialStyle : public QProxyStyle {
public:
    MaterialStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override {
        if (metric == PM_IndicatorWidth) return IsSwitch(widget) ? 52 : 18;
        if (metric == PM_IndicatorHeight) return IsSwitch(widget) ? 32 : 18;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget) const override {
        if (element == PE_FrameFocusRect) return;
        if (element == PE_IndicatorCheckBox && IsSwitch(widget)) {
            DrawSwitch(option, painter);
            return;
        }
        if (element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck) {
            DrawCheckBox(option, painter);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

private:
    // 18 px box, 2 px corners; filled with primary and a white tick when on.
    static void DrawCheckBox(const QStyleOption* option, QPainter* painter) {
        const bool checked = option->state & State_On;
        const QRectF box = QRectF(option->rect).adjusted(1.0, 1.0, -1.0, -1.0);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (!(option->state & State_Enabled)) painter->setOpacity(0.38);
        if (checked) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(kPrimary);
            painter->drawRoundedRect(box, 2.0, 2.0);
            QPainterPath tick;
            tick.moveTo(box.left() + box.width() * 0.22, box.top() + box.height() * 0.52);
            tick.lineTo(box.left() + box.width() * 0.42, box.top() + box.height() * 0.72);
            tick.lineTo(box.left() + box.width() * 0.78, box.top() + box.height() * 0.30);
            painter->setPen(QPen(kOnPrimary, 2.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(tick);
        } else {
            const QColor border = (option->state & State_MouseOver) ? kOnSurface
                                                                    : kOnSurfaceVariant;
            painter->setPen(QPen(border, 2.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(box.adjusted(1.0, 1.0, -1.0, -1.0), 2.0, 2.0);
        }
        painter->restore();
    }

    // 52x32 track. Off: outlined track with a small outline-coloured thumb.
    // On: primary track with a larger white thumb on the right.
    static void DrawSwitch(const QStyleOption* option, QPainter* painter) {
        const bool checked = option->state & State_On;
        const bool hovered = option->state & State_MouseOver;
        const QRectF track = QRectF(option->rect).adjusted(1.0, 1.0, -1.0, -1.0);
        const double radius = track.height() / 2.0;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (!(option->state & State_Enabled)) painter->setOpacity(0.38);
        if (checked) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(kPrimary);
            painter->drawRoundedRect(track, radius, radius);
            const double thumb = hovered ? 13.0 : 12.0;
            painter->setBrush(kOnPrimary);
            painter->drawEllipse(QPointF(track.right() - radius, track.center().y()),
                                 thumb, thumb);
        } else {
            painter->setPen(QPen(kOutline, 2.0));
            painter->setBrush(kSurfaceContainerHighest);
            painter->drawRoundedRect(track.adjusted(1.0, 1.0, -1.0, -1.0),
                                     radius - 1.0, radius - 1.0);
            painter->setPen(Qt::NoPen);
            painter->setBrush(hovered ? kOnSurfaceVariant : kOutline);
            painter->drawEllipse(QPointF(track.left() + radius, track.center().y()), 8.0, 8.0);
        }
        painter->restore();
    }
};

QString StyleSheet() {
    QString sheet = QStringLiteral(R"(
QWidget { color: @onSurface; }
QMainWindow, QDialog, QMessageBox { background: @surface; }
QToolTip {
    color: @inverseOnSurface; background: @inverseSurface; border: none;
    border-radius: 4px; padding: 4px 8px;
}

/* Primary tabs */
QTabWidget::pane { border: none; border-top: 1px solid @outlineVariant; top: -1px; }
QTabBar::tab {
    background: transparent; color: @onSurfaceVariant; border: none;
    border-bottom: 3px solid transparent; padding: 12px 22px 9px 22px;
    min-width: 72px; font-weight: 600;
}
QTabBar::tab:hover { color: @onSurface; background: rgba(25, 28, 32, 0.06); }
QTabBar::tab:selected { color: @primary; border-bottom: 3px solid @primary; }

/* Filled cards; the group title is the card headline. */
QGroupBox {
    background: @surfaceContainerLow; border: none; border-radius: 12px;
    margin-top: 4px; padding: 34px 6px 4px 6px; font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: border; subcontrol-position: top left;
    left: 16px; top: 12px; color: @onSurface;
}

/* Buttons: tonal by default, filled for the main action, text for minor ones. */
QPushButton {
    background: @secondaryContainer; color: @onSecondaryContainer; border: none;
    /* Qt drops rounding whose radius reaches half the height; stay below. */
    min-height: 20px; border-radius: 18px; padding: 10px 24px; font-weight: 600;
}
QPushButton:hover { background: #cad6ea; }
QPushButton:pressed { background: #c2cee2; }
QPushButton[variant="primary"] { background: @primary; color: @onPrimary; }
QPushButton[variant="primary"]:hover { background: #1a6fae; }
QPushButton[variant="primary"]:pressed { background: #2b79b4; }
QPushButton[variant="danger"] { background: @error; color: white; }
QPushButton[variant="danger"]:hover { background: #c13030; }
QPushButton[variant="danger"]:pressed { background: #c63d3d; }
QPushButton[variant="text"] {
    background: transparent; color: @primary; border-radius: 14px; padding: 6px 12px;
}
QPushButton[variant="text"]:hover { background: rgba(0, 97, 164, 0.08); }
QPushButton[variant="text"]:pressed { background: rgba(0, 97, 164, 0.12); }
QPushButton:disabled, QPushButton[variant="primary"]:disabled,
QPushButton[variant="danger"]:disabled {
    background: rgba(25, 28, 32, 0.12); color: rgba(25, 28, 32, 0.38);
}
QPushButton[variant="text"]:disabled { background: transparent; color: rgba(25, 28, 32, 0.38); }

/* Outlined text fields */
QLineEdit, QAbstractSpinBox, QComboBox {
    background: transparent; border: 1px solid @outline; border-radius: 4px;
    padding: 8px 12px; selection-background-color: @primaryContainer;
    selection-color: @onSurface;
}
QLineEdit:hover, QAbstractSpinBox:hover, QComboBox:hover { border-color: @onSurface; }
QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus {
    border: 2px solid @primary; padding: 7px 11px;
}
QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled {
    border-color: rgba(25, 28, 32, 0.12); color: rgba(25, 28, 32, 0.38);
}
QComboBox::drop-down { border: none; width: 28px; }
QComboBox QAbstractItemView {
    background: @surfaceContainer; border: none; border-radius: 4px; outline: none;
    selection-background-color: @secondaryContainer;
    selection-color: @onSecondaryContainer; padding: 4px 0;
}

QCheckBox { spacing: 12px; }

/* Lists */
QTableView {
    background: transparent; alternate-background-color: transparent;
    border: none; gridline-color: transparent; outline: none;
    selection-background-color: @secondaryContainer;
    selection-color: @onSecondaryContainer;
}
QTableView::item { padding: 0 8px; border: none; }
QTableView::item:hover { background: rgba(25, 28, 32, 0.06); }
QTableView::item:selected { background: @secondaryContainer; color: @onSecondaryContainer; }
QHeaderView { background: transparent; }
QHeaderView::section {
    background: transparent; color: @onSurfaceVariant; border: none;
    border-bottom: 1px solid @outlineVariant; padding: 8px; font-weight: 600;
}
QTableCornerButton::section { background: transparent; border: none; }

QPlainTextEdit {
    background: @surfaceContainerLow; border: none; border-radius: 12px;
    padding: 8px; selection-background-color: @primaryContainer;
    selection-color: @onSurface;
}

QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 2px; }
QScrollBar::handle { background: rgba(25, 28, 32, 0.24); border-radius: 4px; }
QScrollBar::handle:vertical { min-height: 32px; }
QScrollBar::handle:horizontal { min-width: 32px; }
QScrollBar::handle:hover { background: rgba(25, 28, 32, 0.38); }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QStatusBar { background: @surfaceContainer; color: @onSurfaceVariant; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 4px 8px; }

QMenu { background: @surfaceContainer; border: none; border-radius: 4px; padding: 8px 0; }
QMenu::item { padding: 10px 28px 10px 16px; }
QMenu::item:selected { background: rgba(25, 28, 32, 0.08); color: @onSurface; }
QMenu::separator { height: 1px; background: @outlineVariant; margin: 8px 0; }
)");
    // Longest names first so no token is a prefix of one replaced later.
    const std::pair<const char*, QColor> roles[] = {
        {"@onSecondaryContainer", kOnSecondaryContainer},
        {"@secondaryContainer", kSecondaryContainer},
        {"@primaryContainer", kPrimaryContainer},
        {"@surfaceContainerLow", kSurfaceContainerLow},
        {"@surfaceContainer", kSurfaceContainer},
        {"@inverseOnSurface", kInverseOnSurface},
        {"@inverseSurface", kInverseSurface},
        {"@onSurfaceVariant", kOnSurfaceVariant},
        {"@onSurface", kOnSurface},
        {"@outlineVariant", kOutlineVariant},
        {"@outline", kOutline},
        {"@onPrimary", kOnPrimary},
        {"@primary", kPrimary},
        {"@surface", kSurface},
        {"@error", kError},
    };
    for (const auto& [token, color] : roles) {
        sheet.replace(QLatin1String(token), color.name());
    }
    return sheet;
}

}  // namespace

void Apply(QApplication& app) {
    app.setStyle(new MaterialStyle());

    // MD3 specifies Roboto; fall back to the platform UI fonts (with CJK).
    QFont font = QApplication::font();
    font.setFamilies({QStringLiteral("Roboto"), QStringLiteral("Segoe UI"),
                      QStringLiteral("Microsoft YaHei UI"), font.family()});
    font.setPointSizeF(10.0);
    app.setFont(font);

    const QColor disabledText(0xa0, 0xa3, 0xa8);
    QPalette palette;
    palette.setColor(QPalette::Window, kSurface);
    palette.setColor(QPalette::WindowText, kOnSurface);
    palette.setColor(QPalette::Base, kSurfaceContainerLowest);
    palette.setColor(QPalette::AlternateBase, kSurfaceContainerLow);
    palette.setColor(QPalette::ToolTipBase, kInverseSurface);
    palette.setColor(QPalette::ToolTipText, kInverseOnSurface);
    palette.setColor(QPalette::PlaceholderText, kOnSurfaceVariant);
    palette.setColor(QPalette::Text, kOnSurface);
    palette.setColor(QPalette::Button, kSecondaryContainer);
    palette.setColor(QPalette::ButtonText, kOnSecondaryContainer);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Highlight, kPrimary);
    palette.setColor(QPalette::HighlightedText, kOnPrimary);
    palette.setColor(QPalette::Link, kPrimary);
    palette.setColor(QPalette::Mid, kOutlineVariant);
    palette.setColor(QPalette::Dark, kOutline);
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

void MakeSwitch(QWidget* checkBox) {
    checkBox->setProperty(kSwitchProperty, true);
    checkBox->updateGeometry();
    checkBox->update();
}

}  // namespace gui_theme

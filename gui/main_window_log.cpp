#include "main_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include "../log.h"
#include "gui_theme.h"

namespace {
constexpr int kMaxLogLines = 2000;
}  // namespace

QWidget* MainWindow::BuildLogTab() {
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);

    auto* toolbar = new QHBoxLayout();
    auto* copyButton = new QPushButton(QStringLiteral("Copy all"), tab);
    copyButton->setToolTip(QStringLiteral("Copy all in-memory log lines to the clipboard"));
    auto* clearButton = new QPushButton(QStringLiteral("Clear"), tab);
    logAutoScroll_ = new QCheckBox(QStringLiteral("Auto-scroll"), tab);
    logAutoScroll_->setChecked(true);
    logLineCount_ = new QLabel(tab);
    logLineCount_->setStyleSheet(gui_theme::TextColorStyle(gui_theme::kMuted));
    toolbar->addWidget(copyButton);
    toolbar->addWidget(clearButton);
    toolbar->addWidget(logAutoScroll_);
    toolbar->addStretch(1);
    toolbar->addWidget(logLineCount_);
    layout->addLayout(toolbar);

    logView_ = new QPlainTextEdit(tab);
    logView_->setReadOnly(true);
    logView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    logView_->setMaximumBlockCount(kMaxLogLines);
    logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(logView_, 1);

    const std::string filePath = GetLogFilePath();
    auto* fileLabel = new QLabel(QStringLiteral("File: %1").arg(
        filePath.empty() ? QStringLiteral("unavailable") : QString::fromStdString(filePath)), tab);
    fileLabel->setStyleSheet(gui_theme::TextColorStyle(gui_theme::kMuted));
    fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(fileLabel);

    connect(copyButton, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(logView_->toPlainText());
    });
    connect(clearButton, &QPushButton::clicked, this, [this] {
        logView_->clear();
        UpdateLogLineCount();
    });
    connect(logAutoScroll_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            logView_->verticalScrollBar()->setValue(logView_->verticalScrollBar()->maximum());
        }
    });
    UpdateLogLineCount();
    return tab;
}

// blockCount() is 1 for both an empty document and a single line, so the
// count is refreshed explicitly after every change instead of on its signal.
void MainWindow::UpdateLogLineCount() {
    const int lines = logView_->document()->isEmpty()
        ? 0 : logView_->document()->blockCount();
    logLineCount_->setText(QStringLiteral("%1 lines").arg(lines));
}

void MainWindow::DrainLog() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (pendingLogLines_.empty()) return;
        lines.swap(pendingLogLines_);
    }
    QString text;
    for (const std::string& line : lines) {
        if (!text.isEmpty()) text += '\n';
        text += QString::fromStdString(line);
    }
    QScrollBar* scrollBar = logView_->verticalScrollBar();
    const int previousPosition = scrollBar->value();
    logView_->appendPlainText(text);
    scrollBar->setValue(logAutoScroll_->isChecked() ? scrollBar->maximum()
                                                    : previousPosition);
    UpdateLogLineCount();
}

// Called from any thread; the UI tick appends the buffered lines.
void MainWindow::OnLog(LogLevel /*level*/, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex_);
    pendingLogLines_.push_back(message);
    if (pendingLogLines_.size() > kMaxLogLines) {
        pendingLogLines_.erase(pendingLogLines_.begin(),
                               pendingLogLines_.end() - kMaxLogLines);
    }
}

#pragma once

#include <string>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

// Shared visual language for the client TUI. Every tab draws its controls
// through these helpers so buttons, section titles and label/value rows keep
// the same width and decoration wherever they appear.
namespace tui_theme {

// Widest label used by the settings rows. Keeping it here lets the two
// settings columns line up without repeating the constant per row.
constexpr int kLabelWidth = 24;

// Buttons render as a single compact "[Label]" cell instead of the FTXUI
// default animated block, which is what made the previous layout look noisy.
inline ftxui::ButtonOption FlatButton() {
    ftxui::ButtonOption option = ftxui::ButtonOption::Simple();
    option.transform = [](const ftxui::EntryState& state) {
        ftxui::Element label = ftxui::text("[" + state.label + "]");
        if (state.focused) label = label | ftxui::inverted;
        return label;
    };
    return option;
}

// Statistics cells cycle their unit when activated. They must read as a value,
// not as a button, so only focus is highlighted.
inline ftxui::ButtonOption ValueButton() {
    ftxui::ButtonOption option = ftxui::ButtonOption::Simple();
    option.transform = [](const ftxui::EntryState& state) {
        ftxui::Element label = ftxui::text(state.label);
        if (state.focused) label = label | ftxui::inverted;
        return label;
    };
    return option;
}

inline ftxui::Element SectionTitle(const std::string& title) {
    return ftxui::text(title) | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
}

inline ftxui::Element WindowTitle(const std::string& title) {
    return ftxui::text(" " + title + " ") | ftxui::bold;
}

inline ftxui::Element LabeledRow(const std::string& label,
                                 const ftxui::Component& field) {
    using namespace ftxui;
    return hbox({text(label) | size(WIDTH, EQUAL, kLabelWidth),
                 field->Render() | flex});
}

}  // namespace tui_theme

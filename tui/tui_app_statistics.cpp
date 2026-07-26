#include "tui_app.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace {
std::string ValueLabel(float value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(value < 10.0f ? 1 : 0) << value;
    return output.str();
}

ftxui::Element BarChart(const std::string& title, ftxui::Color chartColor,
                        const std::vector<float>& values) {
    using namespace ftxui;
    const float maximum = (std::max)(1.0f,
        *std::max_element(values.begin(), values.end()));
    auto bars = graph([values, maximum](int width, int height) {
        std::vector<int> output(static_cast<std::size_t>((std::max)(0, width)), 0);
        const int firstSlot = static_cast<int>(StatisticsHistory::kMaxSamples - values.size());
        for (int x = 0; x < width; ++x) {
            const int slot = width == 1 ? static_cast<int>(StatisticsHistory::kMaxSamples - 1)
                : x * static_cast<int>(StatisticsHistory::kMaxSamples - 1) / (width - 1);
            if (slot < firstSlot) continue;
            const std::size_t index = static_cast<std::size_t>(slot - firstSlot);
            if (index < values.size()) {
                output[static_cast<std::size_t>(x)] = static_cast<int>(
                    std::round((std::max)(0.0f, values[index]) / maximum * height));
            }
        }
        return output;
    });
    return vbox({
        text(title) | bold | color(chartColor),
        hbox({
            vbox({text(ValueLabel(maximum)) | dim, filler(), text("0") | dim})
                | size(WIDTH, EQUAL, 6),
            bars | color(chartColor) | flex,
        }) | flex,
        hbox({text("-60s") | dim, filler(), text("now") | dim}),
    }) | flex;
}

template <typename Value>
std::vector<float> Values(const std::vector<StatisticsSample>& samples, Value value) {
    std::vector<float> output;
    output.reserve(samples.size());
    for (const auto& sample : samples) output.push_back((std::max)(0.0f, value(sample)));
    return output;
}
}  // namespace

void TuiApp::UpdateStatisticsHistory() {
    const auto& stats = engine_.GetStats();
    statisticsHistory_.Update(stats.txBytes.load(), stats.rxBytes.load(),
                              stats.rttMilliseconds.load());
}

// The three charts share one row so the panel keeps a fixed height whatever
// the terminal size is.
ftxui::Element TuiApp::RenderStatisticsCharts() const {
    using namespace ftxui;
    const auto& samples = statisticsHistory_.Samples();
    if (samples.empty()) {
        return vbox({
            filler(),
            hbox({filler(), text("Collecting the first sample...") | dim,
                  filler()}),
            filler(),
        });
    }
    const auto tx = Values(samples,
        [](const StatisticsSample& sample) { return sample.txKibPerSecond; });
    const auto rx = Values(samples,
        [](const StatisticsSample& sample) { return sample.rxKibPerSecond; });
    const auto latency = Values(samples,
        [](const StatisticsSample& sample) { return sample.latencyMilliseconds; });
    return hbox({
        BarChart("TX KiB/s", Color::RedLight, tx),
        separator(),
        BarChart("RX KiB/s", Color::GreenLight, rx),
        separator(),
        BarChart("RTT ms", Color::YellowLight, latency),
    });
}

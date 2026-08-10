#include "gui_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "implot.h"

#include "gui_theme.h"

namespace {
// Always uses fixed notation so large speeds stay readable as plain integers
// instead of switching to the exponent form printf's %g would produce.
std::string ValueLabel(double value, const char* unit) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(value >= 1.0f ? 0
        : value >= 0.1f ? 1 : 2) << value << ' ' << unit;
    return output.str();
}

int TimeAxisFormatter(double value, char* buffer, int size, void*) {
    if (std::abs(value) < 0.5) return std::snprintf(buffer, size, "now");
    return std::snprintf(buffer, size, "%.0f s", value);
}

int ValueAxisFormatter(double value, char* buffer, int size, void*) {
    const int precision = value >= 1.0 ? 0 : value >= 0.1 ? 1 : 2;
    return std::snprintf(buffer, size, "%.*f", precision, value);
}

std::string SampleTimeLabel(const StatisticsSample& sample) {
    const std::time_t time = std::chrono::system_clock::to_time_t(sample.timestamp);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

template <typename Value>
void RenderPlot(const char* id, const char* title,
                const std::vector<StatisticsSample>& samples,
                const char* unit, const ImVec4& color, Value value) {
    const std::size_t firstSlot = StatisticsHistory::kMaxSamples - samples.size();
    std::vector<double> seconds(samples.size());
    std::vector<double> values(samples.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        seconds[index] = static_cast<double>(firstSlot + index)
            - static_cast<double>(StatisticsHistory::kMaxSamples - 1);
        values[index] = (std::max)(0.0, static_cast<double>(value(samples[index])));
    }

    const ImPlotFlags plotFlags = ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText
        | ImPlotFlags_Crosshairs;
    if (ImPlot::BeginPlot(id, ImVec2(-1.0f, -1.0f), plotFlags)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None,
                          ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, TimeAxisFormatter);
        ImPlot::SetupAxisFormat(ImAxis_Y1, ValueAxisFormatter);
        ImPlot::SetupAxisLimits(ImAxis_X1, -59.5, 0.5, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, -59.5, 0.5);
        ImPlot::SetupAxisZoomConstraints(ImAxis_X1, 5.0, 60.0);
        ImPlot::SetupAxisLimitsConstraints(
            ImAxis_Y1, 0.0, (std::numeric_limits<double>::max)());

        ImPlotSpec spec;
        spec.LineColor = color;
        spec.LineWeight = 2.0f;
        spec.FillColor = color;
        spec.FillAlpha = 0.18f;
        spec.Flags = ImPlotItemFlags_NoLegend | ImPlotLineFlags_Shaded;
        ImPlot::PlotLine(title, seconds.data(), values.data(),
                         static_cast<int>(values.size()), spec);

        if (ImPlot::IsPlotHovered()) {
            const double hoveredX = ImPlot::GetPlotMousePos().x;
            const long long slot = std::llround(hoveredX)
                + static_cast<long long>(StatisticsHistory::kMaxSamples - 1);
            if (slot >= static_cast<long long>(firstSlot)
                && slot < static_cast<long long>(StatisticsHistory::kMaxSamples)) {
                const std::size_t index = static_cast<std::size_t>(slot)
                    - firstSlot;
                const long long secondsAgo = static_cast<long long>(
                    StatisticsHistory::kMaxSamples - 1) - slot;
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(title);
                ImGui::Separator();
                ImGui::Text("%s (%lld s ago)",
                            SampleTimeLabel(samples[index]).c_str(), secondsAgo);
                ImGui::TextColored(color, "%s",
                                   ValueLabel(values[index], unit).c_str());
                ImGui::EndTooltip();
            }
        }
        ImPlot::EndPlot();
    }
}
}  // namespace

void GuiApp::UpdateStatisticsHistory() {
    const auto& stats = engine_.GetStats();
    statisticsHistory_.Update(stats.txBytes.load(), stats.rxBytes.load(),
                              stats.rttMilliseconds.load());
}

// ImPlot keeps the three X axes linked, so zooming or panning one chart changes
// the same 60-second window in the other two charts as well.
void GuiApp::RenderStatisticsCharts() {
    ImGui::Spacing();
    ImGui::SeparatorText("Last 60 seconds");
    const auto& samples = statisticsHistory_.Samples();
    if (samples.empty()) {
        ImGui::TextDisabled("Collecting the first sample...");
        return;
    }
    constexpr float kMinimumPlotHeight = 130.0f;
    const float plotHeight = (std::max)(kMinimumPlotHeight,
        ImGui::GetContentRegionAvail().y);
    const ImPlotSubplotFlags subplotFlags = ImPlotSubplotFlags_LinkAllX
        | ImPlotSubplotFlags_NoResize;
    if (ImPlot::BeginSubplots("##History", 1, 3, ImVec2(-1.0f, plotHeight),
                              subplotFlags)) {
        RenderPlot("TX speed (KiB/s)##TxHistory", "TX speed", samples, "KiB/s",
            gui_theme::kTx,
            [](const StatisticsSample& sample) { return sample.txKibPerSecond; });
        RenderPlot("RX speed (KiB/s)##RxHistory", "RX speed", samples, "KiB/s",
            gui_theme::kRx,
            [](const StatisticsSample& sample) { return sample.rxKibPerSecond; });
        RenderPlot("Latency (ms)##LatencyHistory", "Latency", samples, "ms",
            gui_theme::kLatency,
            [](const StatisticsSample& sample) { return sample.latencyMilliseconds; });
        ImPlot::EndSubplots();
    }
}

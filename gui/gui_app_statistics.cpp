#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

namespace {
float NiceScaleMaximum(float maximum) {
    if (maximum <= 0.0f) return 1.0f;
    const float magnitude = std::pow(10.0f, std::floor(std::log10(maximum)));
    const float normalized = maximum / magnitude;
    const float nice = normalized <= 1.0f ? 1.0f
        : normalized <= 2.0f ? 2.0f
        : normalized <= 5.0f ? 5.0f : 10.0f;
    return nice * magnitude;
}

std::string ScaleLabel(float maximum, const char* unit) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(maximum >= 1.0f ? 0
        : maximum >= 0.1f ? 1 : 2) << maximum << ' ' << unit;
    return output.str();
}

void RenderChartHeader(const char* title, const std::string& scaleLabel) {
    ImGui::TextUnformatted(title);
    ImGui::SameLine();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
        - ImGui::CalcTextSize(scaleLabel.c_str()).x;
    if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);
    ImGui::TextDisabled("%s", scaleLabel.c_str());
}

void RenderTimeAxis(const char* id) {
    constexpr const char* kFirstLabel = "60 s";
    constexpr const char* kLastLabel = "0";
    ImGui::PushID(id);
    ImGui::TextDisabled("%s", kFirstLabel);
    ImGui::SameLine();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
        - ImGui::CalcTextSize(kLastLabel).x;
    if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);
    ImGui::TextDisabled("%s", kLastLabel);
    ImGui::PopID();
}

template <typename Value>
void RenderHistogram(const char* id, const char* title,
                     const std::vector<StatisticsSample>& samples,
                     float plotHeight, const char* unit, Value value) {
    constexpr float kPreferredBarSlotWidth = 3.0f;
    const float plotWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
    const std::size_t renderedColumns = (std::max)(StatisticsHistory::kMaxSamples,
        static_cast<std::size_t>(plotWidth / kPreferredBarSlotWidth));
    const std::size_t firstSlot = StatisticsHistory::kMaxSamples - samples.size();
    std::vector<float> values(renderedColumns, 0.0f);
    for (std::size_t column = 0; column < renderedColumns; ++column) {
        const std::size_t slot = column * (StatisticsHistory::kMaxSamples - 1)
            / (renderedColumns - 1);
        if (slot < firstSlot) continue;
        values[column] = (std::max)(0.0f, value(samples[slot - firstSlot]));
    }
    const float scaleMaximum = NiceScaleMaximum(
        *std::max_element(values.begin(), values.end()));
    RenderChartHeader(title, ScaleLabel(scaleMaximum, unit));
    ImGui::PlotHistogram(id, values.data(), static_cast<int>(values.size()), 0,
                         nullptr, 0.0f, scaleMaximum, ImVec2(-FLT_MIN, plotHeight));
    RenderTimeAxis(id);
}
}  // namespace

void GuiApp::UpdateStatisticsHistory() {
    const auto& stats = engine_.GetStats();
    statisticsHistory_.Update(stats.txBytes.load(), stats.rxBytes.load(),
                              stats.rttMilliseconds.load());
}

void GuiApp::RenderStatisticsCharts() {
    ImGui::Spacing();
    ImGui::SeparatorText("60-second history");
    const auto& samples = statisticsHistory_.Samples();
    if (samples.empty()) {
        ImGui::TextDisabled("Collecting the first sample...");
        return;
    }
    constexpr float kMinimumPlotHeight = 76.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float nonPlotHeight = 4.0f * (ImGui::GetTextLineHeight() + style.ItemSpacing.y)
        + 2.0f * style.CellPadding.y + style.ItemSpacing.y;
    const float availableHeight = ImGui::GetContentRegionAvail().y;
    const float plotHeight = (std::max)(kMinimumPlotHeight,
        (availableHeight - nonPlotHeight) * 0.5f);
    if (ImGui::BeginTable("##SpeedHistory", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        RenderHistogram("##TxHistory", "TX speed", samples, plotHeight, "KiB/s",
            [](const StatisticsSample& sample) { return sample.txKibPerSecond; });
        ImGui::TableNextColumn();
        RenderHistogram("##RxHistory", "RX speed", samples, plotHeight, "KiB/s",
            [](const StatisticsSample& sample) { return sample.rxKibPerSecond; });
        ImGui::EndTable();
    }
    RenderHistogram("##LatencyHistory", "Latency", samples, plotHeight, "ms",
        [](const StatisticsSample& sample) { return sample.latencyMilliseconds; });
}

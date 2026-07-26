#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

#include "gui_theme.h"

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

// Always uses fixed notation so large speeds stay readable as plain integers
// instead of switching to the exponent form printf's %g would produce.
std::string ValueLabel(float value, const char* unit) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(value >= 1.0f ? 0
        : value >= 0.1f ? 1 : 2) << value << ' ' << unit;
    return output.str();
}

// Mirrors the hovered-column math of ImGui's PlotEx so a replacement tooltip
// describes the same bar that ImGui highlights.
bool HoveredPlotColumn(std::size_t columnCount, std::size_t* column) {
    if (!ImGui::IsItemHovered() || columnCount == 0) return false;
    const float left = ImGui::GetItemRectMin().x + ImGui::GetStyle().FramePadding.x;
    const float right = ImGui::GetItemRectMax().x - ImGui::GetStyle().FramePadding.x;
    if (right <= left) return false;
    const float position = (ImGui::GetIO().MousePos.x - left) / (right - left);
    const float clamped = (std::min)((std::max)(position, 0.0f), 0.9999f);
    *column = (std::min)(columnCount - 1,
        static_cast<std::size_t>(clamped * static_cast<float>(columnCount)));
    return true;
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
    constexpr const char* kFirstLabel = "-60s";
    constexpr const char* kLastLabel = "now";
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
                     float plotHeight, const char* unit,
                     const ImVec4& color, const ImVec4& hoveredColor, Value value) {
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
    RenderChartHeader(title, ValueLabel(scaleMaximum, unit));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogramHovered, hoveredColor);
    ImGui::PlotHistogram(id, values.data(), static_cast<int>(values.size()), 0,
                         nullptr, 0.0f, scaleMaximum, ImVec2(-FLT_MIN, plotHeight));
    ImGui::PopStyleColor(2);
    // Replaces the built-in tooltip, which prints the raw column index and
    // formats values with %g.
    std::size_t hoveredColumn = 0;
    if (HoveredPlotColumn(renderedColumns, &hoveredColumn)) {
        const std::size_t slot = hoveredColumn * (StatisticsHistory::kMaxSamples - 1)
            / (renderedColumns - 1);
        const std::size_t secondsAgo = StatisticsHistory::kMaxSamples - 1 - slot;
        if (slot < firstSlot) {
            ImGui::SetTooltip("-%zu s: no sample", secondsAgo);
        } else {
            ImGui::SetTooltip("-%zu s: %s", secondsAgo,
                              ValueLabel(values[hoveredColumn], unit).c_str());
        }
    }
    RenderTimeAxis(id);
}
}  // namespace

void GuiApp::UpdateStatisticsHistory() {
    const auto& stats = engine_.GetStats();
    statisticsHistory_.Update(stats.txBytes.load(), stats.rxBytes.load(),
                              stats.rttMilliseconds.load());
}

// All three plots share one row, so the panel keeps a single fixed height and
// the charts grow with the window instead of stacking below the fold.
void GuiApp::RenderStatisticsCharts() {
    ImGui::Spacing();
    ImGui::SeparatorText("Last 60 seconds");
    const auto& samples = statisticsHistory_.Samples();
    if (samples.empty()) {
        ImGui::TextDisabled("Collecting the first sample...");
        return;
    }
    constexpr float kMinimumPlotHeight = 76.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float nonPlotHeight = 2.0f * (ImGui::GetTextLineHeight() + style.ItemSpacing.y)
        + 2.0f * style.CellPadding.y;
    const float plotHeight = (std::max)(kMinimumPlotHeight,
        ImGui::GetContentRegionAvail().y - nonPlotHeight);
    const ImVec4 hovered(1.0f, 1.0f, 1.0f, 1.0f);
    if (ImGui::BeginTable("##History", 3,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        RenderHistogram("##TxHistory", "TX speed", samples, plotHeight, "KiB/s",
            gui_theme::kTx, hovered,
            [](const StatisticsSample& sample) { return sample.txKibPerSecond; });
        ImGui::TableNextColumn();
        RenderHistogram("##RxHistory", "RX speed", samples, plotHeight, "KiB/s",
            gui_theme::kRx, hovered,
            [](const StatisticsSample& sample) { return sample.rxKibPerSecond; });
        ImGui::TableNextColumn();
        RenderHistogram("##LatencyHistory", "Latency", samples, plotHeight, "ms",
            gui_theme::kLatency, hovered,
            [](const StatisticsSample& sample) { return sample.latencyMilliseconds; });
        ImGui::EndTable();
    }
}

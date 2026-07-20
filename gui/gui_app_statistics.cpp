#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <vector>

#include "imgui.h"

namespace {
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
                     const std::vector<StatisticsSample>& samples, Value value) {
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
    ImGui::PlotHistogram(id, values.data(), static_cast<int>(values.size()), 0,
                         title, 0.0f, FLT_MAX, ImVec2(-FLT_MIN, 58.0f));
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
    if (ImGui::BeginTable("##SpeedHistory", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        RenderHistogram("##TxHistory", "TX speed (KiB/s)", samples,
            [](const StatisticsSample& sample) { return sample.txKibPerSecond; });
        ImGui::TableNextColumn();
        RenderHistogram("##RxHistory", "RX speed (KiB/s)", samples,
            [](const StatisticsSample& sample) { return sample.rxKibPerSecond; });
        ImGui::EndTable();
    }
    RenderHistogram("##LatencyHistory", "Latency (ms)", samples,
        [](const StatisticsSample& sample) { return sample.latencyMilliseconds; });
}

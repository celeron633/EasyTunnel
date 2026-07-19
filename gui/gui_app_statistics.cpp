#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

namespace {
std::string TimeLabel(std::chrono::system_clock::time_point time) {
    const std::time_t value = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%H:%M:%S");
    return output.str();
}

void RenderTimeAxis(const char* id, const std::vector<StatisticsSample>& samples) {
    const std::string first = TimeLabel(samples.front().timestamp);
    const std::string last = TimeLabel(samples.back().timestamp);
    ImGui::PushID(id);
    ImGui::TextDisabled("%s", first.c_str());
    ImGui::SameLine();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
        - ImGui::CalcTextSize(last.c_str()).x;
    if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);
    ImGui::TextDisabled("%s", last.c_str());
    ImGui::PopID();
}

template <typename Value>
void RenderHistogram(const char* id, const char* title,
                     const std::vector<StatisticsSample>& samples, Value value) {
    constexpr float kMaximumBarWidth = 8.0f;
    const float plotWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
    const float slotWidth = plotWidth / static_cast<float>(StatisticsHistory::kMaxSamples);
    const int partsPerSlot = (std::max)(2, static_cast<int>(std::ceil(
        slotWidth / kMaximumBarWidth)));
    std::vector<float> values(StatisticsHistory::kMaxSamples
        * static_cast<std::size_t>(partsPerSlot), 0.0f);
    const std::size_t firstSlot = StatisticsHistory::kMaxSamples - samples.size();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const std::size_t slot = firstSlot + index;
        values[slot * static_cast<std::size_t>(partsPerSlot)
            + static_cast<std::size_t>(partsPerSlot / 2)] =
            (std::max)(0.0f, value(samples[index]));
    }
    ImGui::PlotHistogram(id, values.data(), static_cast<int>(values.size()), 0,
                         title, 0.0f, FLT_MAX, ImVec2(-FLT_MIN, 58.0f));
    RenderTimeAxis(id, samples);
}
}  // namespace

void GuiApp::UpdateStatisticsHistory() {
    const auto& stats = engine_.GetStats();
    statisticsHistory_.Update(stats.txBytes.load(), stats.rxBytes.load(),
                              stats.rttMilliseconds.load());
}

void GuiApp::RenderStatisticsCharts() {
    ImGui::Spacing();
    ImGui::SeparatorText("5-second history");
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

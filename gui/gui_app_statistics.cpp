#include "gui_app.h"

#include <algorithm>
#include <cfloat>
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
    std::vector<float> values;
    values.reserve(samples.size());
    for (const auto& sample : samples) values.push_back((std::max)(0.0f, value(sample)));
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
    RenderHistogram("##TxHistory", "TX speed (KiB/s)", samples,
        [](const StatisticsSample& sample) { return sample.txKibPerSecond; });
    RenderHistogram("##RxHistory", "RX speed (KiB/s)", samples,
        [](const StatisticsSample& sample) { return sample.rxKibPerSecond; });
    RenderHistogram("##LatencyHistory", "Latency (ms)", samples,
        [](const StatisticsSample& sample) { return sample.latencyMilliseconds; });
}

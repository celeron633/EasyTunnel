#include "exit_confirmation_dialog.h"

#include "imgui.h"

void ExitConfirmationDialog::Open() {
    openRequested_ = true;
}

bool ExitConfirmationDialog::Render() {
    if (openRequested_) {
        ImGui::OpenPopup("Exit EasyTunnel###ExitConfirmation");
        openRequested_ = false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    bool confirmed = false;
    if (ImGui::BeginPopupModal("Exit EasyTunnel###ExitConfirmation", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize
                                   | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Are you sure you want to exit EasyTunnel?");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Exit", ImVec2(110.0f, 0.0f))) {
            confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f))
            || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    return confirmed;
}

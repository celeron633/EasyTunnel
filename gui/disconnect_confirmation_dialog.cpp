#include "disconnect_confirmation_dialog.h"

#include "imgui.h"

void DisconnectConfirmationDialog::Open(bool hasActiveConnection) {
    hasActiveConnection_ = hasActiveConnection;
    openRequested_ = true;
}

DisconnectConfirmationDialog::Result DisconnectConfirmationDialog::Render() {
    if (openRequested_) {
        ImGui::OpenPopup("Disconnect EasyTunnel###DisconnectConfirmation");
        openRequested_ = false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    Result result = Result::None;
    if (ImGui::BeginPopupModal("Disconnect EasyTunnel###DisconnectConfirmation", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize
                                   | ImGuiWindowFlags_NoSavedSettings)) {
        if (hasActiveConnection_) {
            ImGui::TextUnformatted("Are you sure you want to disconnect?");
        } else {
            ImGui::TextUnformatted("There is no active connection.");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (hasActiveConnection_) {
            if (ImGui::Button("Yes", ImVec2(110.0f, 0.0f))) {
                result = Result::Confirmed;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(110.0f, 0.0f))
                || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
        } else {
            if (ImGui::Button("OK", ImVec2(110.0f, 0.0f))
                || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndPopup();
    }
    return result;
}

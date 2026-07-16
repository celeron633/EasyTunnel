#pragma once

class DisconnectConfirmationDialog {
public:
    enum class Result {
        None,
        Confirmed,
    };

    void Open(bool hasActiveConnection);
    Result Render();

private:
    bool openRequested_ = false;
    bool hasActiveConnection_ = false;
};

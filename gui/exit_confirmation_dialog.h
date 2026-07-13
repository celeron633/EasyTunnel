#pragma once

class ExitConfirmationDialog {
public:
    void Open();
    bool Render();

private:
    bool openRequested_ = false;
};

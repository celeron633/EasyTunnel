#pragma once

#include <string>

// The GUI requires elevation, so Windows startup is implemented as a logon
// task running with the highest available privileges. The scheduled task is
// the source of truth for the settings checkbox.
bool IsWindowsStartupEnabled(bool* enabled, std::string* error);
bool SetWindowsStartupEnabled(bool enabled, std::string* error);

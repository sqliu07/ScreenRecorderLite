#pragma once

#include <windows.h>

#include <optional>
#include <vector>

#include "platform/displays.h"

namespace srl {

struct RegionSelection {
    int monitorIndex = 0;
    HMONITOR monitor = nullptr;
    RECT rect{};  // monitor-local physical pixels
};

// Modal desktop overlay for selecting a rectangle on one monitor. The caller
// hides the main window first; this runs its own message loop and returns when
// Enter/Esc is pressed or the user confirms. Size is in physical pixels.
std::optional<RegionSelection> SelectRegion(HWND owner,
                                            const std::vector<MonitorInfo>& displays);

}  // namespace srl

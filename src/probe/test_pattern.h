#pragma once

#include <windows.h>

#include <string>

#include "platform/displays.h"

namespace probe {

struct TestPatternWindow {
    HWND window = nullptr;
    RECT screenRect{};
    RECT monitorLocalRect{};
};

bool CreateTestPatternWindow(const srl::MonitorInfo& monitor, TestPatternWindow& window,
                             std::wstring* error);
void PumpTestPatternWindow(const TestPatternWindow& window);
bool ResizeTestPatternWindow(const srl::MonitorInfo& monitor, int width, int height,
                             TestPatternWindow& window, std::wstring* error);
void DestroyTestPatternWindow(TestPatternWindow& window);

}  // namespace probe

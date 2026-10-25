#pragma once

#include <windows.h>

#include "core/output_size.h"

namespace srl {

// Small modal dialog for a custom output bound. The entered box is an upper
// limit: the actual output keeps the source aspect ratio and never enlarges.
class CustomSizeDialog {
public:
    [[nodiscard]] static bool Show(HWND owner, HINSTANCE instance, int sourceWidth,
                                   int sourceHeight, Size initial, Size* result);
};

}  // namespace srl

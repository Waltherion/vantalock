#pragma once

#include <cstdint>
#include <vector>

namespace overlay {

// A CPU-rendered RGBA8 (sRGB, non-premultiplied) image for blitting as an overlay.
struct TextImage {
    std::vector<uint8_t> rgba; // w*h*4, row 0 = top
    int w = 0;
    int h = 0;
    bool valid() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * h * 4; }
};

// State of the password field, reflected in the rendered overlay.
struct State {
    int passwordLen = 0;  // number of typed characters (shown as dots)
    bool verifying = false; // PAM check in progress
    bool error = false;     // last attempt failed
};

// Render the current time (HH:mm), date, and the password field onto a fixed-size
// transparent canvas, centred. Fixed size keeps the Vulkan texture + descriptors
// stable across refreshes. Localised via the system locale.
TextImage renderOverlay(const State &state);

} // namespace overlay

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

// Render the current time (HH:MM) and date onto a fixed-size transparent canvas,
// centred. Fixed size keeps the Vulkan texture + descriptors stable across the
// per-minute refresh. Localised via the system locale.
TextImage renderClock();

} // namespace overlay

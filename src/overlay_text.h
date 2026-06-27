#pragma once

#include <cstdint>
#include <vector>

struct Config;

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

struct Color {
    unsigned char r = 255, g = 255, b = 255, a = 255;
};

// Overlay colours, sourced from the active Hyprland theme (see loadTheme).
struct Theme {
    Color text{ 255, 255, 255, 255 };  // clock / date / dots
    Color accent{ 255, 255, 255, 255 }; // field border
    Color error{ 255, 26, 60, 255 };    // wrong-password feedback (#ff1a3c, monochrome-minimalism red)
    Color shadow{ 0, 0, 0, 150 };       // text shadow
};

// Read the active theme's colours from ~/.config/themes/current (a dedicated
// vantalock-colors.conf if present, else the hyprlock-colors.conf already shipped
// per theme). Falls back to the built-in defaults.
Theme loadTheme();

// Render the time (HH:mm), weekday + date lines, and the password field onto a
// fixed-size transparent canvas using the given config (fonts, colours, layout).
// Fixed size keeps the Vulkan texture + descriptors stable across refreshes.
TextImage renderOverlay(const State &state, const Config &cfg);

} // namespace overlay

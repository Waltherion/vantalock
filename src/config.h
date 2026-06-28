#pragma once

#include <string>
#include <vector>

#include "overlay_text.h"

// VantaLock configuration, loaded from ~/.config/vantalock/config.jsonc (a
// commented default is written on first run). Colours default to the active
// Hyprland theme (see overlay::loadTheme) and can be overridden here.
struct Config {
    // Background
    float blur = 0.02f; // blur radius in uv units (0 = sharp)
    int blurType = 1;   // default gaussian; 0 frosted (sparse-tap glass), 1 gaussian, 2 box, 3 pixelate, 4 none
    float dim = 0.5f;   // dim multiplier in linear (1 = none)

    // Thumbnail
    bool thumbShow = true;
    float thumbHeight = 0.24f; // height as fraction of output
    float thumbY = 0.55f;      // vertical centre as fraction of output
    float thumbRadius = 0.08f; // corner radius as fraction of thumb height (0 = square)

    // Fonts
    std::string fontFamily; // empty = system default
    int timeSize = 70;
    int weekdaySize = 30;
    int dateSize = 22;
    int fieldFontSize = 12;

    // Clock/date block (positions are fractions of the 1920x1080 overlay canvas)
    float timeY = 0.22f;    // time baseline-ish centre
    float weekdayY = 0.37f; // weekday line
    float dateY = 0.41f;    // date + year line

    // Password field
    int fieldW = 200;     // px on the 1920x1080 canvas
    int fieldH = 35;      // px
    float fieldY = 0.68f; // field TOP as fraction of canvas height

    // Colours (resolved from the theme, overridable in the JSONC)
    overlay::Color text;
    overlay::Color accent;
    overlay::Color error;
    overlay::Color shadow;

    // Text drop shadow (the shadow colour itself is `shadow` above)
    float shadowOffset = 1.4f;   // offset in reference px (scales with resolution; 0 = none)
    float shadowStrength = 0.5f; // multiplier on the shadow alpha (0 = invisible, 1 = full)

    // Rainbow text: a STATIC gradient across the clock/date (no animation). Off by default.
    bool rainbow = false;
    std::vector<overlay::Color> rainbowStops; // >=2 stops to form the gradient
    float rainbowPeriod = 0.0f;     // px per cycle on the 1920-wide reference (0 = span full width once)
    float rainbowBrightness = 1.0f; // band luminance multiplier (>1 pushes HDR brightness so colours pop)
    float rainbowSpeed = 0.0f;      // band scroll: cycles/sec (0 = static; negative = reverse). >0 = continuous GPU use

    // Bloom: a soft glow halo around the clock/date/field text (0 = off)
    float bloomStrength = 0.0f;

    // Load defaults (seeded from the active theme) merged with the user's JSONC.
    static Config load();
    static std::string configPath();
};

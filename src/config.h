#pragma once

#include <string>

#include "overlay_text.h"

// VantaLock configuration, loaded from ~/.config/vantalock/config.jsonc (a
// commented default is written on first run). Colours default to the active
// Hyprland theme (see overlay::loadTheme) and can be overridden here.
struct Config {
    // Background
    float blur = 0.02f; // blur radius in uv units (0 = sharp)
    float dim = 0.5f;   // dim multiplier in linear (1 = none)

    // Thumbnail
    bool thumbShow = true;
    float thumbHeight = 0.24f; // height as fraction of output
    float thumbY = 0.55f;      // vertical centre as fraction of output
    float thumbRadius = 0.08f; // corner radius as fraction of thumb height (0 = square)

    // Fonts
    std::string fontFamily; // empty = system default
    int timeSize = 120;
    int weekdaySize = 50;
    int dateSize = 38;
    int fieldFontSize = 18;

    // Clock/date block (positions are fractions of the 1920x1080 overlay canvas)
    float timeY = 0.14f;    // time baseline-ish centre
    float weekdayY = 0.27f; // weekday line
    float dateY = 0.34f;    // date + year line

    // Password field
    int fieldW = 300;     // px on the 1920x1080 canvas
    int fieldH = 52;      // px
    float fieldY = 0.68f; // field TOP as fraction of canvas height

    // Colours (resolved from the theme, overridable in the JSONC)
    overlay::Color text;
    overlay::Color accent;
    overlay::Color error;
    overlay::Color shadow;

    // Load defaults (seeded from the active theme) merged with the user's JSONC.
    static Config load();
    static std::string configPath();
};

#pragma once

struct wl_display;
struct wl_surface;
struct wl_event_queue;
struct wp_color_manager_v1;
struct wp_color_management_surface_v1;
struct wp_image_description_v1;

namespace cm {

// Persistent colour-management handle for one lock surface's wl_surface. Tag the
// surface windows-scRGB so the compositor honours true black (0 = 0 nits) instead
// of applying its SDR black lift. Adapted from vantaviewer's cm_tagging, but
// decoupled from Qt: we take the raw wl_display + wl_surface directly (the lock
// surface comes from ext-session-lock, not from Qt's QPA).
//
// A dedicated wl_event_queue isolates our synchronous roundtrips from the main
// loop's event queue, so probing the manager / waiting for an image description
// never re-enters our lock/input dispatch.
class SurfaceColor
{
public:
    SurfaceColor(wl_display *display, wl_surface *surface);
    ~SurfaceColor();

    bool valid() const { return m_manager != nullptr && m_cmSurface != nullptr; }
    bool supportsScrgb() const { return m_supportsScrgb; }

    void setWindowsScrgb(); // HDR: linear scRGB, true blacks + HDR headroom
    void setSrgb();         // SDR: plain sRGB (relative)

private:
    void applyDescription(wp_image_description_v1 *desc, const char *label);

    wl_display *m_display = nullptr;
    wl_event_queue *m_queue = nullptr;
    wp_color_manager_v1 *m_manager = nullptr;
    wp_color_management_surface_v1 *m_cmSurface = nullptr;
    bool m_supportsScrgb = false;
};

} // namespace cm

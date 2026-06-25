#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "renderer.h"

struct HdrImage;
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_seat;
struct wl_keyboard;
struct wl_output;
struct wl_surface;
struct ext_session_lock_manager_v1;
struct ext_session_lock_v1;
struct ext_session_lock_surface_v1;

namespace cm {
class SurfaceColor;
}

// Drives the whole Fase 0 spike: connect to Wayland, lock the session via
// ext-session-lock-v1, create one lock surface + raw-Vulkan scRGB swapchain per
// output, tag each surface windows-scRGB, present the HDR image, and unlock on Esc.
class SessionLock
{
public:
    explicit SessionLock(const HdrImage &image);
    ~SessionLock();

    // Connect, lock, and run the event loop until Esc / finished. Returns true on
    // a clean exit (locked then unlocked), false if locking failed.
    bool run();

    // Non-locking diagnostic: connect, create a throwaway surface, and report the
    // Vulkan GPU + whether the scRGB HDR swapchain format is available. Never locks.
    bool probe();

    // --- Wayland callbacks (public so the C listeners can reach them) ---
    void onGlobal(wl_registry *reg, uint32_t name, const char *iface, uint32_t version);
    void onGlobalRemove(uint32_t name);
    void onLocked();
    void onFinished();
    void onSeatCapabilities(uint32_t caps);
    void onKey(uint32_t key, uint32_t state);

    struct OutputCtx {
        SessionLock *owner = nullptr;
        wl_output *output = nullptr;
        uint32_t globalName = 0;
        std::string name; // connector name (e.g. "DP-1"), from wl_output.name
        wl_surface *surface = nullptr;
        ext_session_lock_surface_v1 *lockSurface = nullptr;
        std::unique_ptr<cm::SurfaceColor> color;
        Renderer::Output render;
        bool configured = false;
        uint32_t w = 0, h = 0;
    };
    void onSurfaceConfigure(OutputCtx *ctx, uint32_t serial, uint32_t w, uint32_t h);
    void onOutputName(OutputCtx *ctx, const char *name);

private:
    void setupOutput(OutputCtx *ctx);
    // True if the monitor with this connector name is in HDR mode, per
    // `hyprctl monitors -j` (colorManagementPreset). Cached on first query.
    bool monitorWantsHdr(const std::string &name);

    std::map<std::string, bool> m_hdrByName;
    bool m_hdrQueried = false;

    const HdrImage &m_image;

    wl_display *m_display = nullptr;
    wl_compositor *m_compositor = nullptr;
    wl_seat *m_seat = nullptr;
    wl_keyboard *m_keyboard = nullptr;
    ext_session_lock_manager_v1 *m_lockManager = nullptr;
    ext_session_lock_v1 *m_lock = nullptr;

    std::unique_ptr<Renderer> m_renderer;
    std::vector<std::unique_ptr<OutputCtx>> m_outputs;

    bool m_locked = false;
    bool m_finished = false;
    bool m_running = true;
    bool m_deviceReady = false;
};

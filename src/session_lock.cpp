#include "session_lock.h"

#include "cm_tag.h"
#include "hdr_image.h"

#include <wayland-client.h>
#include "ext-session-lock-v1-client-protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <unistd.h>

// ---- C listener trampolines ----------------------------------------------

namespace {

void regGlobal(void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    static_cast<SessionLock *>(data)->onGlobal(reg, name, iface, version);
}
void regGlobalRemove(void *data, wl_registry *, uint32_t name)
{
    static_cast<SessionLock *>(data)->onGlobalRemove(name);
}
const wl_registry_listener kRegistryListener = { regGlobal, regGlobalRemove };

void lockLocked(void *data, ext_session_lock_v1 *) { static_cast<SessionLock *>(data)->onLocked(); }
void lockFinished(void *data, ext_session_lock_v1 *) { static_cast<SessionLock *>(data)->onFinished(); }
const ext_session_lock_v1_listener kLockListener = { lockLocked, lockFinished };

void surfConfigure(void *data, ext_session_lock_surface_v1 *, uint32_t serial, uint32_t w, uint32_t h)
{
    auto *ctx = static_cast<SessionLock::OutputCtx *>(data);
    ctx->owner->onSurfaceConfigure(ctx, serial, w, h);
}
const ext_session_lock_surface_v1_listener kSurfaceListener = { surfConfigure };

void kbKeymap(void *, wl_keyboard *, uint32_t, int32_t fd, uint32_t) { if (fd >= 0) ::close(fd); }
void kbEnter(void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {}
void kbLeave(void *, wl_keyboard *, uint32_t, wl_surface *) {}
void kbKey(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    static_cast<SessionLock *>(data)->onKey(key, state);
}
void kbModifiers(void *, wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void kbRepeat(void *, wl_keyboard *, int32_t, int32_t) {}
const wl_keyboard_listener kKeyboardListener = {
    kbKeymap, kbEnter, kbLeave, kbKey, kbModifiers, kbRepeat
};

void seatCaps(void *data, wl_seat *, uint32_t caps)
{
    static_cast<SessionLock *>(data)->onSeatCapabilities(caps);
}
void seatName(void *, wl_seat *, const char *) {}
const wl_seat_listener kSeatListener = { seatCaps, seatName };

} // namespace

// ---- SessionLock ----------------------------------------------------------

SessionLock::SessionLock(const HdrImage &image)
    : m_image(image)
{
}

SessionLock::~SessionLock()
{
    for (auto &o : m_outputs) {
        o->color.reset();
        if (m_renderer)
            m_renderer->destroyOutput(o->render);
        if (o->lockSurface)
            ext_session_lock_surface_v1_destroy(o->lockSurface);
        if (o->surface)
            wl_surface_destroy(o->surface);
    }
    m_outputs.clear();
    m_renderer.reset();

    if (m_keyboard) wl_keyboard_destroy(m_keyboard);
    if (m_seat) wl_seat_destroy(m_seat);
    if (m_lockManager) ext_session_lock_manager_v1_destroy(m_lockManager);
    if (m_compositor) wl_compositor_destroy(m_compositor);
    if (m_display) wl_display_disconnect(m_display);
}

void SessionLock::onGlobal(wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        m_compositor = static_cast<wl_compositor *>(
            wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4));
    } else if (std::strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
        m_lockManager = static_cast<ext_session_lock_manager_v1 *>(
            wl_registry_bind(reg, name, &ext_session_lock_manager_v1_interface, 1));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        m_seat = static_cast<wl_seat *>(
            wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5));
        wl_seat_add_listener(m_seat, &kSeatListener, this);
    } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        auto ctx = std::make_unique<OutputCtx>();
        ctx->owner = this;
        ctx->globalName = name;
        ctx->output = static_cast<wl_output *>(
            wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4));
        m_outputs.push_back(std::move(ctx));
    }
}

void SessionLock::onGlobalRemove(uint32_t name)
{
    // Output unplugged: drop its lock surface (Fase 0 keeps this minimal).
    for (auto it = m_outputs.begin(); it != m_outputs.end(); ++it) {
        if ((*it)->globalName == name) {
            (*it)->color.reset();
            if (m_renderer)
                m_renderer->destroyOutput((*it)->render);
            if ((*it)->lockSurface)
                ext_session_lock_surface_v1_destroy((*it)->lockSurface);
            if ((*it)->surface)
                wl_surface_destroy((*it)->surface);
            m_outputs.erase(it);
            return;
        }
    }
}

void SessionLock::onLocked()
{
    m_locked = true;
    std::fprintf(stderr, "vantalock: session locked\n");
}

void SessionLock::onFinished()
{
    m_finished = true;
    m_running = false;
    std::fprintf(stderr, "vantalock: lock finished (compositor denied or ended lock)\n");
}

void SessionLock::onSeatCapabilities(uint32_t caps)
{
    const bool hasKb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (hasKb && !m_keyboard) {
        m_keyboard = wl_seat_get_keyboard(m_seat);
        wl_keyboard_add_listener(m_keyboard, &kKeyboardListener, this);
    } else if (!hasKb && m_keyboard) {
        wl_keyboard_destroy(m_keyboard);
        m_keyboard = nullptr;
    }
}

void SessionLock::onKey(uint32_t key, uint32_t state)
{
    // Fase 0: Esc (evdev keycode 1) unlocks. Real auth comes in Fase 2.
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && key == 1) {
        std::fprintf(stderr, "vantalock: Esc -> unlocking\n");
        m_running = false;
    }
}

void SessionLock::setupOutput(OutputCtx *ctx)
{
    ctx->surface = wl_compositor_create_surface(m_compositor);
    ctx->lockSurface = ext_session_lock_v1_get_lock_surface(m_lock, ctx->surface, ctx->output);
    ext_session_lock_surface_v1_add_listener(ctx->lockSurface, &kSurfaceListener, ctx);

    VkSurfaceKHR vks = m_renderer->createWaylandSurface(m_display, ctx->surface);
    ctx->render.surface = vks;
}

void SessionLock::onSurfaceConfigure(OutputCtx *ctx, uint32_t serial, uint32_t w, uint32_t h)
{
    ext_session_lock_surface_v1_ack_configure(ctx->lockSurface, serial);
    wl_display_flush(m_display);
    ctx->w = w;
    ctx->h = h;

    // Lazily bring up the Vulkan device on the first configured output.
    if (!m_deviceReady) {
        if (!m_renderer->ensureDevice(ctx->render.surface, m_image)) {
            std::fprintf(stderr, "vantalock: device init failed\n");
            m_running = false;
            return;
        }
        m_deviceReady = true;
    }

    if (!ctx->configured) {
        if (!m_renderer->createOutput(ctx->render, ctx->render.surface, w, h, m_image)) {
            std::fprintf(stderr, "vantalock: createOutput failed\n");
            return;
        }
        // Tag the surface BEFORE the first commit (present) so true black holds.
        // Default OFF: the Vulkan WSI already propagates the swapchain's scRGB
        // colour space, and a second image-description on the same wl_surface can
        // be a protocol error. Enable VANTALOCK_CM_TAG=1 to force a manual tag if
        // black still looks grey (matches vantapaper/vantaviewer's Qt path).
        if (std::getenv("VANTALOCK_CM_TAG")) {
            ctx->color = std::make_unique<cm::SurfaceColor>(m_display, ctx->surface);
            if (ctx->color->valid()) {
                if (m_renderer->hdrActive() && ctx->color->supportsScrgb())
                    ctx->color->setWindowsScrgb();
                else
                    ctx->color->setSrgb();
            }
        }
        ctx->configured = true;
        m_renderer->renderOutput(ctx->render);
    } else if (ctx->render.extent.width != w || ctx->render.extent.height != h) {
        // Resize: rebuild swapchain (keep the cm tag + surface).
        Renderer::Output fresh;
        m_renderer->destroyOutput(ctx->render);
        ctx->render = fresh;
        ctx->render.surface = m_renderer->createWaylandSurface(m_display, ctx->surface);
        m_renderer->createOutput(ctx->render, ctx->render.surface, w, h, m_image);
        m_renderer->renderOutput(ctx->render);
    }
}

bool SessionLock::probe()
{
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        std::fprintf(stderr, "vantalock: cannot connect to Wayland display\n");
        return false;
    }
    wl_registry *registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(registry, &kRegistryListener, this);
    wl_display_roundtrip(m_display);
    if (!m_compositor) {
        std::fprintf(stderr, "vantalock: no wl_compositor\n");
        return false;
    }
    std::fprintf(stderr, "vantalock: ext_session_lock_manager_v1 %s\n",
        m_lockManager ? "present" : "MISSING");

    m_renderer = std::make_unique<Renderer>(true);
    if (!m_renderer->ok())
        return false;

    // A plain (non-lock) throwaway surface is enough to query Vulkan formats and
    // never locks the session.
    wl_surface *tmp = wl_compositor_create_surface(m_compositor);
    VkSurfaceKHR vks = m_renderer->createWaylandSurface(m_display, tmp);
    const bool ok = (vks != VK_NULL_HANDLE) && m_renderer->probe(vks);
    if (vks)
        vkDestroySurfaceKHR(m_renderer->instance(), vks, nullptr);
    wl_surface_destroy(tmp);
    return ok;
}

bool SessionLock::run()
{
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        std::fprintf(stderr, "vantalock: cannot connect to Wayland display\n");
        return false;
    }

    wl_registry *registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(registry, &kRegistryListener, this);
    wl_display_roundtrip(m_display); // bind globals + collect outputs

    if (!m_compositor || !m_lockManager) {
        std::fprintf(stderr, "vantalock: missing wl_compositor or ext_session_lock_manager_v1\n");
        return false;
    }
    if (m_outputs.empty()) {
        std::fprintf(stderr, "vantalock: no outputs\n");
        return false;
    }

    m_renderer = std::make_unique<Renderer>(true /* wantHdr */);
    if (!m_renderer->ok()) {
        std::fprintf(stderr, "vantalock: Vulkan instance init failed\n");
        return false;
    }

    m_lock = ext_session_lock_manager_v1_lock(m_lockManager);
    ext_session_lock_v1_add_listener(m_lock, &kLockListener, this);

    for (auto &o : m_outputs)
        setupOutput(o.get());
    wl_display_flush(m_display);

    // Safety net for testing: VANTALOCK_TIMEOUT=<seconds> auto-unlocks, so a first
    // live test can never strand the session if Esc/keyboard fails.
    long timeoutMs = -1;
    if (const char *t = std::getenv("VANTALOCK_TIMEOUT")) {
        const long secs = std::atol(t);
        if (secs > 0)
            timeoutMs = secs * 1000;
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    const int fd = wl_display_get_fd(m_display);
    while (m_running && !m_finished) {
        // Prepare-read pattern so we can poll with a timeout for the safety net.
        while (wl_display_prepare_read(m_display) != 0)
            wl_display_dispatch_pending(m_display);
        wl_display_flush(m_display);

        int pollTimeout = 1000; // wake periodically to check the safety deadline
        if (timeoutMs >= 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const long elapsed = (now.tv_sec - start.tv_sec) * 1000
                + (now.tv_nsec - start.tv_nsec) / 1000000;
            const long remain = timeoutMs - elapsed;
            if (remain <= 0) {
                wl_display_cancel_read(m_display);
                std::fprintf(stderr, "vantalock: safety timeout -> unlocking\n");
                m_running = false;
                break;
            }
            if (remain < pollTimeout)
                pollTimeout = int(remain);
        }

        struct pollfd pfd{ fd, POLLIN, 0 };
        const int pr = poll(&pfd, 1, pollTimeout);
        if (pr < 0) {
            wl_display_cancel_read(m_display);
            std::fprintf(stderr, "vantalock: poll error\n");
            break;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            wl_display_read_events(m_display);
        } else {
            wl_display_cancel_read(m_display);
        }
        if (wl_display_dispatch_pending(m_display) < 0) {
            std::fprintf(stderr, "vantalock: dispatch error\n");
            break;
        }
    }

    if (m_finished) {
        // Compositor never granted (or revoked) the lock: destroy without unlocking.
        if (m_lock)
            ext_session_lock_v1_destroy(m_lock);
        return false;
    }

    // Clean unlock path.
    if (m_lock) {
        if (m_locked)
            ext_session_lock_v1_unlock_and_destroy(m_lock);
        else
            ext_session_lock_v1_destroy(m_lock);
        wl_display_roundtrip(m_display); // ensure the server processes the unlock
    }
    return m_locked;
}

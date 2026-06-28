#include "session_lock.h"

#include "cm_tag.h"
#include "hdr_image.h"
#include "overlay_text.h"

#include <wayland-client.h>
#include "ext-session-lock-v1-client-protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <xkbcommon/xkbcommon.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/mman.h>
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

void kbKeymap(void *data, wl_keyboard *, uint32_t, int32_t fd, uint32_t size)
{
    static_cast<SessionLock *>(data)->onKeymap(fd, size);
}
void kbEnter(void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {}
void kbLeave(void *, wl_keyboard *, uint32_t, wl_surface *) {}
void kbKey(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    static_cast<SessionLock *>(data)->onKey(key, state);
}
void kbModifiers(void *data, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched,
                 uint32_t locked, uint32_t group)
{
    static_cast<SessionLock *>(data)->onModifiers(depressed, latched, locked, group);
}
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

void outGeometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t,
                 const char *, const char *, int32_t) {}
void outMode(void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {}
void outDone(void *, wl_output *) {}
void outScale(void *, wl_output *, int32_t) {}
void outName(void *data, wl_output *, const char *name)
{
    auto *ctx = static_cast<SessionLock::OutputCtx *>(data);
    ctx->owner->onOutputName(ctx, name);
}
void outDescription(void *, wl_output *, const char *) {}
const wl_output_listener kOutputListener = {
    outGeometry, outMode, outDone, outScale, outName, outDescription
};

} // namespace

// ---- SessionLock ----------------------------------------------------------

SessionLock::SessionLock(const HdrImage &image, const Config &config)
    : m_image(image)
    , m_config(config)
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

    if (m_xkbState) xkb_state_unref(m_xkbState);
    if (m_keymap) xkb_keymap_unref(m_keymap);
    if (m_xkb) xkb_context_unref(m_xkb);
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
        // v4+ delivers the connector name event we match against hyprctl.
        if (version >= 4)
            wl_output_add_listener(ctx->output, &kOutputListener, ctx.get());
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

void SessionLock::onOutputName(OutputCtx *ctx, const char *name)
{
    if (name)
        ctx->name = name;
}

bool SessionLock::monitorWantsHdr(const std::string &name)
{
    if (!m_hdrQueried) {
        m_hdrQueried = true;
        QProcess p;
        p.start(QStringLiteral("hyprctl"), { QStringLiteral("monitors"), QStringLiteral("-j") });
        if (p.waitForFinished(2000)) {
            const QJsonDocument doc = QJsonDocument::fromJson(p.readAllStandardOutput());
            for (const QJsonValue &v : doc.array()) {
                const QJsonObject o = v.toObject();
                const std::string n = o.value(QStringLiteral("name")).toString().toStdString();
                const bool hdr = o.value(QStringLiteral("colorManagementPreset")).toString()
                    == QStringLiteral("hdr");
                m_hdrByName[n] = hdr;
                std::fprintf(stderr, "vantalock: monitor %s -> %s\n", n.c_str(), hdr ? "HDR" : "SDR");
            }
        } else {
            std::fprintf(stderr, "vantalock: hyprctl query failed; assuming all SDR\n");
        }
    }
    auto it = m_hdrByName.find(name);
    // Default SDR when unknown: an sRGB surface displays correctly on any monitor,
    // whereas an scRGB surface on an SDR monitor washes out.
    return it != m_hdrByName.end() ? it->second : false;
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

void SessionLock::onKeymap(int32_t fd, uint32_t size)
{
    if (fd < 0)
        return;
    char *map = static_cast<char *>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED) {
        ::close(fd);
        return;
    }
    if (!m_xkb)
        m_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (m_keymap)
        xkb_keymap_unref(m_keymap);
    if (m_xkbState)
        xkb_state_unref(m_xkbState);
    m_keymap = xkb_keymap_new_from_string(m_xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
    m_xkbState = m_keymap ? xkb_state_new(m_keymap) : nullptr;
    munmap(map, size);
    ::close(fd);
}

void SessionLock::onModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    if (m_xkbState)
        xkb_state_update_mask(m_xkbState, depressed, latched, locked, 0, 0, group);
}

void SessionLock::onKey(uint32_t key, uint32_t state)
{
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !m_xkbState)
        return;
    if (m_auth.busy())
        return; // ignore input while a PAM check is in flight

    const xkb_keycode_t code = key + 8; // evdev -> xkb
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_xkbState, code);

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        submitPassword();
        return;
    }
    if (sym == XKB_KEY_BackSpace) {
        if (!m_password.empty()) {
            // Drop the last UTF-8 character (trailing continuation bytes + lead).
            size_t i = m_password.size();
            do { --i; } while (i > 0 && (uint8_t(m_password[i]) & 0xC0) == 0x80);
            m_password.erase(i);
            if (m_ostate.passwordLen > 0)
                m_ostate.passwordLen--;
            m_ostate.error = false;
            refreshOverlay();
        }
        return;
    }
    if (sym == XKB_KEY_Escape) {
        m_password.clear();
        m_ostate.passwordLen = 0;
        m_ostate.error = false;
        refreshOverlay();
        return;
    }

    char buf[64];
    const int n = xkb_state_key_get_utf8(m_xkbState, code, buf, sizeof(buf));
    if (n > 0 && (buf[0] != '\0')) {
        // Ignore control characters (Tab, etc.) below space.
        if (uint8_t(buf[0]) >= 0x20) {
            m_password.append(buf, size_t(n));
            m_ostate.passwordLen++;
            m_ostate.error = false;
            refreshOverlay();
        }
    }
}

void SessionLock::submitPassword()
{
    if (m_password.empty() || m_auth.busy())
        return;
    m_ostate.verifying = true;
    m_ostate.error = false;
    refreshOverlay();
    m_auth.authenticate(m_password);
}

void SessionLock::onAuthResult()
{
    const bool ok = m_auth.consumeResult();
    m_ostate.verifying = false;
    m_password.clear();
    m_ostate.passwordLen = 0;
    if (ok) {
        std::fprintf(stderr, "vantalock: authentication succeeded -> unlocking\n");
        m_running = false; // cleanup unlocks (m_locked -> unlock_and_destroy)
    } else {
        std::fprintf(stderr, "vantalock: authentication failed\n");
        m_ostate.error = true;
        refreshOverlay();
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
        // Upload the overlay (clock + password field) before any output binds it.
        // Render the canvas at this output's native resolution so the text is sharp
        // (the 1920x1080 reference is upscaled otherwise -> blurry on 4K). The scale
        // is fixed here and reused on every refresh so the texture size stays stable.
        m_overlayScale = ctx->h > 0 ? double(ctx->h) / 1080.0 : 1.0;
        const overlay::TextImage ov = overlay::renderOverlay(m_ostate, m_config, m_overlayScale);
        if (ov.valid())
            m_renderer->uploadOverlay(ov.rgba.data(), ov.w, ov.h);
        m_deviceReady = true;
    }

    const bool wantHdr = monitorWantsHdr(ctx->name);

    if (!ctx->configured) {
        if (!m_renderer->createOutput(ctx->render, ctx->render.surface, w, h, m_image, wantHdr)) {
            std::fprintf(stderr, "vantalock: createOutput failed\n");
            return;
        }
        // Tag the surface BEFORE the first commit (present) so true black holds.
        // Default OFF: the Vulkan WSI already propagates the swapchain's colour
        // space, and a second image-description on the same wl_surface can be a
        // protocol error. Enable VANTALOCK_CM_TAG=1 to force a manual tag if black
        // still looks grey (matches vantapaper/vantaviewer's Qt path).
        if (std::getenv("VANTALOCK_CM_TAG")) {
            ctx->color = std::make_unique<cm::SurfaceColor>(m_display, ctx->surface);
            if (ctx->color->valid()) {
                if (ctx->render.hdr && ctx->color->supportsScrgb())
                    ctx->color->setWindowsScrgb();
                else
                    ctx->color->setSrgb();
            }
        }
        ctx->configured = true;
        m_renderer->renderOutput(ctx->render);
    } else if (ctx->render.extent.width != w || ctx->render.extent.height != h) {
        // Resize: rebuild swapchain (keep the cm tag + surface).
        m_renderer->destroyOutput(ctx->render);
        ctx->render = Renderer::Output{};
        ctx->render.surface = m_renderer->createWaylandSurface(m_display, ctx->surface);
        m_renderer->createOutput(ctx->render, ctx->render.surface, w, h, m_image, wantHdr);
        m_renderer->renderOutput(ctx->render);
    }
}

void SessionLock::refreshOverlay()
{
    if (!m_deviceReady)
        return;
    const overlay::TextImage ov = overlay::renderOverlay(m_ostate, m_config, m_overlayScale);
    if (ov.valid())
        m_renderer->uploadOverlay(ov.rgba.data(), ov.w, ov.h);
    // Re-render directly (NOT via frame callbacks): on Wayland, requestUpdate from
    // a timer/async path may never paint. Each configured output redraws.
    for (auto &o : m_outputs) {
        if (o->configured)
            m_renderer->renderOutput(o->render);
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

    m_renderer = std::make_unique<Renderer>(m_config);
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
    wl_display_roundtrip(m_display); // collect wl_output name/mode events

    if (!m_compositor || !m_lockManager) {
        std::fprintf(stderr, "vantalock: missing wl_compositor or ext_session_lock_manager_v1\n");
        return false;
    }
    if (m_outputs.empty()) {
        std::fprintf(stderr, "vantalock: no outputs\n");
        return false;
    }

    m_renderer = std::make_unique<Renderer>(m_config);
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

    // Rainbow scroll: only when enabled with a non-zero speed do we render continuously
    // (~30fps). Otherwise the loop stays event-driven (minute tick / keypress) at ~zero cost.
    const bool animating = m_config.rainbow && m_config.rainbowStops.size() >= 2
                           && m_config.rainbowSpeed != 0.0f;

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
        if (animating && pollTimeout > 33)
            pollTimeout = 33; // ~30fps while the band is rolling

        struct pollfd pfds[2];
        pfds[0] = { fd, POLLIN, 0 };
        pfds[1] = { m_auth.eventFd(), POLLIN, 0 };
        const int pr = poll(pfds, 2, pollTimeout);
        if (pr < 0) {
            wl_display_cancel_read(m_display);
            std::fprintf(stderr, "vantalock: poll error\n");
            break;
        }
        if (pr > 0 && (pfds[0].revents & POLLIN)) {
            wl_display_read_events(m_display);
        } else {
            wl_display_cancel_read(m_display);
        }
        if (wl_display_dispatch_pending(m_display) < 0) {
            std::fprintf(stderr, "vantalock: dispatch error\n");
            break;
        }

        // PAM result ready: unlock on success, show an error otherwise.
        if (pfds[1].revents & POLLIN)
            onAuthResult();

        // Tick the clock: refresh + re-render when the minute changes.
        std::time_t tt = std::time(nullptr);
        std::tm lt{};
        localtime_r(&tt, &lt);
        if (lt.tm_min != m_lastMinute) {
            m_lastMinute = lt.tm_min;
            refreshOverlay();
        }

        // Advance the rolling band: update the phase from elapsed time and re-render
        // every output. No overlay re-upload -- the white mask is static; only `phase`
        // changes -- so this is just the (cheap) per-frame redraw.
        if (animating) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const double elapsed = double(now.tv_sec - start.tv_sec)
                + double(now.tv_nsec - start.tv_nsec) / 1e9;
            m_renderer->setRainbowPhase(float(m_config.rainbowSpeed * elapsed));
            for (auto &o : m_outputs)
                if (o->configured)
                    m_renderer->renderOutput(o->render);
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

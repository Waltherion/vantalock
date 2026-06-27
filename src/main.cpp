// VantaLock — HDR lock screen (Fase 0 spike).
//
// Decodes one image (the active vantapaper wallpaper by default), then locks the
// session via ext-session-lock-v1 and presents it in true HDR with real black on
// OLED, on every output. Esc unlocks. No authentication yet — this build only
// de-risks "session-lock + raw Vulkan + wp-color-management" on Hyprland/NVIDIA.

#include "config.h"
#include "hdr_image.h"
#include "session_lock.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QString>
#include <QtCore/qfloat16.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv)
{
    // Decode happens via Qt's image stack; we never create a Qt window (the lock
    // surface is raw Wayland), so force the offscreen platform — no display conn.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");

    QGuiApplication app(argc, argv);

    bool probeOnly = false;
    bool decodeTest = false;
    QString path;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--probe"))
            probeOnly = true;
        else if (a == QStringLiteral("--decode"))
            decodeTest = true;
        else if (!a.startsWith(QStringLiteral("--")))
            path = a;
    }

    if (probeOnly) {
        HdrImage dummy; // probe needs no real image
        SessionLock lock(dummy, Config{});
        const bool ok = lock.probe();
        return ok ? 0 : 1;
    }

    // `vantalock --decode <image>`: decode and report stats (no lock). Diagnostic for
    // "black screen" wallpapers — distinguishes a decode failure from a render issue.
    if (decodeTest) {
        if (path.isEmpty()) {
            std::fprintf(stderr, "usage: vantalock --decode <image>\n");
            return 2;
        }
        const HdrImage img = decodeImage(path);
        if (!img.valid()) {
            std::fprintf(stderr, "DECODE FAILED (invalid image): %s\n", qPrintable(path));
            return 1;
        }
        float mn = 1e30f, mx = -1e30f, amn = 1e30f, amx = -1e30f;
        double sum = 0.0;
        size_t nonfinite = 0;
        const size_t px = size_t(img.w) * img.h;
        auto h2f = [&](size_t idx) { qfloat16 hf; std::memcpy(&hf, &img.rgba16f[idx], 2); return float(hf); };
        for (size_t i = 0; i < px; ++i) {
            for (int c = 0; c < 3; ++c) { // RGB
                const float v = h2f(i * 4 + c);
                if (!std::isfinite(v)) { ++nonfinite; continue; }
                mn = v < mn ? v : mn;
                mx = v > mx ? v : mx;
                sum += v;
            }
            const float a = h2f(i * 4 + 3); // alpha
            amn = a < amn ? a : amn;
            amx = a > amx ? a : amx;
        }
        const size_t cen = (size_t(img.h / 2) * img.w + img.w / 2) * 4;
        std::fprintf(stderr,
            "OK %dx%d hdr=%d kind=%s primaries=%s\n"
            "   RGB min=%.4f max=%.4f mean=%.4f nonfinite=%zu\n"
            "   ALPHA min=%.4f max=%.4f   centre RGBA=(%.3f, %.3f, %.3f, %.3f)\n",
            img.w, img.h, int(img.hdr), hdrKindName(img.kind), primariesName(img.primaries),
            mn, mx, sum / double(px * 3), nonfinite,
            amn, amx, h2f(cen), h2f(cen + 1), h2f(cen + 2), h2f(cen + 3));
        return 0;
    }

    if (path.isEmpty()) {
        // Default to the active wallpaper vantapaper records for the primary output.
        path = QDir::homePath() + QStringLiteral("/.local/state/vantapaper/wallpapers/DP-1");
    }

    HdrImage img;
    if (QFileInfo::exists(path)) {
        std::fprintf(stderr, "vantalock: decoding %s\n", qPrintable(path));
        img = decodeImage(path);
    } else {
        std::fprintf(stderr, "vantalock: image not found: %s\n", qPrintable(path));
    }

    // A lock screen must ALWAYS lock. If the wallpaper is missing or fails to
    // decode, fall back to a 1x1 black image (black background, no thumbnail) so
    // the session still locks rather than the process exiting unlocked.
    if (!img.valid()) {
        std::fprintf(stderr, "vantalock: no wallpaper; locking with a black background\n");
        img = HdrImage{};
        img.w = 1;
        img.h = 1;
        img.rgba16f = { 0x0000, 0x0000, 0x0000, 0x3C00 }; // RGBA fp16: black, alpha 1.0
    } else {
        std::fprintf(stderr, "vantalock: %dx%d, hdr=%d, kind=%s, primaries=%s\n",
            img.w, img.h, int(img.hdr), hdrKindName(img.kind), primariesName(img.primaries));
    }

    const Config cfg = Config::load();
    SessionLock lock(img, cfg);
    const bool ok = lock.run();
    std::fprintf(stderr, "vantalock: exit (clean=%d)\n", int(ok));
    return ok ? 0 : 1;
}

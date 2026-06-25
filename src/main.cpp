// VantaLock — HDR lock screen (Fase 0 spike).
//
// Decodes one image (the active vantapaper wallpaper by default), then locks the
// session via ext-session-lock-v1 and presents it in true HDR with real black on
// OLED, on every output. Esc unlocks. No authentication yet — this build only
// de-risks "session-lock + raw Vulkan + wp-color-management" on Hyprland/NVIDIA.

#include "hdr_image.h"
#include "session_lock.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QString>

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    // Decode happens via Qt's image stack; we never create a Qt window (the lock
    // surface is raw Wayland), so force the offscreen platform — no display conn.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");

    QGuiApplication app(argc, argv);

    bool probeOnly = false;
    QString path;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--probe"))
            probeOnly = true;
        else if (!a.startsWith(QStringLiteral("--")))
            path = a;
    }

    if (probeOnly) {
        HdrImage dummy; // probe needs no real image
        SessionLock lock(dummy);
        const bool ok = lock.probe();
        return ok ? 0 : 1;
    }

    if (path.isEmpty()) {
        // Default to the active wallpaper vantapaper records for the primary output.
        path = QDir::homePath() + QStringLiteral("/.local/state/vantapaper/wallpapers/DP-1");
    }

    if (!QFileInfo::exists(path)) {
        std::fprintf(stderr, "vantalock: image not found: %s\n", qPrintable(path));
        return 2;
    }

    std::fprintf(stderr, "vantalock: decoding %s\n", qPrintable(path));
    HdrImage img = decodeImage(path);
    if (!img.valid()) {
        std::fprintf(stderr, "vantalock: failed to decode %s\n", qPrintable(path));
        return 2;
    }
    std::fprintf(stderr, "vantalock: %dx%d, hdr=%d, kind=%s, primaries=%s\n",
        img.w, img.h, int(img.hdr), hdrKindName(img.kind), primariesName(img.primaries));

    SessionLock lock(img);
    const bool ok = lock.run();
    std::fprintf(stderr, "vantalock: exit (clean=%d)\n", int(ok));
    return ok ? 0 : 1;
}

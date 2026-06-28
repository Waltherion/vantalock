#include "overlay_text.h"

#include "config.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QLinearGradient>
#include <QPen>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QRectF>
#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace overlay {

namespace {
// Full-screen 16:9 canvas drawn over the whole output: UI elements are placed at
// absolute positions (clock at the top, password field near the bottom). Fixed
// size keeps the GPU texture + descriptor sets stable across refreshes.
constexpr int kW = 1920;
constexpr int kH = 1080;

QColor qc(const Color &c) { return QColor(c.r, c.g, c.b, c.a); }

// Parse a hyprlock-style "$name = rgba(RRGGBBAA)" line into a Color.
bool parseColor(const QString &hex, Color &out)
{
    bool ok = false;
    const uint32_t v = hex.toUInt(&ok, 16);
    if (!ok || hex.size() != 8)
        return false;
    out = { (unsigned char)((v >> 24) & 0xFF), (unsigned char)((v >> 16) & 0xFF),
            (unsigned char)((v >> 8) & 0xFF), (unsigned char)(v & 0xFF) };
    return true;
}
} // namespace

Theme loadTheme()
{
    Theme t;
    const QString dir = QDir::homePath() + QStringLiteral("/.config/themes/current/");
    QString path = dir + QStringLiteral("vantalock-colors.conf");
    if (!QFile::exists(path))
        path = dir + QStringLiteral("hyprlock-colors.conf");

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return t; // defaults

    static const QRegularExpression re(
        QStringLiteral("\\$(\\w+)\\s*=\\s*rgba\\(([0-9a-fA-F]{8})\\)"));
    const QString text = QString::fromUtf8(f.readAll());
    for (const QString &line : text.split('\n')) {
        const auto m = re.match(line);
        if (!m.hasMatch())
            continue;
        const QString name = m.captured(1);
        Color c;
        if (!parseColor(m.captured(2), c))
            continue;
        if (name == QStringLiteral("primary") || name == QStringLiteral("text")) {
            t.text = c;
            t.accent = c;
        } else if (name == QStringLiteral("accent")) {
            t.accent = c;
        } else if (name == QStringLiteral("error")) {
            t.error = c;
        } else if (name == QStringLiteral("shadow")) {
            c.a = 150; // keep the shadow soft regardless of theme alpha
            t.shadow = c;
        }
    }
    return t;
}

TextImage renderOverlay(const State &state, const Config &cfg, double scale)
{
    const int W = int(kW * scale + 0.5);
    const int H = int(kH * scale + 0.5);
    QImage img(W, H, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);

    const QDateTime now = QDateTime::currentDateTime();
    const QString time = now.time().toString(QStringLiteral("HH:mm"));
    const QLocale loc(QLocale::English);
    const QString weekday = loc.toString(now.date(), QStringLiteral("dddd"));
    const QString date = loc.toString(now.date(), QStringLiteral("d MMMM yyyy"));

    const QString family = QString::fromStdString(cfg.fontFamily);
    auto font = [&](int pt, QFont::Weight w) {
        QFont f;
        if (!family.isEmpty())
            f.setFamily(family);
        f.setPointSizeF(pt * scale);
        f.setWeight(w);
        return f;
    };

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Subtle drop shadow for legibility: a tight offset (so the text doesn't read as
    // "floating") and a softened alpha (the theme's $shadow is often fully opaque).
    QColor shadow = qc(cfg.shadow);
    shadow.setAlphaF(shadow.alphaF() * std::clamp(double(cfg.shadowStrength), 0.0, 1.0));

    // Optional rainbow: rainbow-able elements (clock/date, field outline + dots) are drawn
    // WHITE here; the overlay shader multiplies a rolling 45-degree band onto them (white ->
    // band colour). Dark fill/shadow stay dark. Off -> themed colours. This keeps the CPU
    // panel static, so scrolling is purely a shader uniform (no re-render / re-upload).
    const bool rainbowOn = cfg.rainbow && cfg.rainbowStops.size() >= 2;
    const QColor rainbowMask(255, 255, 255);

    auto drawCentred = [&](const QString &text, const QFont &fnt, int cy, const QColor &col,
                           bool useRainbow = false) {
        p.setFont(fnt);
        const QFontMetrics fm(fnt);
        const QRect br = fm.boundingRect(text);
        const int x = (W - br.width()) / 2 - br.left();
        const int y = cy - br.center().y();
        const int sh = qMax(0, int(cfg.shadowOffset * scale + 0.5)); // configurable, scales with resolution
        p.setPen(shadow);
        p.drawText(x + sh, y + sh, text); // soft shadow for legibility
        p.setPen(useRainbow && rainbowOn ? rainbowMask : col);
        p.drawText(x, y, text);
    };

    // Clock at the top, then weekday on its own line, then date + year below.
    drawCentred(time, font(cfg.timeSize, QFont::DemiBold), int(cfg.timeY * H), qc(cfg.text), true);
    drawCentred(weekday, font(cfg.weekdaySize, QFont::Normal), int(cfg.weekdayY * H), qc(cfg.text), true);
    drawCentred(date, font(cfg.dateSize, QFont::Normal), int(cfg.dateY * H), qc(cfg.text), true);

    // Password field: a rounded pill at cfg.fieldY (top), with dots for typed
    // characters, or a status line while verifying / after a failure.
    const int fieldW = int(cfg.fieldW * scale + 0.5), fieldH = int(cfg.fieldH * scale + 0.5);
    const QRectF field((W - fieldW) / 2.0, cfg.fieldY * H, fieldW, fieldH);
    const QColor accent = state.error ? qc(cfg.error) : qc(cfg.accent);

    const double fieldRad = fieldH * std::clamp(double(cfg.fieldRadius), 0.0, 0.5);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawRoundedRect(field, fieldRad, fieldRad);
    QPen border(rainbowOn ? QColor(255, 255, 255, 180)
                          : QColor(accent.red(), accent.green(), accent.blue(), 180));
    border.setWidthF(2.0 * scale);
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(field, fieldRad, fieldRad);

    if (state.verifying) {
        drawCentred(QStringLiteral("Verifying…"), font(cfg.fieldFontSize, QFont::Normal),
                    int(field.center().y()), accent);
    } else if (state.error && state.passwordLen == 0) {
        drawCentred(QStringLiteral("Wrong password"), font(cfg.fieldFontSize, QFont::Normal),
                    int(field.center().y()), accent);
    } else if (state.passwordLen == 0) {
        QColor dim = qc(cfg.text);
        dim.setAlpha(170);
        drawCentred(QStringLiteral("Enter password"), font(cfg.fieldFontSize, QFont::Normal),
                    int(field.center().y()), dim);
    } else {
        // Row of dots, capped so a long password stays inside the pill.
        const int shown = state.passwordLen < 16 ? state.passwordLen : 16;
        const double r = fieldH * 0.11, gap = fieldH * 0.34;
        const double totalW = (shown - 1) * gap;
        double x = field.center().x() - totalW / 2.0;
        const double y = field.center().y();
        p.setPen(Qt::NoPen);
        p.setBrush(rainbowOn ? rainbowMask : accent);
        for (int i = 0; i < shown; ++i) {
            p.drawEllipse(QPointF(x, y), r, r);
            x += gap;
        }
    }
    p.end();

    TextImage out;
    out.w = W;
    out.h = H;
    out.rgba.resize(size_t(W) * H * 4);
    std::memcpy(out.rgba.data(), img.constBits(), out.rgba.size());
    return out;
}

} // namespace overlay

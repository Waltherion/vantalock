#include "overlay_text.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QRectF>
#include <QRegularExpression>
#include <QString>

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

TextImage renderOverlay(const State &state, const Theme &theme)
{
    QImage img(kW, kH, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);

    const QDateTime now = QDateTime::currentDateTime();
    const QString time = now.time().toString(QStringLiteral("HH:mm"));
    const QString date = QLocale(QLocale::English).toString(now.date(), QStringLiteral("dddd, d MMMM"));

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    auto drawCentred = [&](const QString &text, const QFont &font, int cy, const QColor &col) {
        p.setFont(font);
        const QFontMetrics fm(font);
        const QRect br = fm.boundingRect(text);
        const int x = (kW - br.width()) / 2 - br.left();
        const int y = cy - br.center().y();
        p.setPen(qc(theme.shadow));
        p.drawText(x + 3, y + 3, text); // soft shadow for legibility
        p.setPen(col);
        p.drawText(x, y, text);
    };

    QFont timeFont;
    timeFont.setPointSize(120);
    timeFont.setWeight(QFont::DemiBold);
    QFont dateFont;
    dateFont.setPointSize(40);
    dateFont.setWeight(QFont::Normal);

    // Clock + date at the top of the screen.
    drawCentred(time, timeFont, 150, qc(theme.text));
    drawCentred(date, dateFont, 270, qc(theme.text));

    // Password field: a rounded pill near the bottom, with dots for typed
    // characters, or a status line while verifying / after a failure.
    const int fieldW = 460, fieldH = 84;
    const QRectF field((kW - fieldW) / 2.0, 900.0, fieldW, fieldH);
    const QColor accent = state.error ? qc(theme.error) : qc(theme.accent);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawRoundedRect(field, fieldH / 2.0, fieldH / 2.0);
    QPen border(QColor(accent.red(), accent.green(), accent.blue(), 180));
    border.setWidth(3);
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(field, fieldH / 2.0, fieldH / 2.0);

    if (state.verifying) {
        QFont f;
        f.setPointSize(24);
        drawCentred(QStringLiteral("Verifying…"), f, int(field.center().y()), accent);
    } else if (state.error && state.passwordLen == 0) {
        QFont f;
        f.setPointSize(24);
        drawCentred(QStringLiteral("Wrong password"), f, int(field.center().y()), accent);
    } else if (state.passwordLen == 0) {
        QFont f;
        f.setPointSize(24);
        QColor dim = qc(theme.text);
        dim.setAlpha(170);
        drawCentred(QStringLiteral("Enter password"), f, int(field.center().y()), dim);
    } else {
        // Row of dots, capped so a long password stays inside the pill.
        const int shown = state.passwordLen < 16 ? state.passwordLen : 16;
        const double r = 9.0, gap = 26.0;
        const double totalW = (shown - 1) * gap;
        double x = field.center().x() - totalW / 2.0;
        const double y = field.center().y();
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        for (int i = 0; i < shown; ++i) {
            p.drawEllipse(QPointF(x, y), r, r);
            x += gap;
        }
    }
    p.end();

    TextImage out;
    out.w = kW;
    out.h = kH;
    out.rgba.resize(size_t(kW) * kH * 4);
    std::memcpy(out.rgba.data(), img.constBits(), out.rgba.size());
    return out;
}

} // namespace overlay

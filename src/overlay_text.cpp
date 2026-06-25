#include "overlay_text.h"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QRectF>
#include <QString>

#include <cstring>

namespace overlay {

namespace {
// Full-screen 16:9 canvas drawn over the whole output: UI elements are placed at
// absolute positions (clock at the top, password field near the bottom). Fixed
// size keeps the GPU texture + descriptor sets stable across refreshes.
constexpr int kW = 1920;
constexpr int kH = 1080;
} // namespace

TextImage renderOverlay(const State &state)
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
        p.setPen(QColor(0, 0, 0, 150));
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
    drawCentred(time, timeFont, 150, QColor(255, 255, 255));
    drawCentred(date, dateFont, 270, QColor(235, 235, 235));

    // Password field: a rounded pill near the bottom, with dots for typed
    // characters, or a status line while verifying / after a failure.
    const int fieldW = 460, fieldH = 84;
    const QRectF field((kW - fieldW) / 2.0, 900.0, fieldW, fieldH);
    const QColor accent = state.error ? QColor(235, 90, 90) : QColor(255, 255, 255);

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
        drawCentred(QStringLiteral("Enter password"), f, int(field.center().y()),
                    QColor(200, 200, 200, 180));
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

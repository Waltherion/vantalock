#include "overlay_text.h"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QString>

#include <cstring>

namespace overlay {

namespace {
// Fixed canvas: text is centred within it, transparent margins are invisible. A
// constant size means the GPU texture + descriptor sets never need rebuilding.
constexpr int kW = 1200;
constexpr int kH = 600;
} // namespace

TextImage renderClock()
{
    QImage img(kW, kH, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);

    const QDateTime now = QDateTime::currentDateTime();
    const QString time = now.time().toString(QStringLiteral("HH:mm"));
    const QString date = QLocale::system().toString(now.date(), QStringLiteral("dddd d. MMMM"));

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Soft shadow under the text for legibility on bright wallpapers.
    auto drawCentred = [&](const QString &text, const QFont &font, int cy, const QColor &col) {
        p.setFont(font);
        const QFontMetrics fm(font);
        const QRect br = fm.boundingRect(text);
        const int x = (kW - br.width()) / 2 - br.left();
        const int y = cy - br.center().y();
        p.setPen(QColor(0, 0, 0, 150));
        p.drawText(x + 3, y + 3, text);
        p.setPen(col);
        p.drawText(x, y, text);
    };

    QFont timeFont;
    timeFont.setPointSize(150);
    timeFont.setWeight(QFont::DemiBold);
    QFont dateFont;
    dateFont.setPointSize(48);
    dateFont.setWeight(QFont::Normal);

    drawCentred(time, timeFont, kH * 5 / 12, QColor(255, 255, 255));
    drawCentred(date, dateFont, kH * 9 / 12, QColor(235, 235, 235));
    p.end();

    TextImage out;
    out.w = kW;
    out.h = kH;
    out.rgba.resize(size_t(kW) * kH * 4);
    std::memcpy(out.rgba.data(), img.constBits(), out.rgba.size());
    return out;
}

} // namespace overlay

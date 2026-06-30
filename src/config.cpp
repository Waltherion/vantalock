#include "config.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>

#include <cstdio>

namespace {

// The default config, written verbatim on first run (one source of truth).
const char *kDefaultConfig = R"JSONC(// VantaLock configuration (JSONC: // and /* */ comments + trailing commas OK).
// Colours default to the active Hyprland theme; set them here to override.
// Hex colours are "RRGGBBAA" (or "#RRGGBB"). Positions under "clock"/"field" are
// fractions (0..1) of a 1920x1080 reference canvas drawn across the whole screen
// (text rasterises at the output's native resolution, so it stays sharp on 4K).
//
// Every option is shown below; commented-out lines are optional knobs you can
// turn on by uncommenting them.
{
  // ---- Background: the blurred, dimmed wallpaper behind everything ----
  "background": {
    "blur": 0.02,          // blur radius in uv units (0 = sharp / off)
    "blurType": "gaussian",// style: "gaussian" (smooth, default) | "frosted" (sparse-tap glass) | "box" | "pixelate" | "none"
    "dim": 0.5            // dim multiplier in linear light (1 = none, 0 = black)
  },
  // ---- Thumbnail: the sharp, framed copy of the wallpaper ----
  "thumbnail": {
    "show": true,   // false = hide the thumbnail entirely
    "height": 0.24, // height as a fraction of screen height
    "y": 0.55,      // vertical centre as a fraction
    "radius": 0.08, // corner rounding, fraction of thumb height (0 = square)
    "border": 1     // border thickness around the thumbnail in reference px (0 = off); rides the rainbow band
  },
  // ---- Fonts: point sizes on the 1920x1080 reference (they scale to your output) ----
  "fonts": {
    "family": "",   // empty = system default
    "time": 70,     // clock (HH:mm)
    "weekday": 30,  // weekday line
    "date": 22,     // date + year line
    "field": 12     // password-field status text
  },
  // ---- Clock/date vertical positions (fractions of screen height) + formatting ----
  "clock": {
    "timeY": 0.22,
    "weekdayY": 0.37,
    "dateY": 0.41,
    "format": "24h",   // "24h" (e.g. 14:05) or "12h" (e.g. 2:05 PM)
    "locale": "",      // "" = system locale; else e.g. "da_DK", "en_US" (localises weekday/month/AM-PM)
    "dateFormat": ""   // "" = "d MMMM yyyy" (localised); else a Qt date format, e.g. "yyyy-MM-dd" or "dddd d. MMMM"
  },
  // ---- Password field ----
  "field": {
    "width": 200,
    "height": 35,
    "y": 0.68,      // field TOP as a fraction of screen height
    "radius": 0     // corner rounding as a fraction of field height (0 = sharp, 0.5 = pill)
  },
  // ---- Colours: empty string = inherit the active theme's colour ----
  "colors": {
    "text": "",     // clock / date / dots
    "accent": "",   // password-field border + dots
    "error": "",    // wrong-password feedback
    "shadow": ""    // text-shadow colour
  },

  // ---- Text shadow geometry (the colour itself is "shadow" under "colors") ----
  // "shadow": { "offset": 1.4, "strength": 0.5 }, // offset = reference px (0 = none); strength = alpha mult (0..1)

  // ---- Rainbow: a rolling 45-degree band over the clock/date, field outline + thumbnail border (off by default) ----
  // "rainbow": {
  //   "enabled": true,
  //   "stops": ["#ff5555", "#ffb86c", "#50fa7b", "#8be9fd", "#bd93f9"], // >=2 hex colours
  //   "period": 0,        // px per cycle on the 1920-wide reference (0 = span full width once; smaller = tighter repeats)
  //   "brightness": 2.5,  // band luminance (1 = faithful sRGB; >1 pushes HDR brightness so vivid colours pop)
  //   "speed": 0          // band scroll in cycles/sec (0 = static, near-zero cost; negative = reverse).
  //                       // NOTE: >0 renders continuously on the GPU while the screen is on -- on laptops prefer 0.
  // },

  // ---- Bloom: a soft glow around the text (rainbow colour, or the text colour). 0 = off ----
  // "bloom": { "strength": 1.5 } // glow amount; combined with a non-zero rainbow "speed" it adds per-frame cost

  // Optional env overrides (no file edit; handy for quick tests):
  //   VANTALOCK_BLUR=<r>   VANTALOCK_DIM=<d>      override blur / dim
  //   VANTALOCK_CM_TAG=1   force a colour-management tag (if black looks grey)
  //   VANTALOCK_TIMEOUT=<seconds>  safety auto-unlock (testing only)
}
)JSONC";

QByteArray stripJsonComments(const QByteArray &in)
{
    static const QRegularExpression block(QStringLiteral("/\\*.*?\\*/"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression line(QStringLiteral("//[^\n]*"));
    static const QRegularExpression trailingComma(QStringLiteral(",(\\s*[}\\]])"));
    QString s = QString::fromUtf8(in);
    s.remove(block);
    s.remove(line);
    s.replace(trailingComma, QStringLiteral("\\1"));
    return s.toUtf8();
}

bool parseColor(const QString &in, overlay::Color &out)
{
    QString h = in.trimmed();
    if (h.isEmpty())
        return false;
    if (h.startsWith(QLatin1Char('#')))
        h = h.mid(1);
    static const QRegularExpression rgba(QStringLiteral("rgba?\\(([0-9a-fA-F]{6,8})\\)"));
    const auto m = rgba.match(h);
    if (m.hasMatch())
        h = m.captured(1);
    if (h.size() == 6)
        h += QStringLiteral("ff");
    if (h.size() != 8)
        return false;
    bool ok = false;
    const uint32_t v = h.toUInt(&ok, 16);
    if (!ok)
        return false;
    out = { (unsigned char)((v >> 24) & 0xFF), (unsigned char)((v >> 16) & 0xFF),
            (unsigned char)((v >> 8) & 0xFF), (unsigned char)(v & 0xFF) };
    return true;
}

} // namespace

std::string Config::configPath()
{
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");
    return (base + QStringLiteral("/vantalock/config.jsonc")).toStdString();
}

Config Config::load()
{
    Config cfg;

    // Colours default to the active theme.
    const overlay::Theme theme = overlay::loadTheme();
    cfg.text = theme.text;
    cfg.accent = theme.accent;
    cfg.error = theme.error;
    cfg.shadow = theme.shadow;

    const QString path = QString::fromStdString(configPath());

    // Write the commented default on first run.
    if (!QFile::exists(path)) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(kDefaultConfig);
            f.close();
        }
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return cfg;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(stripJsonComments(f.readAll()), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        std::fprintf(stderr, "vantalock: config parse error: %s (using defaults)\n",
            qPrintable(err.errorString()));
        return cfg;
    }
    const QJsonObject root = doc.object();

    auto section = [&](const char *name) { return root.value(QLatin1String(name)).toObject(); };

    const QJsonObject bg = section("background");
    cfg.blur = float(bg.value("blur").toDouble(cfg.blur));
    cfg.dim = float(bg.value("dim").toDouble(cfg.dim));
    const QString bt = bg.value("blurType").toString().trimmed().toLower();
    if (bt == QLatin1String("gaussian"))      cfg.blurType = 1;
    else if (bt == QLatin1String("box"))      cfg.blurType = 2;
    else if (bt == QLatin1String("pixelate")) cfg.blurType = 3;
    else if (bt == QLatin1String("none"))     cfg.blurType = 4;
    else if (bt == QLatin1String("frosted"))  cfg.blurType = 0;
    // empty/unknown -> keep the default (frosted)

    const QJsonObject th = section("thumbnail");
    cfg.thumbShow = th.value("show").toBool(cfg.thumbShow);
    cfg.thumbHeight = float(th.value("height").toDouble(cfg.thumbHeight));
    cfg.thumbY = float(th.value("y").toDouble(cfg.thumbY));
    cfg.thumbRadius = float(th.value("radius").toDouble(cfg.thumbRadius));
    cfg.thumbBorder = float(th.value("border").toDouble(cfg.thumbBorder));

    const QJsonObject ft = section("fonts");
    cfg.fontFamily = ft.value("family").toString().toStdString();
    cfg.timeSize = ft.value("time").toInt(cfg.timeSize);
    cfg.weekdaySize = ft.value("weekday").toInt(cfg.weekdaySize);
    cfg.dateSize = ft.value("date").toInt(cfg.dateSize);
    cfg.fieldFontSize = ft.value("field").toInt(cfg.fieldFontSize);

    const QJsonObject ck = section("clock");
    cfg.timeY = float(ck.value("timeY").toDouble(cfg.timeY));
    cfg.weekdayY = float(ck.value("weekdayY").toDouble(cfg.weekdayY));
    cfg.dateY = float(ck.value("dateY").toDouble(cfg.dateY));
    cfg.timeFormat = ck.value("format").toString(QString::fromStdString(cfg.timeFormat)).toStdString();
    cfg.locale = ck.value("locale").toString().toStdString();
    cfg.dateFormat = ck.value("dateFormat").toString().toStdString();

    const QJsonObject fl = section("field");
    cfg.fieldW = fl.value("width").toInt(cfg.fieldW);
    cfg.fieldH = fl.value("height").toInt(cfg.fieldH);
    cfg.fieldY = float(fl.value("y").toDouble(cfg.fieldY));
    cfg.fieldRadius = float(fl.value("radius").toDouble(cfg.fieldRadius));

    const QJsonObject co = section("colors");
    overlay::Color c;
    if (parseColor(co.value("text").toString(), c)) cfg.text = c;
    if (parseColor(co.value("accent").toString(), c)) cfg.accent = c;
    if (parseColor(co.value("error").toString(), c)) cfg.error = c;
    if (parseColor(co.value("shadow").toString(), c)) cfg.shadow = c;

    const QJsonObject sh = section("shadow");
    cfg.shadowOffset = float(sh.value("offset").toDouble(cfg.shadowOffset));
    cfg.shadowStrength = float(sh.value("strength").toDouble(cfg.shadowStrength));

    const QJsonObject rb = section("rainbow");
    cfg.rainbow = rb.value("enabled").toBool(cfg.rainbow);
    cfg.rainbowPeriod = float(rb.value("period").toDouble(cfg.rainbowPeriod));
    cfg.rainbowBrightness = float(rb.value("brightness").toDouble(cfg.rainbowBrightness));
    cfg.rainbowSpeed = float(rb.value("speed").toDouble(cfg.rainbowSpeed));

    const QJsonObject bl = section("bloom");
    cfg.bloomStrength = float(bl.value("strength").toDouble(cfg.bloomStrength));
    const QJsonArray stops = rb.value("stops").toArray();
    if (!stops.isEmpty()) {
        cfg.rainbowStops.clear();
        overlay::Color sc;
        for (const QJsonValue &v : stops)
            if (parseColor(v.toString(), sc)) cfg.rainbowStops.push_back(sc);
    }

    return cfg;
}

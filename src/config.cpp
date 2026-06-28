#include "config.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
// fractions (0..1) of a 1920x1080 reference canvas drawn across the whole screen.
{
  "background": {
    "blur": 0.02,         // blur radius (0 = sharp)
    "blurType": "frosted",// "frosted" (sparse-tap glass), "gaussian" (smooth), "box", "pixelate", "none"
    "dim": 0.5            // dim multiplier (1 = none)
  },
  "thumbnail": {
    "show": true,
    "height": 0.24, // fraction of screen height
    "y": 0.55,      // vertical centre
    "radius": 0.08  // corner rounding, fraction of thumb height (0 = square)
  },
  "fonts": {
    "family": "",   // empty = system default
    "time": 120,
    "weekday": 50,
    "date": 38,
    "field": 18
  },
  "clock": {
    "timeY": 0.14,
    "weekdayY": 0.27,
    "dateY": 0.34
  },
  "field": {
    "width": 300,
    "height": 52,
    "y": 0.68       // field TOP
  },
  "colors": {
    "text": "",     // empty = theme colour
    "accent": "",
    "error": "",
    "shadow": ""
  }
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

    const QJsonObject fl = section("field");
    cfg.fieldW = fl.value("width").toInt(cfg.fieldW);
    cfg.fieldH = fl.value("height").toInt(cfg.fieldH);
    cfg.fieldY = float(fl.value("y").toDouble(cfg.fieldY));

    const QJsonObject co = section("colors");
    overlay::Color c;
    if (parseColor(co.value("text").toString(), c)) cfg.text = c;
    if (parseColor(co.value("accent").toString(), c)) cfg.accent = c;
    if (parseColor(co.value("error").toString(), c)) cfg.error = c;
    if (parseColor(co.value("shadow").toString(), c)) cfg.shadow = c;

    return cfg;
}

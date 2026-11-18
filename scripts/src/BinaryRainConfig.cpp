#include "BinaryRainConfig.h"

#include <QCoreApplication>
#include <QSettings>
#include <algorithm>

namespace BinaryRain {

namespace {

struct PresetDef {
    Preset id;
    const char* key;
    const char* name;
    Params params;
};

// Single source of truth for preset values. Order = Preset enum order.
// Params order: density, speed, charSize, brightness, glow, backgroundDarkness.
const PresetDef kPresets[] = {
    {Preset::Classic, "classic", QT_TRANSLATE_NOOP("BinaryRain", "Classic Matrix"), {60, 100, 18, 80, 50, 90}},
    {Preset::Dense, "dense", QT_TRANSLATE_NOOP("BinaryRain", "Dense Matrix"), {90, 120, 14, 85, 60, 92}},
    {Preset::Minimal, "minimal", QT_TRANSLATE_NOOP("BinaryRain", "Minimal Matrix"), {25, 70, 20, 50, 20, 95}},
    {Preset::Neon, "neon", QT_TRANSLATE_NOOP("BinaryRain", "Neon Matrix"), {70, 110, 18, 100, 90, 88}},
    {Preset::Dark, "dark", QT_TRANSLATE_NOOP("BinaryRain", "Dark Matrix"), {55, 90, 18, 45, 25, 100}},
};

struct ParamDef {
    Param id;
    const char* key;
    const char* label;
    ParamRange range;
};

const ParamDef kParams[] = {
    {Param::Density, "density", QT_TRANSLATE_NOOP("BinaryRain", "Binary density"), {5, 100}},
    {Param::Speed, "speed", QT_TRANSLATE_NOOP("BinaryRain", "Falling speed"), {10, 300}},
    {Param::CharSize, "charSize", QT_TRANSLATE_NOOP("BinaryRain", "Character size"), {8, 64}},
    {Param::Brightness, "brightness", QT_TRANSLATE_NOOP("BinaryRain", "Character brightness"), {10, 100}},
    {Param::Glow, "glow", QT_TRANSLATE_NOOP("BinaryRain", "Glow intensity"), {0, 100}},
    {Param::BackgroundDarkness, "backgroundDarkness", QT_TRANSLATE_NOOP("BinaryRain", "Background darkness"), {0, 100}},
};

const ParamDef& paramDef(Param p) {
    return kParams[static_cast<int>(p)];
}

} // namespace

bool Params::operator==(const Params& o) const {
    return density == o.density && speed == o.speed && charSize == o.charSize && brightness == o.brightness &&
           glow == o.glow && backgroundDarkness == o.backgroundDarkness;
}

int Params::get(Param p) const {
    switch (p) {
    case Param::Density: return density;
    case Param::Speed: return speed;
    case Param::CharSize: return charSize;
    case Param::Brightness: return brightness;
    case Param::Glow: return glow;
    case Param::BackgroundDarkness: return backgroundDarkness;
    }
    return 0;
}

void Params::set(Param p, int value) {
    switch (p) {
    case Param::Density: density = value; break;
    case Param::Speed: speed = value; break;
    case Param::CharSize: charSize = value; break;
    case Param::Brightness: brightness = value; break;
    case Param::Glow: glow = value; break;
    case Param::BackgroundDarkness: backgroundDarkness = value; break;
    }
}

ParamRange rangeOf(Param p) {
    return paramDef(p).range;
}

int clampParam(Param p, int value) {
    const ParamRange r = rangeOf(p);
    return std::clamp(value, r.min, r.max);
}

Params validated(Params p) {
    for (int i = 0; i < kParamCount; ++i) {
        const Param id = static_cast<Param>(i);
        p.set(id, clampParam(id, p.get(id)));
    }
    return p;
}

const char* paramKey(Param p) {
    return paramDef(p).key;
}

QString paramLabel(Param p) {
    return QCoreApplication::translate("BinaryRain", paramDef(p).label);
}

Params presetParams(Preset preset) {
    for (const PresetDef& d : kPresets) {
        if (d.id == preset) {
            return d.params;
        }
    }
    return kPresets[0].params;
}

const char* presetKey(Preset preset) {
    for (const PresetDef& d : kPresets) {
        if (d.id == preset) {
            return d.key;
        }
    }
    return "custom";
}

QString presetName(Preset preset) {
    for (const PresetDef& d : kPresets) {
        if (d.id == preset) {
            return QCoreApplication::translate("BinaryRain", d.name);
        }
    }
    return QCoreApplication::translate("BinaryRain", "Custom");
}

bool presetFromKey(const QString& key, Preset* out) {
    for (const PresetDef& d : kPresets) {
        if (key == QLatin1String(d.key)) {
            if (out) *out = d.id;
            return true;
        }
    }
    if (key == QLatin1String("custom")) {
        if (out) *out = Preset::Custom;
        return true;
    }
    return false;
}

void Config::applyPreset(Preset p) {
    preset = p;
    if (p != Preset::Custom) {
        params = presetParams(p);
    }
}

bool Config::setParam(Param p, int value) {
    const int clamped = clampParam(p, value);
    if (params.get(p) == clamped) {
        return false;
    }
    params.set(p, clamped);
    preset = Preset::Custom;
    return true;
}

void Config::resetToDefaults() {
    applyPreset(Preset::Classic);
}

void Config::validate() {
    params = validated(params);
    if (preset != Preset::Custom) {
        params = presetParams(preset);
    }
}

Config Config::fromSettings(const QSettings& settings) {
    Config config;
    Preset preset = Preset::Classic;
    if (!presetFromKey(settings.value(QStringLiteral("binaryRain/preset")).toString(), &preset)) {
        preset = Preset::Classic;
    }
    config.preset = preset;
    for (int i = 0; i < kParamCount; ++i) {
        const Param param = static_cast<Param>(i);
        bool ok = false;
        const int value =
            settings.value(QStringLiteral("binaryRain/") + QLatin1String(paramKey(param))).toInt(&ok);
        if (ok) {
            config.params.set(param, value);
        }
    }
    config.validate();
    return config;
}

void Config::toSettings(QSettings& settings) const {
    Config c = *this;
    c.validate();
    settings.setValue(QStringLiteral("binaryRain/preset"), QLatin1String(presetKey(c.preset)));
    for (int i = 0; i < kParamCount; ++i) {
        const Param param = static_cast<Param>(i);
        settings.setValue(QStringLiteral("binaryRain/") + QLatin1String(paramKey(param)), c.params.get(param));
    }
}

} // namespace BinaryRain

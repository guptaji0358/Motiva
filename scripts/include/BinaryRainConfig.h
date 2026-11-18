#pragma once

#include <QString>

class QSettings;

// Pure data model for the procedural "Binary Rain" wallpaper: parameter
// ranges, the five presets (defined once, in BinaryRainConfig.cpp), validation
// and the "any edit turns the preset into Custom" rule. No Qt GUI, Win32 or
// storage dependency, so it is unit-testable on its own. Persistence lives in
// SettingsManager (binaryRainConfig/setBinaryRainConfig).
namespace BinaryRain {

enum class Preset { Classic = 0, Dense, Minimal, Neon, Dark, Custom };
constexpr int kPresetCount = 6; // including Custom

enum class Param { Density = 0, Speed, CharSize, Brightness, Glow, BackgroundDarkness };
constexpr int kParamCount = 6;

struct Params {
    int density = 60;            // % of columns carrying a stream
    int speed = 100;             // % of the baseline fall speed
    int charSize = 18;           // glyph cell height, px at a 1080-line frame
    int brightness = 80;         // % character brightness
    int glow = 50;               // % glow intensity
    int backgroundDarkness = 90; // % (100 = pure black)

    bool operator==(const Params& o) const;
    bool operator!=(const Params& o) const { return !(*this == o); }
    int get(Param p) const;
    void set(Param p, int value); // raw store, no clamping
};

struct ParamRange {
    int min;
    int max;
};
ParamRange rangeOf(Param p);
int clampParam(Param p, int value);
Params validated(Params p); // every field clamped into its range
const char* paramKey(Param p); // stable storage key suffix, e.g. "density"
QString paramLabel(Param p);

// The preset table: the single place preset values are defined.
Params presetParams(Preset preset); // Custom -> Classic values
const char* presetKey(Preset preset); // stable storage id, e.g. "classic"
QString presetName(Preset preset);
bool presetFromKey(const QString& key, Preset* out);

// Active configuration = which preset it came from + the effective values.
struct Config {
    Preset preset = Preset::Classic;
    Params params = presetParams(Preset::Classic);

    // Applies a preset's complete configuration (Custom keeps current values).
    void applyPreset(Preset p);
    // Sets one parameter (clamped). Returns true if the value changed; any
    // change makes the configuration Custom.
    bool setParam(Param p, int value);
    // Back to the defaults (Classic Matrix).
    void resetToDefaults();
    // Clamps values and repairs an inconsistent preset label: a named preset
    // always carries exactly that preset's values.
    void validate();

    // Persistence under "binaryRain/*" in any QSettings (SettingsManager
    // passes its own). Loading validates: unknown preset -> Classic,
    // out-of-range values clamped, a named preset takes its table values.
    static Config fromSettings(const QSettings& settings);
    void toSettings(QSettings& settings) const;
};

} // namespace BinaryRain

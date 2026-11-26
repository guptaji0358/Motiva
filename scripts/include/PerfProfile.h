#pragma once
// Cloud performance profile: pure parsing / validation / signature check.
// No UI, no network, no wallpaper code. A profile can only set the allowlisted
// numeric parameters below, each inside a hard range; it carries no code.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

namespace perf {

// Existing, verified configuration points a profile may tune (defaults are the
// hard-coded values Motiva shipped with, so "no profile" == previous behaviour).
enum class Param {
    VerifyWaitMs,      // WallpaperManager post-attach frame check (default 700)
    AttachRetryMs,     // WallpaperManager attach retry poll (default 300)
    DownloadTimeoutMs, // WebMediaCache transfer timeout (default 60000)
    Count
};

struct ParamSpec {
    const char* name;
    int def;
    int min;
    int max;
};
const ParamSpec& spec(Param p);
int paramCount();

struct HardwareClass {
    QString gpuVendor = QStringLiteral("unknown"); // nvidia | amd | intel | other | unknown
    int ramGb = 0;
};

struct Rule {
    QString gpuVendor; // empty = any
    int minRamGb = 0;
    QHash<int, int> params; // Param -> value
};

struct Profile {
    int version = 0;
    qint64 issuedAt = 0;
    qint64 expiresAt = 0;
    QVector<Rule> rules;
};

struct ParseResult {
    bool ok = false;
    QString error;
    Profile profile;
};

constexpr int kSchemaVersion = 1;
constexpr int kMaxEnvelopeBytes = 64 * 1024;

// Envelope: {"payload": base64(JSON), "signature": base64(64-byte r||s)}.
// Rejects: oversize, malformed, unsigned, bad signature (ECDSA P-256/SHA-256
// over the exact payload bytes), wrong schema, not yet valid, expired, unknown
// parameter names, out-of-range values, too many rules.
ParseResult verifyAndParse(const QByteArray& envelope, const QByteArray& publicKeyXY64, qint64 nowSecs);

// Flattens the rules matching this hardware (later rules win) -> Param -> value.
QHash<int, int> resolve(const Profile& profile, const HardwareClass& hw);

// Raw signature primitive (exposed for tests).
bool verifyEcdsaP256(const QByteArray& message, const QByteArray& signature64, const QByteArray& publicKeyXY64);

// Development placeholder key (matches scripts/tools/mock-perf/dev_private_key.pem).
// Production distribution stays disabled until this is replaced by a real key.
QByteArray placeholderPublicKey();
constexpr bool kProductionKeyConfigured = false;

} // namespace perf

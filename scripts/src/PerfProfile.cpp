#include "PerfProfile.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <windows.h>
#include <bcrypt.h>
#endif

namespace perf {

namespace {
const ParamSpec kSpecs[] = {
    {"verifyWaitMs", 700, 300, 1500},
    {"attachRetryMs", 300, 150, 1000},
    {"downloadTimeoutMs", 60000, 15000, 120000},
};
static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == static_cast<size_t>(Param::Count), "spec table size");

ParseResult fail(const QString& why) {
    ParseResult r;
    r.error = why;
    return r;
}
} // namespace

const ParamSpec& spec(Param p) {
    return kSpecs[static_cast<int>(p)];
}
int paramCount() {
    return static_cast<int>(Param::Count);
}

QByteArray placeholderPublicKey() {
    return QByteArray::fromHex("536b9a0c9896f7a24a4cf2da63d5e23883226b9e35f8f68418438090004094ca"
                               "72b25e731f1867531719de13fd988e41e0b4902f06df5140dba90d649c20c7ee");
}

bool verifyEcdsaP256(const QByteArray& message, const QByteArray& signature64, const QByteArray& publicKeyXY64) {
#ifdef Q_OS_WIN
    if (signature64.size() != 64 || publicKeyXY64.size() != 64) {
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    QByteArray blob(sizeof(BCRYPT_ECCKEY_BLOB), '\0');
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    blob.append(publicKeyXY64);
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, reinterpret_cast<PUCHAR>(blob.data()),
                            static_cast<ULONG>(blob.size()), 0) == 0) {
        QByteArray digest = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
        ok = BCryptVerifySignature(key, nullptr, reinterpret_cast<PUCHAR>(digest.data()),
                                   static_cast<ULONG>(digest.size()),
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(signature64.constData())),
                                   static_cast<ULONG>(signature64.size()), 0) == 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
#else
    Q_UNUSED(message) Q_UNUSED(signature64) Q_UNUSED(publicKeyXY64)
    return false;
#endif
}

ParseResult verifyAndParse(const QByteArray& envelope, const QByteArray& publicKeyXY64, qint64 nowSecs) {
    if (envelope.isEmpty() || envelope.size() > kMaxEnvelopeBytes) {
        return fail(QStringLiteral("envelope size invalid"));
    }
    const QJsonDocument envDoc = QJsonDocument::fromJson(envelope);
    if (!envDoc.isObject()) {
        return fail(QStringLiteral("envelope is not JSON"));
    }
    const QJsonObject env = envDoc.object();
    const QByteArray payload = QByteArray::fromBase64(env.value(QStringLiteral("payload")).toString().toLatin1());
    const QByteArray sig = QByteArray::fromBase64(env.value(QStringLiteral("signature")).toString().toLatin1());
    if (payload.isEmpty()) {
        return fail(QStringLiteral("missing payload"));
    }
    if (sig.isEmpty()) {
        return fail(QStringLiteral("unsigned profile"));
    }
    if (!verifyEcdsaP256(payload, sig, publicKeyXY64)) {
        return fail(QStringLiteral("signature check failed"));
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        return fail(QStringLiteral("payload is not JSON"));
    }
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("schema")).toInt(-1) != kSchemaVersion) {
        return fail(QStringLiteral("unsupported schema"));
    }
    Profile p;
    p.version = o.value(QStringLiteral("version")).toInt(0);
    p.issuedAt = static_cast<qint64>(o.value(QStringLiteral("issuedAt")).toDouble(0));
    p.expiresAt = static_cast<qint64>(o.value(QStringLiteral("expiresAt")).toDouble(0));
    if (p.version <= 0) {
        return fail(QStringLiteral("invalid version"));
    }
    if (p.issuedAt <= 0 || p.expiresAt <= p.issuedAt) {
        return fail(QStringLiteral("invalid validity window"));
    }
    if (nowSecs + 300 < p.issuedAt) {
        return fail(QStringLiteral("profile not yet valid"));
    }
    if (nowSecs >= p.expiresAt) {
        return fail(QStringLiteral("profile expired"));
    }
    const QJsonArray rules = o.value(QStringLiteral("rules")).toArray();
    if (rules.size() > 32) {
        return fail(QStringLiteral("too many rules"));
    }
    for (const QJsonValue& rv : rules) {
        const QJsonObject ro = rv.toObject();
        const QJsonObject match = ro.value(QStringLiteral("match")).toObject();
        const QJsonObject params = ro.value(QStringLiteral("params")).toObject();
        Rule rule;
        rule.gpuVendor = match.value(QStringLiteral("gpuVendor")).toString();
        if (rule.gpuVendor == QLatin1String("any")) {
            rule.gpuVendor.clear();
        }
        rule.minRamGb = match.value(QStringLiteral("minRamGb")).toInt(0);
        for (auto it = params.begin(); it != params.end(); ++it) {
            int idx = -1;
            for (int i = 0; i < paramCount(); ++i) {
                if (it.key() == QLatin1String(kSpecs[i].name)) {
                    idx = i;
                }
            }
            if (idx < 0) {
                return fail(QStringLiteral("parameter not allowed: ") + it.key());
            }
            if (!it.value().isDouble()) {
                return fail(QStringLiteral("parameter not numeric: ") + it.key());
            }
            const double d = it.value().toDouble();
            // Range first: casting an out-of-range double to int is undefined behaviour.
            if (!(d >= kSpecs[idx].min && d <= kSpecs[idx].max) || d != static_cast<int>(d)) {
                return fail(QStringLiteral("parameter out of range: ") + it.key());
            }
            const int v = static_cast<int>(d);
            rule.params.insert(idx, v);
        }
        p.rules.push_back(rule);
    }
    ParseResult r;
    r.ok = true;
    r.profile = p;
    return r;
}

QHash<int, int> resolve(const Profile& profile, const HardwareClass& hw) {
    QHash<int, int> out;
    for (const Rule& r : profile.rules) {
        if (!r.gpuVendor.isEmpty() && r.gpuVendor != hw.gpuVendor) {
            continue;
        }
        if (hw.ramGb < r.minRamGb) {
            continue;
        }
        for (auto it = r.params.begin(); it != r.params.end(); ++it) {
            out.insert(it.key(), it.value());
        }
    }
    return out;
}

} // namespace perf

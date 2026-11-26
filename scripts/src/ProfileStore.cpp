#include "ProfileStore.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <atomic>

namespace {
std::atomic<int> g_values[static_cast<int>(perf::Param::Count)];
std::atomic<bool> g_valuesInit{false};

void ensureDefaults() {
    bool expected = false;
    if (g_valuesInit.compare_exchange_strong(expected, true)) {
        for (int i = 0; i < perf::paramCount(); ++i) {
            g_values[i].store(perf::spec(static_cast<perf::Param>(i)).def);
        }
    }
}

bool writeAtomic(const QString& path, const QByteArray& data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(data);
    return f.commit();
}
} // namespace

ProfileStore::ProfileStore(QObject* parent) : QObject(parent) {
    ensureDefaults();
}

ProfileStore& ProfileStore::instance() {
    static ProfileStore store;
    return store;
}

int ProfileStore::value(perf::Param p) {
    ensureDefaults();
    return g_values[static_cast<int>(p)].load(std::memory_order_relaxed);
}

qint64 ProfileStore::now() const {
    return m_now ? m_now() : QDateTime::currentSecsSinceEpoch();
}

bool ProfileStore::write(const QString& path, const QByteArray& data) {
    return m_writer ? m_writer(path, data) : writeAtomic(path, data);
}

ProfileStore::InstallResult ProfileStore::fail(const QString& why, QString* error) {
    m_lastError = why;
    if (error) {
        *error = why;
    }
    qWarning() << "[Perf] profile not installed:" << why;
    return InstallResult::StoreFailed;
}

void ProfileStore::applyProfile(const perf::Profile* p, bool newIdentity) {
    if (newIdentity) {
        ++m_generation; // a verification begun under the previous profile must not confirm this one
    }
    const QHash<int, int> resolved = p ? perf::resolve(*p, m_hw) : QHash<int, int>();
    for (int i = 0; i < perf::paramCount(); ++i) {
        const perf::ParamSpec& s = perf::spec(static_cast<perf::Param>(i));
        const int v = resolved.value(i, s.def);
        // Defence in depth: the verifier already range-checked, never trust twice.
        g_values[i].store(qBound(s.min, v, s.max), std::memory_order_relaxed);
    }
    m_activeVersion = p ? p->version : 0;
    if (p) {
        m_active = *p;
    } else {
        m_active = perf::Profile();
    }
}

bool ProfileStore::loadFile(const QString& name, perf::Profile* out, QByteArray* raw) const {
    QFile f(m_dir + QLatin1Char('/') + name);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = f.readAll();
    const perf::ParseResult r = perf::verifyAndParse(bytes, m_key, now());
    if (!r.ok) {
        qWarning() << "[Perf] stored profile" << name << "rejected:" << r.error;
        return false;
    }
    *out = r.profile;
    if (raw) {
        *raw = bytes;
    }
    return true;
}

void ProfileStore::loadState() {
    m_trial = false;
    m_cleanExit = false;
    m_trialVersion = 0;
    m_badVersions.clear();
    QFile f(m_dir + QStringLiteral("/state.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    m_trial = o.value(QStringLiteral("trial")).toBool();
    m_cleanExit = o.value(QStringLiteral("cleanExit")).toBool();
    m_trialVersion = o.value(QStringLiteral("trialVersion")).toInt();
    for (const QJsonValue& v : o.value(QStringLiteral("bad")).toArray()) {
        m_badVersions.insert(v.toInt());
    }
}

bool ProfileStore::saveState() {
    QJsonArray bad;
    for (int v : m_badVersions) {
        bad.append(v);
    }
    QJsonObject o{{QStringLiteral("trial"), m_trial}, {QStringLiteral("cleanExit"), m_cleanExit},
                {QStringLiteral("trialVersion"), m_trialVersion}, {QStringLiteral("bad"), bad}};
    if (!write(m_dir + QStringLiteral("/state.json"), QJsonDocument(o).toJson(QJsonDocument::Compact))) {
        m_lastError = QStringLiteral("could not write state.json");
        qWarning() << "[Perf]" << m_lastError;
        return false;
    }
    return true;
}

void ProfileStore::init(const QString& dir, const QByteArray& publicKey, const perf::HardwareClass& hw) {
    m_dir = dir;
    m_key = publicKey;
    m_hw = hw;
    QDir().mkpath(m_dir);
    loadState();
    if (m_trial && m_trialVersion > 0) {
        perf::Profile stored;
        if (!loadFile(QStringLiteral("active.json"), &stored) || stored.version != m_trialVersion) {
            // install() persisted the trial first and was interrupted before the new profile became active:
            // nothing was trialled, so nothing is rolled back or marked bad.
            qWarning() << "[Perf] interrupted install of version" << m_trialVersion << "- no trial was running";
            m_trial = false;
            m_cleanExit = false;
            m_trialVersion = 0;
            saveState(); // failure is harmless: the same record is recognised as interrupted again next start
        }
    }
    if (m_trial && m_cleanExit) {
        // Exited normally before any verified wallpaper: unproven, not failed. Keep trialling - but only if
        // clearing the clean-exit marker is durable. If it is not, a later crash would be mistaken for a clean
        // exit and the unproven profile would survive it, so fall through to the rollback instead.
        m_cleanExit = false;
        perf::Profile t;
        if (saveState() && loadFile(QStringLiteral("active.json"), &t)) {
            applyProfile(&t);
            qInfo() << "[Perf] continuing unconfirmed trial of version" << t.version;
            return;
        }
    }
    if (m_trial) {
        // The last run activated a profile and never reached a healthy wallpaper.
        qWarning() << "[Perf] previous profile trial did not complete - rolling back";
        rollback();
        return;
    }
    perf::Profile p;
    if (loadFile(QStringLiteral("active.json"), &p)) {
        applyProfile(&p);
        qInfo() << "[Perf] active profile version" << p.version;
    } else {
        applyProfile(nullptr);
        if (QFile::exists(m_dir + QStringLiteral("/active.json")) && rollback()) {
            return; // stored profile expired/invalid: fall back to the previous one
        }
    }
}

void ProfileStore::setHardware(const perf::HardwareClass& hw) {
    m_hw = hw;
    if (m_activeVersion > 0) {
        applyProfile(&m_active, false); // same profile, re-resolved: not a new identity
    }
}

ProfileStore::InstallResult ProfileStore::install(const QByteArray& envelope, QString* error) {
    const perf::ParseResult r = perf::verifyAndParse(envelope, m_key, now());
    if (!r.ok) {
        if (error) {
            *error = r.error;
        }
        qWarning() << "[Perf] profile rejected:" << r.error;
        return InstallResult::Rejected;
    }
    if (m_badVersions.contains(r.profile.version)) {
        if (error) {
            *error = QStringLiteral("version previously failed");
        }
        return InstallResult::KnownBad;
    }
    if (m_trial) {
        if (error) {
            *error = QStringLiteral("earlier profile trial unconfirmed");
        }
        return InstallResult::TrialActive; // previous.json (last known-good) stays untouched
    }
    if (r.profile.version <= m_activeVersion) {
        if (error) {
            *error = QStringLiteral("not newer");
        }
        return InstallResult::NotNewer;
    }
    // Keep the working profile as the rollback target, then activate the new one.
    const QString active = m_dir + QStringLiteral("/active.json");
    const QString previous = m_dir + QStringLiteral("/previous.json");
    // Step 1: rollback copy. Atomic replace (never remove-then-copy), so a failure keeps the old previous.json.
    if (QFile::exists(active)) {
        QFile af(active);
        if (!af.open(QIODevice::ReadOnly)) {
            return fail(QStringLiteral("could not read the active profile for the rollback copy"), error);
        }
        const QByteArray current = af.readAll();
        af.close();
        if (!write(previous, current)) {
            return fail(QStringLiteral("could not store the rollback profile"), error);
        }
    } else {
        if (QFile::exists(previous) && !QFile::remove(previous)) { // a stale one would become a rollback target
            return fail(QStringLiteral("could not clear a stale rollback profile"), error);
        }
    }
    // Step 2: the trial record is persisted BEFORE active.json changes: a crash between the two writes leaves the
    // old active profile plus a trial record that init() recognises as "interrupted install" (see ProfileStore.h).
    // If the record cannot be made durable no trial is started and active.json is not touched at all.
    m_trial = true;
    m_cleanExit = false;
    m_trialVersion = r.profile.version;
    if (!saveState()) {
        m_trial = false;
        m_trialVersion = 0;
        return fail(QStringLiteral("could not persist the trial state"), error);
    }
    // Step 3: activate. On failure undo the trial record; if even that cannot be written the leftover record
    // names a version that is not in active.json, which init() recovers as an interrupted install.
    if (!write(active, envelope)) {
        m_trial = false;
        m_trialVersion = 0;
        if (!saveState()) {
            qWarning() << "[Perf] could not clear the trial record after a failed activation; init() recovers it";
        }
        return fail(QStringLiteral("could not store the profile"), error);
    }
    applyProfile(&r.profile);
    qInfo() << "[Perf] activated profile version" << r.profile.version << "(trial)";
    return InstallResult::Activated;
}

// Restores the known-good profile on disk in an order where a crash after ANY step leaves a recoverable state in
// which the failed version is never healthy and never eligible for another trial:
//   1. failed version -> bad list, persisted while the trial record is still in place (trial=true, cleanExit=false)
//   2. active.json := previous.json (atomic replace) or removed
//   3. trial cleared (state.json: trial=false; the bad list is already durable, never rewritten without it)
//   4. previous.json removed (cleanup only; a leftover copy equals active.json and is overwritten by the next install)
// Crash after 1: init() sees an unproven trial whose active.json still holds that version -> repeats the rollback.
// Crash after 2: init() sees a trial record naming a version active.json no longer holds -> "interrupted", and the
// version is already in the bad list. Crash after 3 or 4: nothing pending. A failed write at any step returns false and
// leaves the earlier durable state in place; the caller keeps m_rollbackPending so nothing ends the trial.
bool ProfileStore::restoreKnownGood(const QByteArray* prevRaw) {
    const QString active = m_dir + QStringLiteral("/active.json");
    const QString previous = m_dir + QStringLiteral("/previous.json");
    const int trialVersion = m_trialVersion;
    m_cleanExit = false;
    if (!saveState()) { // step 1: the failed-version marker must be durable before anything else changes
        return false;
    }
    const bool restored = prevRaw ? write(active, *prevRaw) : (!QFile::exists(active) || QFile::remove(active)); // step 2
    if (!restored) {
        m_lastError = QStringLiteral("could not restore active.json during rollback");
        return false;
    }
    m_trial = false; // step 3
    m_trialVersion = 0;
    if (!saveState()) {
        m_trial = true; // still recorded as an unresolved trial on disk: keep memory identical
        m_trialVersion = trialVersion;
        return false;
    }
    QFile::remove(previous); // step 4
    return true;
}

bool ProfileStore::rollback() {
    const int failed = m_activeVersion;
    if (failed > 0) {
        m_badVersions.insert(failed);
        if (m_trial && m_trialVersion == 0) {
            m_trialVersion = failed; // legacy state: name the version in the record so an interrupted rollback is recognised
        }
    } else if (m_trial) {
        // Trial profile not loaded yet (startup path): mark the version the trial record names, else whatever is stored.
        perf::Profile cur;
        if (m_trialVersion > 0) {
            m_badVersions.insert(m_trialVersion);
        } else if (loadFile(QStringLiteral("active.json"), &cur)) {
            m_badVersions.insert(cur.version);
        }
    }
    perf::Profile prev;
    QByteArray prevRaw;
    const bool havePrev = loadFile(QStringLiteral("previous.json"), &prev, &prevRaw);
    // The known-good values are applied in memory regardless; the disk restore follows the ordering above.
    applyProfile(havePrev ? &prev : nullptr);
    const bool done = restoreKnownGood(havePrev ? &prevRaw : nullptr);
    if (!done) {
        m_rollbackPending = true; // confirmHealthy()/noteCleanExit() must not end this trial: disk is not yet resolved
        if (m_lastError.isEmpty()) {
            m_lastError = QStringLiteral("rollback not durable");
        }
        qWarning() << "[Perf] rollback not durable:" << m_lastError << "- the trial record is kept so the next start retries";
    }
    qInfo() << "[Perf] rolled back to" << (havePrev ? QStringLiteral("version %1").arg(prev.version) : QStringLiteral("built-in defaults"));
    return havePrev && done;
}

bool ProfileStore::endTrial() {
    const int version = m_trialVersion;
    m_trial = false;
    m_cleanExit = false;
    m_trialVersion = 0;
    if (!saveState()) {
        // Not durable: keep the trial pending in memory too, so a later verification retries it.
        m_trial = true;
        m_trialVersion = version;
        return false;
    }
    qInfo() << "[Perf] profile version" << m_activeVersion.load() << "confirmed healthy";
    return true;
}

bool ProfileStore::finishPendingRestore() {
    perf::Profile prev;
    QByteArray raw;
    const bool havePrev = loadFile(QStringLiteral("previous.json"), &prev, &raw);
    if (!restoreKnownGood(havePrev ? &raw : nullptr)) { // same ordering as rollback(): marker, restore, clear, cleanup
        return false;
    }
    m_rollbackPending = false;
    return true;
}

bool ProfileStore::confirmHealthy() {
    if (m_rollbackPending) {
        finishPendingRestore(); // a failed rollback is completed, never turned into a confirmation
        return false;
    }
    return m_trial && endTrial();
}

bool ProfileStore::confirmHealthy(quint64 verificationGeneration) {
    if (!m_trial) {
        return false;
    }
    if (m_rollbackPending) {
        finishPendingRestore();
        return false;
    }
    if (verificationGeneration != m_generation.load()) {
        qInfo() << "[Perf] ignoring a verification that began under profile generation" << verificationGeneration
                << "(current" << m_generation.load() << "): it does not prove the current trial";
        return false;
    }
    return endTrial();
}

bool ProfileStore::noteCleanExit() {
    if (m_rollbackPending && !finishPendingRestore()) {
        return false; // leave cleanExit=false: the next start sees a crash and retries the rollback
    }
    if (!m_trial) {
        return true;
    }
    m_cleanExit = true;
    if (!saveState()) {
        m_cleanExit = false; // next start sees a crash and rolls back: conservative
        return false;
    }
    return true;
}

void ProfileStore::resetToDefaults() {
    applyProfile(nullptr);
    QFile::remove(m_dir + QStringLiteral("/active.json"));
    QFile::remove(m_dir + QStringLiteral("/previous.json"));
    m_trial = false;
    m_cleanExit = false;
    m_trialVersion = 0;
    saveState();
}

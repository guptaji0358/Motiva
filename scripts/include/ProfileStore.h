#pragma once
// Holds the active, validated performance profile. Reads are lock-free atomics,
// so wallpaper code can query a value at any time without blocking. Install /
// activation / rollback only swap those atomics after full validation, so they
// can never interrupt playback. With no profile everything is the built-in default.

#include "PerfProfile.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <atomic>
#include <functional>

// Trial recovery policy (all of it is enforced in ProfileStore.cpp and covered by PerfIntelligenceTests):
//  - Activating a profile starts a TRIAL. state.json records trial=true and trialVersion BEFORE active.json
//    is replaced, so a crash inside install() can never leave an unproven profile marked healthy.
//  - ONLY confirmHealthy() ends a trial as healthy, and the app calls it only on wallpaperVerified (frames
//    actually advancing after an attach). Exiting never confirms.
//  - Clean exit (aboutToQuit -> noteCleanExit) while the trial is unproven: next start CONTINUES the trial
//    (not bad, not healthy). Repeated clean exits keep it a trial; newer profiles stay deferred (TrialActive).
//  - Crash, hang, forced termination (taskkill /F, power loss, logoff kill) or any exit that never reached
//    noteCleanExit while the trial is unproven: indistinguishable from a bad profile, so the next start ROLLS
//    BACK to the last known-good previous.json (or built-in defaults) and marks that version bad forever.
//  - A crash after confirmHealthy() is not a trial failure (trial=false is already persisted).
//  - A crash before the new profile replaced active.json (install interrupted) is not a trial at all: the
//    old active profile stays, nothing is marked bad.
//  - Validation is never relaxed: every load re-verifies signature, schema, ranges and expiry.
class ProfileStore : public QObject {
    Q_OBJECT
public:
    enum class InstallResult { Activated, Rejected, NotNewer, KnownBad, TrialActive, StoreFailed };
    // TrialActive: an earlier profile is still unproven; the new one is deferred (not stored) so the
    // last known-good previous.json can never be replaced by an unproven profile.
    // StoreFailed: the rollback copy, the trial record or active.json could not be written. The profile is
    // NOT activated, the previously active (known-good) profile stays in force and lastError() says why.
    // Persistence failures are never ignored: every write is checked, and a trial is only started once its
    // trial record is durably on disk.

    // Writes a file atomically; false on failure. Replaceable so tests can inject deterministic write failures.
    using Writer = std::function<bool(const QString& path, const QByteArray& data)>;
    void setWriterForTest(Writer w) { m_writer = std::move(w); }

    explicit ProfileStore(QObject* parent = nullptr);
    static ProfileStore& instance();

    // Value for a tunable parameter: the active profile's, else the built-in default.
    static int value(perf::Param p);

    // dir: where active/previous/state files live. Loads the active profile
    // (re-verified), first rolling back a profile whose trial run never
    // completed (crash or hang before the first healthy wallpaper).
    void init(const QString& dir, const QByteArray& publicKey, const perf::HardwareClass& hw);
    void setClock(std::function<qint64()> nowSecs) { m_now = std::move(nowSecs); }
    void setHardware(const perf::HardwareClass& hw);

    InstallResult install(const QByteArray& envelope, QString* error = nullptr);
    bool rollback(); // previous profile, or built-in defaults when none; true only if a previous profile was restored durably
    // Ends the trial; call ONLY on verified wallpaper application evidence. Returns true only when the trial
    // end is durably recorded; on a persistence failure the trial stays pending (retried on the next verification).
    bool confirmHealthy();
    // Same, but only if `verificationGeneration` (generation() captured when that verification BEGAN) is the
    // currently active profile generation. A verification that started before the trial profile was installed
    // (or before a newer profile / rollback replaced it) proves nothing about the current profile and is rejected.
    bool confirmHealthy(quint64 verificationGeneration);
    // Identity of the profile currently in force; changes on init, install, rollback and reset (not on setHardware).
    quint64 generation() const { return m_generation.load(); }
    // Clean application exit while a trial is unconfirmed: the trial stays pending (never confirmed by
    // exiting) but the next start continues it instead of treating it as a crash. False if it could not be recorded
    // (the next start then treats the exit as a crash and rolls back: the conservative outcome).
    bool noteCleanExit();
    void resetToDefaults();
    QString lastError() const { return m_lastError; }

    int activeVersion() const { return m_activeVersion.load(); } // also read by the PerfProbe worker thread
    bool trialPending() const { return m_trial; }

private:
    void applyProfile(const perf::Profile* p, bool newIdentity = true);
    bool loadFile(const QString& name, perf::Profile* out, QByteArray* raw = nullptr) const;
    bool write(const QString& path, const QByteArray& data);
    bool endTrial();
    bool restoreKnownGood(const QByteArray* prevRaw); // ordered, durable disk part of a rollback (see ProfileStore.cpp)
    bool finishPendingRestore(); // retry restoring active.json after a failed rollback; true once it is durable
    InstallResult fail(const QString& why, QString* error);
    bool saveState(); // false when state.json could not be written (lastError() set)
    void loadState();
    qint64 now() const;

    QString m_dir;
    QByteArray m_key;
    perf::HardwareClass m_hw;
    perf::Profile m_active;
    std::atomic<int> m_activeVersion{0};
    std::atomic<quint64> m_generation{0};
    Writer m_writer;
    QString m_lastError;
    bool m_rollbackPending = false; // rollback applied known-good values in memory but active.json still holds the failed profile
    bool m_trial = false;
    int m_trialVersion = 0; // version being trialled; 0 only for state files written before this field existed
    bool m_cleanExit = false;
    QSet<int> m_badVersions;
    std::function<qint64()> m_now;
};

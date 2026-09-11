#pragma once

#include <QString>
#include <QJsonObject>

// Small, structured, persistent recovery/diagnostic state - survives process
// crashes and Windows restarts, unlike the verbose text log
// (%TEMP%\Motiva.log) or SettingsManager's user-preference registry
// keys. Deliberately NOT a growing log: each mutator does a full
// read-modify-atomic-replace of one small JSON document.
//
// This file must never be treated as authoritative for "is the wallpaper
// currently attached" - only WindowsDesktopWallpaper's live Win32 queries
// and D3DWallpaperRenderer::presentedFrameCount are. It exists purely to
// carry recovery INTENT and diagnostics across a restart the user cannot
// repeat on demand (see VideoWallpaper/CLAUDE.md).
class RecoveryState {
public:
    RecoveryState();

    // Snapshot of what was on disk when this object was constructed (i.e.
    // what the previous session left behind), for logging a one-line
    // "previous session" summary at startup.
    struct PreviousSession {
        bool valid = false; // false if no prior state file existed
        bool cleanExit = false;
        bool wallpaperWasAttached = false;
        qint64 lastVideoReadyMs = -1;
        qint64 lastVideoPresentedMs = -1;
        quint64 lastAttachGeneration = 0;
        qint64 lastExplorerPid = 0;
        int explorerRecoveryCount = 0;
        QString videoPath;
        QString lastFailure;
        qint64 lastHResult = 0;
        // Previous session's own startup{} block, so a single reboot is
        // enough evidence without needing to reproduce it again.
        QJsonObject previousStartupDiagnostics;
    };
    const PreviousSession& previousSession() const { return m_previous; }

    // --- lastSession fields ---
    void setCleanExit(bool clean);
    void setWallpaperAttached(bool attached);
    void markVideoReady(qint64 elapsedMs);
    void markVideoPresenting(qint64 elapsedMs);
    void setAttachGeneration(quint64 generation);
    void setExplorerPid(qint64 pid);
    void setVideoPath(const QString& path);

    // --- recovery fields ---
    void incrementExplorerRecoveryCount();
    void recordFailure(const QString& reason, qint64 hresult = 0);

    // --- startup{} diagnostics block (see StartupDiagnostics) ---
    void setStartupDiagnostics(const QJsonObject& diagnostics);

private:
    QString statePath() const;
    QString tempPath() const;
    void load();
    void save();

    QJsonObject m_root;
    PreviousSession m_previous;
};

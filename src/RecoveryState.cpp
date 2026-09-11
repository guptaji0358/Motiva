#include "RecoveryState.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDebug>
#include <windows.h>

namespace {
constexpr int kSchemaVersion = 1;
}

RecoveryState::RecoveryState() {
    load();
}

QString RecoveryState::statePath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/recovery-state.json";
}

QString RecoveryState::tempPath() const {
    return statePath() + ".tmp";
}

void RecoveryState::load() {
    QFile f(statePath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            m_root = doc.object();
        } else {
            qWarning() << "[RecoveryState] Existing state file failed to parse (" << err.errorString()
                       << ") - starting fresh; previous session details are unavailable.";
        }
    }

    QJsonObject lastSession = m_root.value("lastSession").toObject();
    if (!lastSession.isEmpty()) {
        m_previous.valid = true;
        m_previous.cleanExit = lastSession.value("cleanExit").toBool(false);
        m_previous.wallpaperWasAttached = lastSession.value("wallpaperWasAttached").toBool(false);
        m_previous.lastVideoReadyMs = static_cast<qint64>(lastSession.value("lastVideoReadyMs").toDouble(-1));
        m_previous.lastVideoPresentedMs = static_cast<qint64>(lastSession.value("lastVideoPresentedMs").toDouble(-1));
        m_previous.lastAttachGeneration = static_cast<quint64>(lastSession.value("lastAttachGeneration").toDouble(0));
        m_previous.lastExplorerPid = static_cast<qint64>(lastSession.value("lastExplorerPid").toDouble(0));
        m_previous.videoPath = lastSession.value("videoPath").toString();
    }
    QJsonObject recovery = m_root.value("recovery").toObject();
    m_previous.explorerRecoveryCount = recovery.value("explorerRecoveryCount").toInt(0);
    m_previous.lastFailure = recovery.value("lastFailure").toString();
    m_previous.lastHResult = static_cast<qint64>(recovery.value("lastHRESULT").toDouble(0));
    m_previous.previousStartupDiagnostics = m_root.value("startup").toObject();

    // Fresh document going forward, seeded with the recovery counters we
    // want to keep accumulating (explorerRecoveryCount) but with
    // lastSession reset to "this session hasn't proven anything yet" -
    // cleanExit defaults to false until onExitRequested() runs, so a crash
    // or kill correctly leaves cleanExit=false on disk for the next launch.
    m_root["schemaVersion"] = kSchemaVersion;
    QJsonObject session;
    session["cleanExit"] = false;
    session["wallpaperWasAttached"] = m_previous.wallpaperWasAttached;
    session["lastVideoReadyMs"] = -1;
    session["lastVideoPresentedMs"] = -1;
    session["lastAttachGeneration"] = static_cast<double>(m_previous.lastAttachGeneration);
    session["lastExplorerPid"] = static_cast<double>(m_previous.lastExplorerPid);
    session["videoPath"] = m_previous.videoPath;
    m_root["lastSession"] = session;

    if (!m_root.contains("recovery")) {
        QJsonObject rec;
        rec["explorerRecoveryCount"] = m_previous.explorerRecoveryCount;
        rec["lastFailure"] = m_previous.lastFailure;
        rec["lastHRESULT"] = static_cast<double>(m_previous.lastHResult);
        m_root["recovery"] = rec;
    }

    save();

    if (m_previous.valid) {
        qInfo() << "[RecoveryState] Previous session: cleanExit=" << m_previous.cleanExit
                << "wallpaperWasAttached=" << m_previous.wallpaperWasAttached
                << "videoReadyMs=" << m_previous.lastVideoReadyMs
                << "videoPresentedMs=" << m_previous.lastVideoPresentedMs
                << "explorerRecoveryCount=" << m_previous.explorerRecoveryCount
                << "lastFailure=" << m_previous.lastFailure;
        if (!m_previous.previousStartupDiagnostics.isEmpty()) {
            qInfo() << "[RecoveryState] Previous session startup diagnostics:"
                    << QJsonDocument(m_previous.previousStartupDiagnostics).toJson(QJsonDocument::Compact);
        }
    } else {
        qInfo() << "[RecoveryState] No previous session state found (first run, or state file was missing).";
    }
}

void RecoveryState::save() {
    QFile tmp(tempPath());
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "[RecoveryState] Failed to open temp state file for writing:" << tempPath();
        return;
    }
    tmp.write(QJsonDocument(m_root).toJson(QJsonDocument::Indented));
    tmp.flush();
    tmp.close();

    const std::wstring wTmp = QDir::toNativeSeparators(tempPath()).toStdWString();
    const std::wstring wFinal = QDir::toNativeSeparators(statePath()).toStdWString();
    // Observed in testing: an occasional transient ERROR_ACCESS_DENIED on
    // the very first save of a fresh process (plausibly antivirus/indexing
    // briefly holding the destination open right after the previous
    // process wrote it) - a few short retries clears it without risking
    // corruption either way, since a failed MoveFileExW here leaves the
    // previous state file completely untouched (the design this atomic
    // replace exists for in the first place).
    bool replaced = false;
    for (int attempt = 0; attempt < 5 && !replaced; ++attempt) {
        replaced = MoveFileExW(wTmp.c_str(), wFinal.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!replaced) {
            Sleep(20);
        }
    }
    if (!replaced) {
        qWarning() << "[RecoveryState] Atomic replace of state file failed, GetLastError=" << GetLastError()
                   << "- previous state file (if any) left untouched.";
    }
}

void RecoveryState::setCleanExit(bool clean) {
    QJsonObject session = m_root.value("lastSession").toObject();
    session["cleanExit"] = clean;
    m_root["lastSession"] = session;
    save();
}

void RecoveryState::setWallpaperAttached(bool attached) {
    QJsonObject session = m_root.value("lastSession").toObject();
    session["wallpaperWasAttached"] = attached;
    m_root["lastSession"] = session;
    save();
}

void RecoveryState::markVideoReady(qint64 elapsedMs) {
    QJsonObject session = m_root.value("lastSession").toObject();
    session["lastVideoReadyMs"] = static_cast<double>(elapsedMs);
    m_root["lastSession"] = session;
    save();
}

void RecoveryState::markVideoPresenting(qint64 elapsedMs) {
    QJsonObject session = m_root.value("lastSession").toObject();
    session["lastVideoPresentedMs"] = static_cast<double>(elapsedMs);
    m_root["lastSession"] = session;
    save();
}

void RecoveryState::setAttachGeneration(quint64 generation) {
    QJsonObject session = m_root.value("lastSession").toObject();
    session["lastAttachGeneration"] = static_cast<double>(generation);
    m_root["lastSession"] = session;
    save();
}

void RecoveryState::setExplorerPid(qint64 pid) {
    QJsonObject session = m_root.value("lastSession").toObject();
    session["lastExplorerPid"] = static_cast<double>(pid);
    m_root["lastSession"] = session;
    save();
}

void RecoveryState::setVideoPath(const QString& path) {
    QJsonObject session = m_root.value("lastSession").toObject();
    session["videoPath"] = path;
    m_root["lastSession"] = session;
    save();
}

void RecoveryState::incrementExplorerRecoveryCount() {
    QJsonObject recovery = m_root.value("recovery").toObject();
    recovery["explorerRecoveryCount"] = recovery.value("explorerRecoveryCount").toInt(0) + 1;
    m_root["recovery"] = recovery;
    save();
}

void RecoveryState::recordFailure(const QString& reason, qint64 hresult) {
    QJsonObject recovery = m_root.value("recovery").toObject();
    recovery["lastFailure"] = reason;
    recovery["lastHRESULT"] = static_cast<double>(hresult);
    m_root["recovery"] = recovery;
    save();
}

void RecoveryState::setStartupDiagnostics(const QJsonObject& diagnostics) {
    m_root["startup"] = diagnostics;
    save();
}

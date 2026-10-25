#include "BrowserIntegration.h"

#include "SettingsManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <windows.h>

namespace BrowserIntegration {

namespace {

// Per-browser registry roots that Chromium browsers read host manifests from.
const wchar_t* const kRegistryRoots[] = {
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\",
    L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\",
    L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\",
};

QString manifestPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
           QStringLiteral("/browser/com.motiva.browser.json");
}

} // namespace

QString hostExecutablePath() {
    // Beside the running exe (installed layout), else - for a dev build in
    // <repo>/build - beside the deployed copy that shortcuts/verbs point at.
    const QString own = QCoreApplication::applicationDirPath() + QStringLiteral("/MotivaBrowserHost.exe");
    if (QFileInfo::exists(own) && QFileInfo(QCoreApplication::applicationDirPath()).fileName().compare("build", Qt::CaseInsensitive) != 0) {
        return QDir::toNativeSeparators(own);
    }
    const QString deployedDir = QFileInfo(SettingsManager::motivaExecutablePath()).absolutePath();
    for (const QString& candidate : {deployedDir + QStringLiteral("/resources/bin"), deployedDir}) {
        if (QFileInfo::exists(candidate + QStringLiteral("/MotivaBrowserHost.exe"))) {
            return QDir::toNativeSeparators(candidate + QStringLiteral("/MotivaBrowserHost.exe"));
        }
    }
    return QDir::toNativeSeparators(own);
}

QString extensionFolder() {
    // Installed layout and the build tree both keep it beside Motiva.exe;
    // a development checkout has it at the project root.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString deployedDir = QFileInfo(SettingsManager::motivaExecutablePath()).absolutePath();
    for (const QString& candidate :
         {appDir + QStringLiteral("/extension"), deployedDir + QStringLiteral("/resources/bin/extension"),
          appDir + QStringLiteral("/../extension"), appDir + QStringLiteral("/../../extension")}) {
        if (QFileInfo::exists(candidate + QStringLiteral("/manifest.json"))) {
            return QDir::toNativeSeparators(QDir::cleanPath(candidate));
        }
    }
    return QString();
}

bool registerHost(QString* error) {
    const QString host = hostExecutablePath();
    if (!QFileInfo::exists(host)) {
        if (error) {
            *error = QCoreApplication::translate("BrowserIntegration", "MotivaBrowserHost.exe was not found next to Motiva.exe.");
        }
        return false;
    }
    QJsonObject manifest;
    manifest[QStringLiteral("name")] = QString::fromLatin1(kHostName);
    manifest[QStringLiteral("description")] = QStringLiteral("Motiva browser extension bridge");
    manifest[QStringLiteral("path")] = host;
    manifest[QStringLiteral("type")] = QStringLiteral("stdio");
    manifest[QStringLiteral("allowed_origins")] =
        QJsonArray{QStringLiteral("chrome-extension://%1/").arg(QLatin1String(kExtensionId))};

    const QString path = manifestPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        f.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0) {
        if (error) {
            *error = f.errorString();
        }
        return false;
    }
    f.close();

    const std::wstring manifestW = QDir::toNativeSeparators(path).toStdWString();
    bool any = false;
    for (const wchar_t* root : kRegistryRoots) {
        const std::wstring key = std::wstring(root) + QString::fromLatin1(kHostName).toStdWString();
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) ==
            ERROR_SUCCESS) {
            RegSetValueExW(h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(manifestW.c_str()),
                           static_cast<DWORD>((manifestW.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(h);
            any = true;
        }
    }
    if (!any && error) {
        *error = QCoreApplication::translate("BrowserIntegration", "The registry entries could not be written.");
    }
    return any;
}

void unregisterHost() {
    for (const wchar_t* root : kRegistryRoots) {
        const std::wstring key = std::wstring(root) + QString::fromLatin1(kHostName).toStdWString();
        RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
    }
    QFile::remove(manifestPath());
}

} // namespace BrowserIntegration

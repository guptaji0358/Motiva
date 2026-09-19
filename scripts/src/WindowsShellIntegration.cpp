#include "WindowsShellIntegration.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <windows.h>
#include <shlobj.h>
#include <shlguid.h>
#include <objbase.h>

namespace WindowsShellIntegration {

namespace {

QString startMenuShortcutPath() {
    const QString programsDir =
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    return QDir::toNativeSeparators(programsDir + QStringLiteral("/Motiva.lnk"));
}

// COM is apartment-threaded and CoInitialize is not reentrant-safe to call
// unconditionally - RAII guard that tolerates "already initialized on this
// thread" (RPC_E_CHANGED_MODE / S_FALSE) rather than treating either as a
// hard failure, since callers here always run on the GUI thread where Qt
// itself may have already initialized COM for other purposes.
class ComGuard {
public:
    ComGuard() : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComGuard() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    bool ok() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT m_hr;
};

constexpr const wchar_t* kShellClassesRoot = L"Software\\Classes";
constexpr const wchar_t* kVerbName = L"MotivaSetBackground";
constexpr const wchar_t* kVerbDisplayName = L"Set as background";

QString verbKeyPath(const QString& extension) {
    return QStringLiteral("Software\\Classes\\SystemFileAssociations\\.%1\\shell\\MotivaSetBackground")
        .arg(extension);
}

bool setStringValue(HKEY key, const wchar_t* valueName, const QString& value) {
    const std::wstring w = value.toStdWString();
    return RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()),
                           static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

} // namespace

// A target is runnable only if Qt's DLLs can be found for it: either the
// deployment-root launcher (real binary at resources/bin beside resources/Qt)
// or an exe with Qt6Core.dll right next to it. Anything else (e.g. a bare
// build-tree exe) would produce DLL-not-found errors, so never register it.
bool isRunnableTarget(const QString& exePath) {
    const QFileInfo fi(exePath);
    if (!fi.exists()) {
        return false;
    }
    const QDir dir = fi.absoluteDir();
    return QFileInfo::exists(dir.filePath(QStringLiteral("resources/bin/") + fi.fileName())) &&
               QFileInfo::exists(dir.filePath(QStringLiteral("resources/Qt/Qt6Core.dll"))) ||
           QFileInfo::exists(dir.filePath(QStringLiteral("Qt6Core.dll")));
}

// Icon comes from the real binary (embedded app.rc icon), not the icon-less launcher.
QString iconSourceFor(const QString& exePath) {
    const QFileInfo fi(exePath);
    const QString real = fi.absoluteDir().filePath(QStringLiteral("resources/bin/") + fi.fileName());
    return QFileInfo::exists(real) ? real : exePath;
}

bool CreateStartMenuShortcut(const QString& exePath) {
    if (!isRunnableTarget(exePath)) {
        qWarning() << "[ShellIntegration] Refusing to create shortcut: not a runnable deployment:" << exePath;
        RemoveStartMenuShortcut(); // never leave a stale/broken one behind
        return false;
    }
    ComGuard com;
    if (!com.ok()) {
        qWarning() << "[ShellIntegration] CoInitializeEx failed - cannot create Start Menu shortcut.";
        return false;
    }

    IShellLinkW* shellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**>(&shellLink));
    if (FAILED(hr) || !shellLink) {
        qWarning() << "[ShellIntegration] CoCreateInstance(CLSID_ShellLink) failed, hr=" << hr;
        return false;
    }

    const std::wstring targetW = QDir::toNativeSeparators(exePath).toStdWString();
    const std::wstring workingDirW =
        QDir::toNativeSeparators(QFileInfo(exePath).absolutePath()).toStdWString();

    shellLink->SetPath(targetW.c_str());
    shellLink->SetWorkingDirectory(workingDirW.c_str());
    const std::wstring iconW = QDir::toNativeSeparators(iconSourceFor(exePath)).toStdWString();
    shellLink->SetIconLocation(iconW.c_str(), 0);
    shellLink->SetDescription(L"Motiva - live video wallpaper");

    IPersistFile* persistFile = nullptr;
    hr = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
    bool ok = false;
    if (SUCCEEDED(hr) && persistFile) {
        const std::wstring lnkPathW = startMenuShortcutPath().toStdWString();
        // Overwrites the existing file if one is already there - the fixed
        // target path is what makes repeated "enable" calls idempotent
        // rather than creating duplicates.
        hr = persistFile->Save(lnkPathW.c_str(), TRUE);
        ok = SUCCEEDED(hr);
        if (!ok) {
            qWarning() << "[ShellIntegration] IPersistFile::Save failed, hr=" << hr;
        }
        persistFile->Release();
    } else {
        qWarning() << "[ShellIntegration] QueryInterface(IID_IPersistFile) failed, hr=" << hr;
    }
    shellLink->Release();
    return ok;
}

bool RemoveStartMenuShortcut() {
    const QString path = startMenuShortcutPath();
    if (!QFile::exists(path)) {
        return true;
    }
    if (!QFile::remove(path)) {
        qWarning() << "[ShellIntegration] Failed to remove Start Menu shortcut at" << path;
        return false;
    }
    return true;
}

bool RegisterSetBackgroundVerb(const QString& exePath, const QStringList& extensions) {
    if (!isRunnableTarget(exePath)) {
        qWarning() << "[ShellIntegration] Refusing to register verb: not a runnable deployment:" << exePath;
        UnregisterSetBackgroundVerb(extensions);
        return false;
    }
    bool allOk = true;
    const QString command =
        QStringLiteral("\"%1\" --set-background \"%2\"").arg(QDir::toNativeSeparators(exePath), "%1");
    for (const QString& ext : extensions) {
        HKEY verbKey = nullptr;
        const std::wstring verbPathW = verbKeyPath(ext).toStdWString();
        if (RegCreateKeyExW(HKEY_CURRENT_USER, verbPathW.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                             &verbKey, nullptr) != ERROR_SUCCESS) {
            qWarning() << "[ShellIntegration] Failed to create verb key for ." << ext;
            allOk = false;
            continue;
        }
        setStringValue(verbKey, nullptr, QString::fromWCharArray(kVerbDisplayName));
        setStringValue(verbKey, L"Icon", QDir::toNativeSeparators(iconSourceFor(exePath)));
        RegCloseKey(verbKey);

        HKEY commandKey = nullptr;
        const std::wstring commandPathW = (verbKeyPath(ext) + QStringLiteral("\\command")).toStdWString();
        if (RegCreateKeyExW(HKEY_CURRENT_USER, commandPathW.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                             &commandKey, nullptr) != ERROR_SUCCESS) {
            qWarning() << "[ShellIntegration] Failed to create command key for ." << ext;
            allOk = false;
            continue;
        }
        if (!setStringValue(commandKey, nullptr, command)) {
            allOk = false;
        }
        RegCloseKey(commandKey);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return allOk;
}

bool UnregisterSetBackgroundVerb(const QStringList& extensions) {
    bool allOk = true;
    for (const QString& ext : extensions) {
        const std::wstring verbPathW = verbKeyPath(ext).toStdWString();
        // RegDeleteTreeW is a no-op success when the key is already absent
        // (ERROR_FILE_NOT_FOUND) - treated as success, not a failure, since
        // "not registered" and "just unregistered" are the same end state.
        LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, verbPathW.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            qWarning() << "[ShellIntegration] Failed to remove verb key for ." << ext << "status=" << status;
            allOk = false;
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return allOk;
}

} // namespace WindowsShellIntegration

#include "WindowsShellIntegration.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
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

constexpr const wchar_t* kFileAssociationsRoot = L"Software\\Classes\\SystemFileAssociations";
constexpr const char* kClassesRoot = "Software\\Classes\\";
constexpr const char* kMenuVerb = "Motiva";
// The flat per-extension verbs earlier builds registered (two root entries
// per image extension) - removed whenever the menu is (re)registered.
constexpr const char* kLegacyVerbs[] = {"MotivaSetBackground", "MotivaAddToPlaylist"};
// ECF_SEPARATORBEFORE: a separator above "Create New Playlist...".
constexpr DWORD kSeparatorBefore = 0x20;

enum class MenuKind { Video, Image, Background };

QString kindName(MenuKind kind) {
    switch (kind) {
    case MenuKind::Video:
        return QStringLiteral("Video");
    case MenuKind::Image:
        return QStringLiteral("Image");
    default:
        return QStringLiteral("Background");
    }
}

// Relative to HKCR (what ExtendedSubCommandsKey expects).
QString menuKeyName(MenuKind kind) {
    return QStringLiteral("Motiva.ExplorerMenu.") + kindName(kind);
}
QString playlistsKeyName(MenuKind kind) {
    return menuKeyName(kind) + QStringLiteral("Playlists");
}
QString hkcu(const QString& classesRelative) {
    return QLatin1String(kClassesRoot) + classesRelative;
}

bool setStringValue(HKEY key, const wchar_t* valueName, const QString& value) {
    const std::wstring w = value.toStdWString();
    return RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()),
                           static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool deleteTree(const QString& path) {
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, path.toStdWString().c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

// Creates (or opens) `path` and writes the given string values.
bool writeKey(const QString& path, const QList<QPair<const wchar_t*, QString>>& values, DWORD commandFlags = 0) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.toStdWString().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        qWarning() << "[ShellIntegration] Failed to create registry key" << path;
        return false;
    }
    bool ok = true;
    for (const auto& value : values) {
        ok = setStringValue(key, value.first, value.second) && ok;
    }
    if (commandFlags) {
        ok = RegSetValueExW(key, L"CommandFlags", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&commandFlags),
                            sizeof(commandFlags)) == ERROR_SUCCESS && ok;
    }
    RegCloseKey(key);
    return ok;
}

// A menu command: <path>\command's default value is the command line.
bool writeCommand(const QString& path, const QString& label, const QString& command, DWORD commandFlags = 0,
                  bool singleSelection = false) {
    QList<QPair<const wchar_t*, QString>> values{{L"MUIVerb", label}};
    if (singleSelection) {
        // Offered only when exactly one file is selected (one dialog, one file).
        values.append({L"MultiSelectModel", QStringLiteral("Single")});
    }
    return writeKey(path, values, commandFlags) && writeKey(path + QStringLiteral("\\command"), {{nullptr, command}});
}

// MUIVerb treats '&' as an accelerator marker and a leading '@' as an
// indirect resource string - neither may come from a playlist name as-is.
QString menuText(const QString& name) {
    QString text = name;
    text.replace(QLatin1Char('&'), QStringLiteral("&&"));
    if (text.startsWith(QLatin1Char('@'))) {
        text.prepend(QChar(0x200B)); // zero-width space
    }
    return text;
}

QString quotedExe(const QString& exePath) {
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(exePath));
}

QStringList fileAssociationExtensions() {
    QStringList names;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kFileAssociationsRoot, 0, KEY_READ, &root) != ERROR_SUCCESS) {
        return names;
    }
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        names << QString::fromWCharArray(name, static_cast<int>(len));
    }
    RegCloseKey(root);
    return names;
}

QString associationVerbPath(const QString& dotExtension, const QString& verb) {
    return QStringLiteral("%1\\%2\\shell\\%3").arg(QString::fromWCharArray(kFileAssociationsRoot), dotExtension, verb);
}

// Removes Motiva's verbs (the cascade root and the legacy flat verbs) from
// every file association except `keep` (lowercase ".ext" whose cascade is
// being (re)written). Only Motiva's own verb names are ever touched.
bool removeMotivaVerbs(const QSet<QString>& keep = {}) {
    bool ok = true;
    for (const QString& assoc : fileAssociationExtensions()) {
        for (const char* legacy : kLegacyVerbs) {
            ok = deleteTree(associationVerbPath(assoc, QLatin1String(legacy))) && ok;
        }
        if (!keep.contains(assoc.toLower())) {
            ok = deleteTree(associationVerbPath(assoc, QLatin1String(kMenuVerb))) && ok;
        }
    }
    return ok;
}

bool writeCreateNewEntry(MenuKind kind, const QString& exePath, bool separator) {
    return writeCommand(hkcu(playlistsKeyName(kind)) + QStringLiteral("\\shell\\zzCreateNew"),
                        QStringLiteral("Create New Playlist") + QChar(0x2026),
                        QStringLiteral("%1 --add-to-new-playlist \"%2\"").arg(quotedExe(exePath), QStringLiteral("%1")),
                        separator ? kSeparatorBefore : 0, true);
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

bool RegisterExplorerMenu(const QString& exePath, const QStringList& videoExtensions,
                          const QStringList& imageExtensions, const QStringList& backgroundOnlyExtensions) {
    if (!isRunnableTarget(exePath)) {
        qWarning() << "[ShellIntegration] Refusing to register Explorer menu: not a runnable deployment:" << exePath;
        UnregisterExplorerMenu();
        return false;
    }
    const QString setBackground =
        QStringLiteral("%1 --set-background \"%2\"").arg(quotedExe(exePath), QStringLiteral("%1"));
    const QString icon = QDir::toNativeSeparators(iconSourceFor(exePath));

    bool ok = true;
    QSet<QString> keep;
    const QList<QPair<MenuKind, QStringList>> groups = {
        {MenuKind::Video, videoExtensions},
        {MenuKind::Image, imageExtensions},
        {MenuKind::Background, backgroundOnlyExtensions},
    };
    for (const auto& group : groups) {
        const MenuKind kind = group.first;
        const QString menuShell = hkcu(menuKeyName(kind)) + QStringLiteral("\\shell\\");
        ok = writeCommand(menuShell + QStringLiteral("01SetBackground"), QStringLiteral("Set as background"),
                          setBackground) && ok;
        if (kind != MenuKind::Background) {
            ok = writeKey(menuShell + QStringLiteral("02AddToPlaylist"),
                          {{L"MUIVerb", QStringLiteral("Add to playlist")},
                           {L"ExtendedSubCommandsKey", playlistsKeyName(kind)}}) && ok;
            // The playlist entries themselves are written by
            // UpdateExplorerPlaylistMenu(); this only guarantees the
            // submenu is never empty (and keeps existing entries).
            ok = writeCreateNewEntry(kind, exePath, false) && ok;
        }
        for (const QString& ext : group.second) {
            const QString dotExt = QLatin1Char('.') + ext.toLower();
            keep.insert(dotExt);
            ok = writeKey(associationVerbPath(dotExt, QLatin1String(kMenuVerb)),
                          {{L"MUIVerb", QStringLiteral("Motiva")},
                           {L"Icon", icon},
                           {L"ExtendedSubCommandsKey", menuKeyName(kind)}}) && ok;
        }
    }
    ok = removeMotivaVerbs(keep) && ok;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool UpdateExplorerPlaylistMenu(const QString& exePath, const QVector<ExplorerPlaylist>& imagePlaylists,
                                const QVector<ExplorerPlaylist>& videoPlaylists) {
    if (!isRunnableTarget(exePath)) {
        return false;
    }
    bool ok = true;
    const QList<QPair<MenuKind, QVector<ExplorerPlaylist>>> groups = {
        {MenuKind::Image, imagePlaylists},
        {MenuKind::Video, videoPlaylists},
    };
    for (const auto& group : groups) {
        const QString shell = hkcu(playlistsKeyName(group.first)) + QStringLiteral("\\shell\\");
        ok = deleteTree(hkcu(playlistsKeyName(group.first))) && ok;
        int index = 0;
        for (const ExplorerPlaylist& playlist : group.second) {
            // Key names only fix the order (library order); the command
            // carries the persistent playlist id, never the name.
            ok = writeCommand(shell + QStringLiteral("p%1").arg(++index, 4, 10, QLatin1Char('0')),
                              menuText(playlist.name),
                              QStringLiteral("%1 --add-to-playlist-id %2 \"%3\"")
                                  .arg(quotedExe(exePath))
                                  .arg(playlist.id)
                                  .arg(QStringLiteral("%1"))) && ok;
        }
        ok = writeCreateNewEntry(group.first, exePath, !group.second.isEmpty()) && ok;
    }
    return ok;
}

bool UnregisterExplorerMenu() {
    bool ok = removeMotivaVerbs();
    for (MenuKind kind : {MenuKind::Video, MenuKind::Image, MenuKind::Background}) {
        ok = deleteTree(hkcu(menuKeyName(kind))) && ok;
        ok = deleteTree(hkcu(playlistsKeyName(kind))) && ok;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

} // namespace WindowsShellIntegration

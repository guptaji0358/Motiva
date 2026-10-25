#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// All raw Win32/COM Windows-shell-integration calls (Start Menu shortcut,
// the Explorer "Motiva" context menu) live only here - see
// WindowsDesktopWallpaper.h for the equivalent rule covering desktop-attach
// Win32 calls. Nothing else in this app should call IShellLinkW or touch
// HKCU\Software\Classes directly.
namespace WindowsShellIntegration {

// Creates/overwrites a Start Menu shortcut to exePath at
// %APPDATA%\Microsoft\Windows\Start Menu\Programs\Motiva.lnk, using
// exePath itself for the icon. Windows Search indexes Start Menu shortcuts
// natively, so this is the only mechanism needed to make Motiva
// discoverable there. Idempotent: the target path is fixed, so a repeat
// call simply overwrites the same file rather than creating a duplicate.
bool CreateStartMenuShortcut(const QString& exePath);

// Removes the shortcut created above, if present. No-op (returns true) if
// it doesn't exist.
bool RemoveStartMenuShortcut();

// --- Explorer context menu ("Motiva >" cascade) ---
//
// Normal case (MotivaShell.dll present): ONE verb per supported extension and
// on folders, with ExplorerCommandHandler -> the COM handler in
// scripts/shell/MotivaShell.cpp, which builds Set as wallpaper / Add to
// "<last playlist>" / Add to playlist > ... from the whole selection
// (multi-select, mixed image+video, folders that contain images). The static
// layout described below is only the fallback when the DLL is missing.
//
// Exactly ONE root entry per supported media extension:
//
//   Motiva  >  Set as background
//              Add to playlist  >  <each playlist of the file's type>
//                                  Create New Playlist...
//
// Layout (all per-user, HKCU\Software\Classes - no admin rights):
//   SystemFileAssociations\.<ext>\shell\Motiva
//       MUIVerb=Motiva, Icon, ExtendedSubCommandsKey=Motiva.ExplorerMenu.<Kind>
//   Motiva.ExplorerMenu.<Kind>\shell\01SetBackground      (--set-background "%1")
//   Motiva.ExplorerMenu.<Kind>\shell\02AddToPlaylist
//       ExtendedSubCommandsKey=Motiva.ExplorerMenu.<Kind>Playlists
//   Motiva.ExplorerMenu.<Kind>Playlists\shell\pNNNN       (--add-to-playlist-id <id> "%1")
//   Motiva.ExplorerMenu.<Kind>Playlists\shell\zzCreateNew (--add-to-new-playlist "%1")
// <Kind> is Video, Image, or Background (formats no playlist type accepts,
// e.g. GIF: only "Set as background"). Static registry verbs - the same
// mechanism Motiva always used; on Windows 11 they appear under "Show more
// options", as before.
//
// Replaces the old flat MotivaSetBackground / MotivaAddToPlaylist verbs
// (two root entries per image extension); registering removes them.

// A playlist as shown in the submenu. The id (playlists.id in the .mtv) is
// what the command carries - the name is display text only.
struct ExplorerPlaylist {
    qint64 id = 0;
    QString name;
};

// Registers/overwrites the cascade for the given extension groups. Leaves
// existing playlist entries in place (see UpdateExplorerPlaylistMenu) and
// makes sure "Create New Playlist..." exists. Removes Motiva entries from
// extensions no longer listed, and the legacy flat verbs. Idempotent.
//
// folderMenu (v2.1): also registers "Motiva > Add to playlist" on FOLDERS
// (Directory\shell\Motiva). It is shown only for a folder that directly
// contains at least one image: MotivaShell.dll (IExplorerCommandState,
// registered per-user as a CLSID, CommandStateHandler on the verb) hides it
// otherwise. Skipped if MotivaShell.dll is missing. The command carries the
// folder path ("%1"); the app adds every image directly inside it.
bool RegisterExplorerMenu(const QString& exePath, const QStringList& videoExtensions,
                          const QStringList& imageExtensions, const QStringList& backgroundOnlyExtensions,
                          bool folderMenu = true);

// Rewrites the "Add to playlist" submenus from the current library: image
// files list image playlists, video files list video playlists (in library
// order), each followed by "Create New Playlist...". Only meaningful while
// the menu is registered.
//
// With MotivaShell.dll present (normal case) the playlists are stored as
// values on the COM handler's CLSID key, and lastPlaylistId drives its
// "Add to <last playlist>" shortcut; without the DLL the static submenu keys
// are rewritten as before.
bool UpdateExplorerPlaylistMenu(const QString& exePath, const QVector<ExplorerPlaylist>& imagePlaylists,
                                const QVector<ExplorerPlaylist>& videoPlaylists, qint64 lastPlaylistId = 0);

// Removes everything above - every shell\Motiva verb, the shared menu keys,
// and any legacy flat verb - whatever extension set registered them.
bool UnregisterExplorerMenu();

} // namespace WindowsShellIntegration

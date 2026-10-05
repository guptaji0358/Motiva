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
bool RegisterExplorerMenu(const QString& exePath, const QStringList& videoExtensions,
                          const QStringList& imageExtensions, const QStringList& backgroundOnlyExtensions);

// Rewrites the "Add to playlist" submenus from the current library: image
// files list image playlists, video files list video playlists (in library
// order), each followed by "Create New Playlist...". Only meaningful while
// the menu is registered.
bool UpdateExplorerPlaylistMenu(const QString& exePath, const QVector<ExplorerPlaylist>& imagePlaylists,
                                const QVector<ExplorerPlaylist>& videoPlaylists);

// Removes everything above - every shell\Motiva verb, the shared menu keys,
// and any legacy flat verb - whatever extension set registered them.
bool UnregisterExplorerMenu();

} // namespace WindowsShellIntegration

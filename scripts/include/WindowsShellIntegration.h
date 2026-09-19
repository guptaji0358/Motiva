#pragma once

#include <QString>
#include <QStringList>

// All raw Win32/COM Windows-shell-integration calls (Start Menu shortcut,
// Explorer "Set as background" context-menu verb) live only here - see
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

// Registers a per-user Explorer context-menu verb ("Set as background")
// for each extension in extensions, under
// HKCU\Software\Classes\SystemFileAssociations\.<ext>\shell\MotivaSetBackground.
// The verb's command launches exePath with "--set-background %1". Per-user,
// no admin rights required, fully reversible. Idempotent: each call
// overwrites the same fixed set of registry values.
bool RegisterSetBackgroundVerb(const QString& exePath, const QStringList& extensions);

// Removes the verb registered above for each extension, if present. No-op
// (returns true) for any extension that was never registered.
bool UnregisterSetBackgroundVerb(const QStringList& extensions);

} // namespace WindowsShellIntegration

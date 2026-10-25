#pragma once

#include <QString>

// Registers (or removes) Motiva's Native Messaging host for Chrome, Edge and
// Brave so the Motiva browser extension (extension/) can talk to the app.
// Per-user only (HKCU + a manifest under AppLocalData), no admin rights.
namespace BrowserIntegration {

constexpr const char* kHostName = "com.motiva.browser";
constexpr const char* kExtensionId = "decemojbcdaodchnbeopkdofadijfkgi";

// Writes the host manifest and the three browser registry entries.
// Returns false (with a reason) when the helper exe is missing.
bool registerHost(QString* error = nullptr);
void unregisterHost();

// <Motiva folder>/MotivaBrowserHost.exe and the shipped extension folder.
QString hostExecutablePath();
// The unpacked extension to load in the browser (empty if not shipped).
QString extensionFolder();

} // namespace BrowserIntegration

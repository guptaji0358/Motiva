#pragma once

// Motiva's own app registrations for cloud sign-in. Fill these in ONCE (as the
// developer) and every user just clicks "Connect" - no per-user setup.
// Client IDs are public identifiers, not secrets (OAuth PKCE desktop apps);
// Google's "client secret" for desktop apps is likewise not confidential.
//
// Register each app with the redirect URI  http://localhost:53682/
//  - OneDrive : https://entra.microsoft.com -> App registrations -> New.
//               Supported accounts: personal + work. Platform: "Mobile and
//               desktop" with the URI above. API permissions: Files.Read.
//  - Google   : https://console.cloud.google.com/apis/credentials -> OAuth
//               client ID, type "Desktop app". Enable the Google Drive API.
//               drive.readonly is a restricted scope: publishing to all users
//               needs Google's verification (test users work without it).
//  - Dropbox  : https://www.dropbox.com/developers/apps -> Scoped access,
//               "Full Dropbox" (or App folder), permissions files.metadata.read
//               and files.content.read, redirect URI above.
// A user can still override these with their own registration (Set Up...).
namespace CloudAppIds {
inline constexpr const char* kOneDriveClientId = "";
inline constexpr const char* kGoogleDriveClientId = "";
inline constexpr const char* kGoogleDriveClientSecret = "";
inline constexpr const char* kDropboxClientId = "";
} // namespace CloudAppIds

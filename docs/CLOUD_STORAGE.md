# Cloud storage (rclone)

Motiva reads pictures and videos from Google Drive, Microsoft OneDrive and Dropbox **only through
[rclone](https://rclone.org)**. Motiva contains no OAuth code, no token refresh and no provider API client:
rclone signs the user in (in their browser), refreshes tokens, lists and downloads files; Motiva starts rclone
with `QProcess` and reads its output (`lsjson`, `--use-json-log`).

Cloud access is **read-only and media-only**. Motiva has no upload, sync, backup, restore, folder-creation or
cloud-delete function. (Older versions of this document described such features; they were removed.)

Status: checked against the source on 2026-11-27. No sign-in, listing or download against a real
Google, OneDrive or Dropbox account has been verified, and the repository has no cloud tests.

Open it from **Playlists → Cloud…**.

## What the user can do

| Page | Function | rclone command |
|---|---|---|
| Accounts | Connect, reconnect, test, remove the account; reset cloud accounts | `config create`, `config update … config_refresh_token=true`, `lsjson`, `config delete` |
| Browse | List and open folders, search the open folder, page through 200 items at a time | `lsjson` |
| Browse | Download selected files to a PC folder | `copy <folder> <dest> --include /<name> --max-depth 1 --immutable` |
| Browse | Add files to the playlist (downloaded into Motiva's local cache, so playback never waits on the network) | `copyto` into `cloud-cache` in local app data |

* **One account at a time.** A second connect is refused until the current account is removed. The account is
  named after the service.
* The path line shows where you are; **Up** and **Refresh** navigate.

### Safety rules in the code
* Only picture and video names (`CloudManager::isAllowedMediaName`; GIF counts as a picture) are listed or
  fetched. Folders are never copied whole.
* Download to folder uses `--immutable`: an existing, different local file is reported, not replaced.
* Connecting asks for read-only access where the service allows the client to choose: Google
  `scope=drive.readonly`; OneDrive `Files.Read Files.Read.All Sites.Read.All offline_access`.
  Dropbox fixes permissions in the app registration, so register that app with read scopes only.
* Cancel sends Ctrl+C to rclone, then hard-kills it after 4 s; `*.partial` leftovers in the plan's destination
  folders are removed.
* **Remove account** deletes it from Motiva's rclone config. Access granted at the provider stays until the user
  revokes it in the provider's security settings.

## Credentials and secrets
* rclone's config (which holds the OAuth tokens) is `rclone/rclone.conf` under Motiva's local app data and is
  **encrypted by rclone** with a random password. The password is stored under Windows DPAPI (current user) in
  Motiva's settings (`cloud/rcloneKey`) and reaches rclone through its process environment, not the command
  line. The user's own `RCLONE_*` variables are removed from that environment.
  If the config cannot be decrypted, Motiva says so and offers **Reset cloud accounts** (deletes only that file
  and the key; the user connects again).
* A client secret saved in Motiva is DPAPI-encrypted. It is passed to `rclone config create` on the command
  line while connecting (Google's desktop "secrets" are not confidential; OneDrive/Dropbox public clients have
  none).
* Error text from rclone is scrubbed (`RcloneJob::scrub`) before it reaches the UI.
* Factory Reset clears Motiva's settings (including the key). It does not delete `rclone.conf`.
  Cleanup's downloaded-media deletion removes the cloud and web caches, never cloud originals.

## API registrations (product owner)

If no client ID is configured, Motiva lets rclone use **its shared registration**, Google included; the code
does not refuse a Google connection. **Do not rely on shared registrations in a release.**

* **Google Drive.** rclone has announced that its shared Google client ID is being retired during 2026 (recorded
  2026-10; not re-checked, and whether it still works is unverified). Create a Google Cloud project, enable the
  *Google Drive API*, configure the OAuth consent screen and create an OAuth client of type **Desktop app**
  (https://rclone.org/drive/#making-your-own-client-id). Publishing to the public may require Google's OAuth
  verification.
* **OneDrive / Dropbox.** Register your own app (Entra ID "Mobile and desktop" app; Dropbox "Scoped access"
  app), redirect `http://localhost:53682/`.
* Where the IDs go (first match wins): value saved through *API credentials…* (the button is shown only when the
  environment variable `MOTIVA_CLOUD_DEV` is set) → `rclone/motiva-cloud.ini` beside `Motiva.exe`
  (`scripts/deploy/Deploy.ps1` copies it from `third_party/rclone/` if present) → rclone's shared registration.

```ini
[gdrive]
client_id=1234-abc.apps.googleusercontent.com
client_secret=GOCSPX-...
[onedrive]
client_id=...
[dropbox]
client_id=...
client_secret=...
```

## rclone packaging and licence
* Bundled: rclone **1.75.2**, `windows-amd64`. Download URL, zip hash and the Motiva modifications (sign-in page
  patch, executable branding) are recorded in `third_party/rclone/README.txt`.
* Licence: **MIT**. `LICENSE.txt` must stay beside `rclone.exe` in every build and deployment. rclone's binary
  also contains Go modules under their own licences; their notices are not collected yet - audit that before a
  commercial release (https://rclone.org/licence/).
* Lookup order at run time: `cloud/rclonePath` setting, `rclone/rclone.exe` or `rclone.exe` beside Motiva, then
  `PATH`. Motiva runs `rclone version` and requires 1.65 or newer.
* `rclone.exe` is git-ignored; the CMake build copies `third_party/rclone/` into `<build>/rclone/`.

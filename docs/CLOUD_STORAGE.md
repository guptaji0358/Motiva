# Cloud storage (rclone)

Motiva talks to Google Drive, Microsoft OneDrive and Dropbox **only through
[rclone](https://rclone.org)**. Motiva contains no OAuth code, no token refresh
and no provider API client: rclone signs the user in (in their browser), refreshes
tokens, lists and transfers files; Motiva starts rclone with `QProcess` and reads
its structured output (`lsjson`, `--use-json-log`, `--combined`).

Open it from **Playlists → Cloud…** (it then also offers *Add to playlist*).

## What the user can do

| Tab | Function | rclone command |
|---|---|---|
| Accounts | Connect / reconnect / test / remove several named accounts | `config create`, `config update … config_refresh_token=true`, `lsjson`, `config delete` |
| Browse | List, open folders, make folders | `lsjson`, `mkdir` |
| Browse | Download selected items to a folder | `copy … --immutable` |
| Browse | Add files to a playlist (downloads into Motiva's local cache first, so playback never waits on the network) | `copyto` into `%LOCALAPPDATA%\…\cloud-cache` |
| Browse | Upload files / folders | `copy … --immutable` (or `--backup-dir` after the user chose *Replace*) |
| Sync | Preview, then **Copy** (adds/updates) or **Mirror** (also removes) a PC folder ↔ cloud folder; Verify | `copy`/`sync --dry-run --combined -`, `copy`/`sync --backup-dir … --max-delete N`, `check` |
| Backup & Restore | Snapshot of the `.mtv` library (`VACUUM INTO`, consistent while Motiva runs) → `Motiva Backup/Library/`; back up a folder → `Motiva Backup/Files/`; restore saves a copy under a fresh name | `copyto`, `copy` |

### Safety rules built in
* Imports, uploads and backups use `copy`, never `sync`; nothing is deleted by them.
* `--immutable`: a *different* existing file is reported (“left untouched”), not replaced.
  (Single-file `copyto` ignores `--immutable`, so single files are copied with an exact
  `--include` filter instead — covered by a test.)
* **Mirror** only runs after a dry-run preview and a confirmation that lists the counts;
  replaced/removed destination files are moved (`--backup-dir`) to `Motiva Sync Trash/<time>`
  on the same cloud account (or `%LOCALAPPDATA%\…\cloud-trash\<time>` for a PC destination),
  `--max-delete` aborts an unexpectedly large removal, and mirroring a drive root, the user's
  home/Documents/Desktop/Pictures folder or a cloud account root is refused.
* Motiva never touches `Motiva Sync Trash` / `Motiva Backup` itself except to write new items.
  Those safety folders are not cleaned automatically (Cleanup & Reset does not know them yet).
* Restore never replaces the live library; it saves `MotivaLibrary-<time> (n).mtv` next to the files.
* Cancel sends Ctrl+C to rclone (it removes its partial files), then a hard kill after 4 s;
  leftover `*.partial` files in the destination folder are swept.

## Credentials and secrets
* rclone's config (which holds the OAuth tokens) lives in `%LOCALAPPDATA%\Motiva\Motiva\rclone\rclone.conf`
  and is **encrypted by rclone** with a random password. That password is stored under
  Windows DPAPI (current user) in Motiva's settings (`cloud/rcloneKey`) and reaches rclone only
  through its process environment (`RCLONE_CONFIG_PASS`). It is never on a command line or in a log.
  If the key is lost (new Windows account, settings wiped) the file cannot be read; Motiva says so
  and offers **Reset cloud accounts** (deletes only that file; the user connects again).
* The optional client secret typed into *API credentials…* is stored DPAPI-encrypted. It is passed
  to `rclone config create` on the command line **once**, while connecting (Google's desktop
  “secrets” are not confidential; OneDrive/Dropbox public clients have none).
* Error text from rclone is scrubbed (`RcloneJob::scrub`) before it reaches the UI.
* Factory Reset clears Motiva's settings (including the key); the then-unreadable
  `rclone.conf` is offered for reset the next time the Cloud window opens.

## Setup required from the product owner  ⚠
rclone ships with its own shared API registrations. **Do not rely on them in a commercial release.**

* **Google Drive — required.** rclone itself reports that its shared Google client ID *“is being
  retired and will stop working during 2026”*, and it is rate limited. Motiva therefore **refuses to
  connect Google Drive until you supply your own client ID**. Create a Google Cloud project, enable
  the *Google Drive API*, configure the OAuth consent screen, and create an OAuth client of type
  **Desktop app** (https://rclone.org/drive/#making-your-own-client-id). Note: the `drive` scope is a
  *restricted* scope — publishing to the public needs Google's OAuth verification (and possibly a
  security assessment); only listed test users work before that. Motiva cannot do this for you.
* **OneDrive / Dropbox — strongly recommended.** They work with rclone's shared registration for
  testing, but register your own app (Entra ID “Mobile and desktop” app with redirect
  `http://localhost:53682/`; Dropbox “Scoped access” app, same redirect) for a release.
* Where the IDs go (first match wins): *Cloud → Accounts → API credentials…* (per user, saved in
  Motiva) → `motiva-cloud.ini` placed in `resources/bin/rclone/` (shipped by you, copied there by
  `deploy/Deploy.ps1` from `third_party/rclone/`):

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
All redirect URIs must be `http://localhost:53682/` (rclone's loopback listener).

## rclone packaging and licence
* Bundled: rclone **1.75.2**, `windows-amd64`, from
  `https://downloads.rclone.org/v1.75.2/rclone-v1.75.2-windows-amd64.zip`
  (SHA-256 `8a9db2ba…aff818`, equal to the published `SHA256SUMS`; the PGP signature of that file
  was not verified). Source of truth: `third_party/rclone/README.txt`.
* Licence: **MIT** (“Copyright (C) 2012 by Nick Craig-Wood”). `LICENSE.txt` is copied beside
  `rclone.exe` in every build/deployment (`resources/bin/rclone/`) — it must stay with the binary.
  rclone's binary also contains many Go modules under their own licences (mostly BSD/MIT/Apache,
  some MPL); their notices are not collected yet — do that audit before a commercial release
  (see https://rclone.org/licence/ and the module list in the rclone source tag).
* Lookup order at run time: `cloud/rclonePath` setting, `<Motiva>/rclone/rclone.exe`, then `PATH`.
  Motiva runs `rclone version`, requires ≥ 1.65 and shows the version and licence-file status in the window.
* `rclone.exe` (85 MB) is git-ignored; the CMake build copies `third_party/rclone/` into `<build>/rclone/`.

## 2026-10-23 addendum - end-user flow
* End users never see client IDs: the "API credentials..." button is hidden unless the env var
  `MOTIVA_CLOUD_DEV` is set. IDs come from `motiva-cloud.ini` (shipped by the publisher).
* Browse tab: breadcrumbs (click any level), search box (filters the open folder), 200-item pages.
* Sync tab, mode **Two-way**: files on one side only are copied to the other (`--ignore-existing`);
  a file that differs on both sides is a conflict - the cloud version is saved beside the local one as
  `name (cloud copy <time>).ext`; deletions are never propagated; the run is blocked if the preview
  had comparison errors. (`CloudManager::planTwoWay`; preview = `check --combined`.)
* Fixed: `check --combined` marks `+` = only on the source, `-` = only on the destination
  (the Verify report had these swapped).

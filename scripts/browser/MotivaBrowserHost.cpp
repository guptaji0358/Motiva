// MotivaBrowserHost.exe - Chrome/Edge/Brave Native Messaging host for the
// Motiva browser extension.
//
// Deliberately tiny and Qt-free, like MotivaShell.dll: the browser starts one
// copy per extension connection, speaking length-prefixed JSON on stdin/stdout.
// This program does not interpret requests (besides "ping"); it relays each
// one to the running Motiva window as WM_COPYDATA (starting Motiva first when
// the request needs it) and relays Motiva's JSON replies back to the browser.
//
//   browser --stdin--> host --WM_COPYDATA(5, UTF-16 JSON)--> Motiva
//   browser <-stdout-- host <--WM_COPYDATA(6, UTF-16 JSON)-- Motiva
//
// Motiva learns where to send replies from the WPARAM (this host's window).

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr DWORD kRequestCommand = 5; // host -> Motiva
constexpr DWORD kReplyCommand = 6;   // Motiva -> host
constexpr const wchar_t* kMotivaClass = L"MotivaIpcWindowClass";
constexpr const wchar_t* kHostClass = L"MotivaBrowserHostWindow";
constexpr const char* kVersion = "1.0";
constexpr uint32_t kMaxIncoming = 80u * 1024u * 1024u; // Chrome's own limit is 64 MiB
constexpr UINT WM_HOST_QUIT = WM_APP + 1;
// The only extension allowed to use this host (also enforced by the browser
// through "allowed_origins" in the host manifest).
constexpr const char* kAllowedOrigin = "chrome-extension://decemojbcdaodchnbeopkdofadijfkgi/";

HANDLE g_in = nullptr;
HANDLE g_out = nullptr;
HWND g_window = nullptr;
DWORD g_mainThread = 0;
std::mutex g_writeMutex;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const wchar_t* w, int count) {
    if (count <= 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, count, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, count, &s[0], n, nullptr, nullptr);
    return s;
}

bool ReadExact(void* buffer, DWORD size) {
    auto* p = static_cast<char*>(buffer);
    while (size > 0) {
        DWORD got = 0;
        if (!ReadFile(g_in, p, size, &got, nullptr) || got == 0) return false;
        p += got;
        size -= got;
    }
    return true;
}

void WriteMessage(const std::string& json) {
    std::lock_guard<std::mutex> lock(g_writeMutex);
    const uint32_t len = static_cast<uint32_t>(json.size());
    DWORD written = 0;
    WriteFile(g_out, &len, sizeof(len), &written, nullptr);
    WriteFile(g_out, json.data(), len, &written, nullptr);
    FlushFileBuffers(g_out);
}

// Minimal scans over the extension's own JSON (JSON.stringify output). An
// embedded quote inside a string value is always escaped, so an unescaped
// `"key":` can only be a key.
std::string FindStringField(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    const size_t start = at + needle.size();
    const size_t end = json.find('"', start);
    return end == std::string::npos ? std::string() : json.substr(start, end - start);
}

std::string FindNumberField(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return "0";
    size_t i = at + needle.size();
    std::string digits;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') digits += json[i++];
    return digits.empty() ? "0" : digits;
}

HWND FindMotivaWindow() {
    HWND w = FindWindowExW(HWND_MESSAGE, nullptr, kMotivaClass, nullptr);
    return w ? w : FindWindowW(kMotivaClass, nullptr);
}

std::wstring MotivaExePath() {
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    return dir + L"Motiva.exe";
}

// Starts Motiva (tray-only, like Windows autostart) and waits for its IPC window.
HWND EnsureMotivaRunning() {
    HWND w = FindMotivaWindow();
    if (w) return w;
    const std::wstring exe = MotivaExePath();
    std::wstring cmd = L"\"" + exe + L"\" --autostart";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (int i = 0; i < 300 && !w; ++i) { // up to ~30 s (cold start under load)
        Sleep(100);
        w = FindMotivaWindow();
    }
    return w;
}

void Reply(const std::string& id, bool ok, const std::string& extra) {
    std::string json = "{\"id\":" + id + ",\"ok\":" + (ok ? "true" : "false");
    if (!extra.empty()) json += "," + extra;
    json += "}";
    WriteMessage(json);
}

void HandleRequest(const std::string& json) {
    const std::string id = FindNumberField(json, "id");
    const std::string type = FindStringField(json, "type");

    if (type == "ping") {
        const bool running = FindMotivaWindow() != nullptr;
        Reply(id, true, std::string("\"version\":\"") + kVersion + "\",\"appRunning\":" + (running ? "true" : "false"));
        return;
    }

    HWND target = nullptr;
    if (type == "playlists") {
        // A passive refresh must never launch Motiva.
        target = FindMotivaWindow();
        if (!target) {
            Reply(id, false, "\"error\":\"not-running\"");
            return;
        }
    } else if (type == "abort") {
        target = FindMotivaWindow();
        if (!target) {
            Reply(id, true, "");
            return;
        }
    } else {
        target = EnsureMotivaRunning();
        if (!target) {
            Reply(id, false, "\"error\":\"Motiva could not be started\"");
            return;
        }
    }

    const std::wstring payload = Widen(json);
    COPYDATASTRUCT cds{};
    cds.dwData = kRequestCommand;
    cds.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(payload.c_str());
    DWORD_PTR result = 0;
    // Bounded: Motiva only queues the request here, it does not do the work
    // inside the call (replies arrive later through WM_COPYDATA, kReplyCommand).
    if (!SendMessageTimeoutW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(g_window),
                             reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 15000, &result)) {
        Reply(id, false, "\"error\":\"Motiva is not responding\"");
    }
}

DWORD WINAPI ReaderThread(LPVOID) {
    for (;;) {
        uint32_t len = 0;
        if (!ReadExact(&len, sizeof(len))) break;
        if (len == 0 || len > kMaxIncoming) break;
        std::string body(len, '\0');
        if (!ReadExact(&body[0], len)) break;
        HandleRequest(body);
    }
    PostMessageW(g_window, WM_HOST_QUIT, 0, 0); // browser closed the connection
    return 0;
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COPYDATA) {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (cds && cds->dwData == kReplyCommand && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
            WriteMessage(Narrow(static_cast<const wchar_t*>(cds->lpData),
                                static_cast<int>(cds->cbData / sizeof(wchar_t))));
        }
        return TRUE;
    }
    if (msg == WM_HOST_QUIT) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int main(int argc, char** argv) {
    // Chrome passes the calling extension's origin as the first argument.
    if (argc >= 2 && std::string(argv[1]) != kAllowedOrigin) {
        return 2;
    }
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_mainThread = GetCurrentThreadId();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    g_window = CreateWindowExW(0, kHostClass, L"MotivaBrowserHost", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                               wc.hInstance, nullptr);
    if (!g_window) return 1;

    HANDLE reader = CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
    if (!reader) return 1;

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    TerminateThread(reader, 0); // may be blocked reading stdin
    CloseHandle(reader);
    return 0;
}

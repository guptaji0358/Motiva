// Deployment-root launcher (see the "Organize Final Deployment Output"
// task). This is deliberately the ONLY thing that ships as
// "<Product>.exe" at the deployment root - it links against nothing but
// the Win32 API, so it carries zero Qt/FFmpeg/MinGW-runtime DLL
// dependencies of its own and can sit alone next to resources/ with
// nothing else beside it.
//
// The real, Qt-linked application binary lives at resources/bin/<same
// exe name> instead. Windows resolves an executable's own *implicit*
// (load-time) DLL imports before that executable's own main()/WinMain()
// ever runs, so nothing inside the real binary's own startup code could
// redirect its own DLL search path away from resources/bin/ - only an
// earlier, separate process (this one) can do that, by modifying the
// environment PATH the OS loader will consult before creating that
// process. Qt plugin discovery (platforms/styles/imageformats/...) is a
// separate, Qt-internal mechanism instead, pointed at resources/plugins/
// via resources/bin/qt.conf - this launcher has no involvement in that
// part.
//
// This is intentionally a thin, static, side-effect-free process
// shell: read our own path, extend PATH for the child, forward the
// command line and working directory, launch, exit. Nothing here
// touches wallpaper/recovery/IPC state - that all still lives entirely
// in the real binary, unchanged.

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace {

std::wstring exeDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L".";
    }
    const std::wstring path(buffer, len);
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : path.substr(0, pos);
}

// Reconstructs "<appExe>" <original args, quoted>, deliberately dropping
// this launcher's own argv[0] rather than reusing GetCommandLineW()
// verbatim (which would otherwise duplicate/mismatch the target path) -
// existing arguments such as main.cpp's "--autostart" flag still reach
// the real binary unchanged.
std::wstring buildChildCommandLine(const std::wstring& appExe) {
    std::wstring commandLine = L"\"" + appExe + L"\"";
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            commandLine += L" \"";
            commandLine += argv[i];
            commandLine += L"\"";
        }
        LocalFree(argv);
    }
    return commandLine;
}

void prependToPath(const std::wstring& dir) {
    wchar_t existing[32768];
    const DWORD existingLen = GetEnvironmentVariableW(L"PATH", existing, 32768);
    std::wstring newPath = dir;
    if (existingLen > 0 && existingLen < 32768) {
        newPath += L";";
        newPath += existing;
    }
    SetEnvironmentVariableW(L"PATH", newPath.c_str());
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring baseDir = exeDirectory();
    const std::wstring binDir = baseDir + L"\\resources\\bin";
    const std::wstring appExe = binDir + L"\\Motiva.exe";
    const std::wstring qtDllDir = baseDir + L"\\resources\\Qt";

    // Only this process's own environment is modified; CreateProcessW
    // below passes lpEnvironment=nullptr, meaning the child inherits
    // that (now-modified) block directly - no manual double-null-
    // terminated environment block construction needed.
    prependToPath(qtDllDir);

    std::wstring commandLine = buildChildCommandLine(appExe);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    const BOOL created = CreateProcessW(appExe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
        nullptr, binDir.c_str(), &startupInfo, &processInfo);

    if (!created) {
        const DWORD error = GetLastError();
        wchar_t message[512];
        wsprintfW(message,
            L"Failed to launch Motiva.\n\n%s\n\nWindows error code: %lu", appExe.c_str(), error);
        MessageBoxW(nullptr, message, L"Motiva", MB_OK | MB_ICONERROR);
        return 1;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return 0;
}

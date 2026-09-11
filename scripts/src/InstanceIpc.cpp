#include "InstanceIpc.h"
#include <QDebug>
#include <QMetaObject>

namespace {
constexpr const wchar_t* kClassName = L"MotivaIpcWindowClass";
constexpr DWORD kRecoverCommand = 1;
ATOM g_classAtom = 0;

void EnsureClassRegistered() {
    if (g_classAtom != 0) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &InstanceIpc::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    g_classAtom = RegisterClassExW(&wc);
}
} // namespace

InstanceIpc::InstanceIpc(QObject* parent) : QObject(parent) {}

InstanceIpc::~InstanceIpc() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool InstanceIpc::startListening() {
    EnsureClassRegistered();
    // HWND_MESSAGE parent: a message-only window, never visible, never
    // enumerated by EnumWindows - just a target for SendMessage/WM_COPYDATA.
    m_hwnd = CreateWindowExW(0, kClassName, L"MotivaIpc", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) {
        qWarning() << "[IPC] Failed to create the recovery-request receiver window, GetLastError="
                   << GetLastError();
        return false;
    }
    qInfo() << "[IPC] Listening for second-instance recovery requests, hwnd="
            << reinterpret_cast<quintptr>(m_hwnd);
    return true;
}

bool InstanceIpc::sendRecoverRequest() {
    HWND target = FindWindowW(kClassName, nullptr);
    if (!target) {
        qWarning() << "[IPC] Second instance detected, but no existing instance's IPC "
                       "window was found yet (it may still be starting up) - exiting anyway "
                       "since the single-instance lock proves a process is running.";
        return false;
    }
    qInfo() << "[IPC] Second instance detected - sending recovery request to hwnd="
            << reinterpret_cast<quintptr>(target);

    COPYDATASTRUCT cds{};
    cds.dwData = kRecoverCommand;
    cds.cbData = 0;
    cds.lpData = nullptr;
    LRESULT result = SendMessageW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
    qInfo() << "[IPC] Recovery request sent, result=" << result;
    return true;
}

LRESULT CALLBACK InstanceIpc::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<InstanceIpc*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_COPYDATA && self) {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        if (cds && cds->dwData == kRecoverCommand) {
            qInfo() << "[IPC] Existing instance received recovery request.";
            // WndProc for a raw HWND runs synchronously on whatever thread
            // pumps this message (the GUI thread here, since the window was
            // created on it) - a queued connection still keeps this
            // decoupled from WndProc's own call stack/return value, the
            // same pattern WallpaperManager uses for TaskbarCreated.
            QMetaObject::invokeMethod(self, "recoverRequested", Qt::QueuedConnection);
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

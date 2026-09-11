#pragma once

#include <QObject>
#include <windows.h>

// Single-instance recovery hand-off. The QSharedMemory lock in main.cpp
// stays the sole mutual-exclusion mechanism (exactly one Motiva.exe
// process ever runs); this class only adds a way for a second launch
// attempt to tell the surviving instance "please recover/activate" instead
// of silently exiting with no effect, which previously forced the user to
// End Task the existing process to get a usable instance back.
//
// Implemented as a hidden message-only window (HWND_MESSAGE) + WM_COPYDATA,
// matching the app's existing native-Win32-plus-Qt-signal style (see
// WallpaperManager's TaskbarCreated handling) rather than adding a new IPC
// dependency (named pipe/socket).
class InstanceIpc : public QObject {
    Q_OBJECT
public:
    explicit InstanceIpc(QObject* parent = nullptr);
    ~InstanceIpc() override;

    // Creates this process's message-only receiver window. Call once, only
    // by the instance that won the QSharedMemory lock.
    bool startListening();

    // Called by a second launch that lost the QSharedMemory lock: finds the
    // first instance's receiver window and asks it to recover/activate.
    // Returns false if no listener could be found (e.g. the first instance
    // is still starting up) - callers should just exit either way, since
    // the QSharedMemory lock already proves an instance exists.
    static bool sendRecoverRequest();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

signals:
    // Emitted (via a queued connection, since the raw WndProc can run
    // whenever Windows delivers the message) when another launch attempt
    // asked this instance to recover/activate.
    void recoverRequested();

private:
    HWND m_hwnd = nullptr;
};

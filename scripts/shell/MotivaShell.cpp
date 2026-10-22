// MotivaShell.dll - tiny, Qt-free Explorer helper (per-user COM server).
//
// Implements IExplorerCommandState for the folder "Motiva" verb
// (HKCU\Software\Classes\Directory\shell\Motiva\CommandStateHandler): Explorer
// asks it, per right-click, whether the verb is shown. It is hidden unless the
// selected folder directly contains at least one image. Static registry verbs
// cannot look at a folder's contents, which is the only reason this DLL exists.
//
// The recognised image extensions are read from the CLSID key's
// "ImageExtensions" value (written by WindowsShellIntegration from the same
// list the app uses), so there is a single source of truth.

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cwchar>
#include <cwctype>
#include <new>
#include <string>
#include <vector>

// {7B3F0C52-5E1A-4D3B-9A44-6C2D8E1F0B97}
static const CLSID CLSID_MotivaFolderState = {
    0x7b3f0c52, 0x5e1a, 0x4d3b, {0x9a, 0x44, 0x6c, 0x2d, 0x8e, 0x1f, 0x0b, 0x97}};

static const wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\{7B3F0C52-5E1A-4D3B-9A44-6C2D8E1F0B97}";

static LONG g_dllRefs = 0;

namespace {

// ".jpg;.png" -> {L".jpg", L".png"} (lowercase, with dot).
std::vector<std::wstring> loadImageExtensions() {
    std::vector<std::wstring> out;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kClsidKey, L"ImageExtensions", RRF_RT_REG_SZ, nullptr, buf, &size) ==
        ERROR_SUCCESS) {
        std::wstring cur;
        for (const wchar_t* p = buf;; ++p) {
            if (*p == L';' || *p == L'\0') {
                if (!cur.empty()) {
                    out.push_back(cur);
                }
                cur.clear();
                if (*p == L'\0') {
                    break;
                }
            } else {
                cur += static_cast<wchar_t>(std::towlower(*p));
            }
        }
    }
    return out;
}

bool hasImageExtension(const wchar_t* name, const std::vector<std::wstring>& exts) {
    const wchar_t* dot = std::wcsrchr(name, L'.');
    if (!dot) {
        return false;
    }
    std::wstring ext;
    for (const wchar_t* p = dot; *p; ++p) {
        ext += static_cast<wchar_t>(std::towlower(*p));
    }
    for (const std::wstring& e : exts) {
        if (e == ext) {
            return true;
        }
    }
    return false;
}

// True as soon as one image file is found directly inside `folder`. Bounded
// (entries and time) so a pathological folder can never stall Explorer's menu.
bool folderHasImage(const std::wstring& folder, const std::vector<std::wstring>& exts) {
    std::wstring pattern = folder;
    if (!pattern.empty() && pattern.back() != L'\\') {
        pattern += L'\\';
    }
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const ULONGLONG start = GetTickCount64();
    bool found = false;
    int seen = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        if (hasImageExtension(fd.cFileName, exts)) {
            found = true;
            break;
        }
        if (++seen > 50000 || (seen % 256 == 0 && GetTickCount64() - start > 400)) {
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

class FolderState final : public IExplorerCommandState {
public:
    FolderState() { InterlockedIncrement(&g_dllRefs); }
    ~FolderState() { InterlockedDecrement(&g_dllRefs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IExplorerCommandState) {
            *ppv = static_cast<IExplorerCommandState*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&m_refs);
        if (n == 0) {
            delete this;
        }
        return static_cast<ULONG>(n);
    }

    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL /*okToBeSlow*/, EXPCMDSTATE* state) override {
        if (!state) {
            return E_POINTER;
        }
        *state = ECS_HIDDEN;
        if (!items) {
            return S_OK;
        }
        DWORD count = 0;
        if (FAILED(items->GetCount(&count))) {
            return S_OK;
        }
        const std::vector<std::wstring> exts = loadImageExtensions();
        if (exts.empty()) {
            return S_OK;
        }
        // Shown if ANY selected folder has an image (the command then runs
        // once per selected folder; the app reports folders without images).
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* item = nullptr;
            if (FAILED(items->GetItemAt(i, &item)) || !item) {
                continue;
            }
            PWSTR path = nullptr;
            const bool ok = SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path;
            const bool has = ok && folderHasImage(path, exts);
            if (path) {
                CoTaskMemFree(path);
            }
            item->Release();
            if (has) {
                *state = ECS_ENABLED;
                break;
            }
        }
        return S_OK;
    }

private:
    LONG m_refs = 1;
};

class Factory final : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&m_refs);
        if (n == 0) {
            delete this;
        }
        return static_cast<ULONG>(n);
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        FolderState* obj = new (std::nothrow) FolderState();
        if (!obj) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&g_dllRefs);
        } else {
            InterlockedDecrement(&g_dllRefs);
        }
        return S_OK;
    }

private:
    LONG m_refs = 1;
};

} // namespace

extern "C" {

__declspec(dllexport) HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (clsid != CLSID_MotivaFolderState) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    Factory* factory = new (std::nothrow) Factory();
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

__declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow() {
    return g_dllRefs == 0 ? S_OK : S_FALSE;
}

} // extern "C"

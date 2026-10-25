// MotivaShell.dll - tiny, Qt-free Explorer helper (per-user COM server).
//
// One IExplorerCommand implements the whole right-click "Motiva" menu:
//
//   Motiva  >  Set as wallpaper               (exactly one image/video/GIF selected)
//              Add to "<last-used playlist>"  (quick repeat)
//              Add to playlist  >  <playlists that can hold the selection>
//                                  Create New Playlist...
//
// Why a COM handler instead of static registry verbs: it receives the WHOLE
// selection in one call (static verbs launch one process per file and are
// dropped beyond 15 files), it can look at what is selected (folders only
// qualify when they directly contain an image; a mixed image+video selection
// lists only mixed playlists), and it is the same interface the Windows 11
// top-level menu uses.
//
// Registered per-user by WindowsShellIntegration: the verb
// ...\shell\Motiva has ExplorerCommandHandler={CLSID}. Everything it needs
// (extension lists, playlists, last-used playlist, the exe) is read from
// values on the CLSID key, which the app rewrites whenever the library changes.
// Invoke never talks to the app directly: it writes the selection to a temp
// file and starts "Motiva.exe --explorer-add <playlistId|0> <file>" (a second
// launch hands it to the running instance).

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstring>
#include <cwchar>
#include <cwctype>
#include <new>
#include <string>
#include <vector>

// {2D6F4A18-91C3-4E7B-8A05-3B7C9E5D1F42}
static const CLSID CLSID_MotivaMenu = {
    0x2d6f4a18, 0x91c3, 0x4e7b, {0x8a, 0x05, 0x3b, 0x7c, 0x9e, 0x5d, 0x1f, 0x42}};

static const wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\{2D6F4A18-91C3-4E7B-8A05-3B7C9E5D1F42}";

static LONG g_dllRefs = 0;

namespace {

using std::vector;
using std::wstring;

// ---- configuration written by the app (values on the CLSID key) ----

struct PlaylistInfo {
    LONGLONG id = 0;
    wchar_t kind = L'i'; // i = image, v = video, m = mixed
    wstring name;
};

struct Config {
    wstring exe;
    wstring icon;
    vector<wstring> imageExts, videoExts, otherExts; // lowercase, with dot
    LONGLONG lastPlaylistId = 0;
    vector<PlaylistInfo> playlists;
};

vector<wchar_t> readRegValue(const wchar_t* name, DWORD flags) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kClsidKey, name, flags, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) {
        return {};
    }
    vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
    DWORD got = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kClsidKey, name, flags, nullptr, buf.data(), &got) != ERROR_SUCCESS) {
        return {};
    }
    return buf;
}

wstring readString(const wchar_t* name) {
    const vector<wchar_t> buf = readRegValue(name, RRF_RT_REG_SZ);
    return buf.empty() ? wstring() : wstring(buf.data());
}

// ".jpg;.png" -> {L".jpg", L".png"}
vector<wstring> splitExtensions(const wstring& list) {
    vector<wstring> out;
    wstring cur;
    for (size_t i = 0; i <= list.size(); ++i) {
        if (i == list.size() || list[i] == L';') {
            if (!cur.empty()) {
                out.push_back(cur);
            }
            cur.clear();
        } else {
            cur += static_cast<wchar_t>(std::towlower(list[i]));
        }
    }
    return out;
}

Config loadConfig() {
    Config c;
    c.exe = readString(L"Exe");
    c.icon = readString(L"Icon");
    c.imageExts = splitExtensions(readString(L"ImageExtensions"));
    c.videoExts = splitExtensions(readString(L"VideoExtensions"));
    c.otherExts = splitExtensions(readString(L"OtherExtensions"));
    c.lastPlaylistId = _wtoi64(readString(L"LastPlaylistId").c_str());
    // REG_MULTI_SZ, one playlist per string: "<id>|<i|v|m>|<name>"
    const vector<wchar_t> multi = readRegValue(L"Playlists", RRF_RT_REG_MULTI_SZ);
    for (const wchar_t* p = multi.data(); !multi.empty() && *p;) {
        const wstring line(p);
        p += line.size() + 1;
        const size_t a = line.find(L'|');
        const size_t b = a == wstring::npos ? wstring::npos : line.find(L'|', a + 1);
        if (b == wstring::npos || b != a + 2) {
            continue;
        }
        PlaylistInfo info;
        info.id = _wtoi64(line.substr(0, a).c_str());
        info.kind = line[a + 1];
        info.name = line.substr(b + 1);
        if (info.id > 0 && (info.kind == L'i' || info.kind == L'v' || info.kind == L'm')) {
            c.playlists.push_back(info);
        }
    }
    return c;
}

// ---- selection analysis ----

bool inList(const vector<wstring>& exts, const wstring& ext) {
    for (const wstring& e : exts) {
        if (e == ext) {
            return true;
        }
    }
    return false;
}

wstring lowerExtension(const wchar_t* name) {
    const wchar_t* dot = std::wcsrchr(name, L'.');
    wstring ext;
    for (const wchar_t* p = dot ? dot : L""; *p; ++p) {
        ext += static_cast<wchar_t>(std::towlower(*p));
    }
    return ext;
}

// True as soon as one image file is found directly inside `folder`. Bounded
// (entries and time) so a pathological folder can never stall Explorer's menu.
bool folderHasImage(const wstring& folder, const vector<wstring>& exts) {
    wstring pattern = folder;
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
        if (inList(exts, lowerExtension(fd.cFileName))) {
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

struct Selection {
    DWORD itemCount = 0;
    int images = 0;  // image files + folders that directly contain an image
    int videos = 0;
    int others = 0;  // GIF etc.: can be a wallpaper but no playlist holds it
    bool singleFile = false;

    bool playable() const { return images > 0 || videos > 0; }
    bool canSetWallpaper() const { return singleFile && (images + videos + others) == 1; }
    bool any() const { return playable() || canSetWallpaper(); }
};

wstring itemPath(IShellItem* item) {
    PWSTR p = nullptr;
    wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = p;
    }
    if (p) {
        CoTaskMemFree(p);
    }
    return out;
}

vector<wstring> selectedPaths(IShellItemArray* items) {
    vector<wstring> out;
    DWORD count = 0;
    if (!items || FAILED(items->GetCount(&count))) {
        return out;
    }
    for (DWORD i = 0; i < count; ++i) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
            const wstring path = itemPath(item);
            if (!path.empty()) {
                out.push_back(path);
            }
            item->Release();
        }
    }
    return out;
}

Selection analyze(IShellItemArray* items, const Config& cfg) {
    Selection sel;
    const vector<wstring> paths = selectedPaths(items);
    sel.itemCount = static_cast<DWORD>(paths.size());
    bool anyFolder = false;
    for (const wstring& path : paths) {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            anyFolder = true;
            if (folderHasImage(path, cfg.imageExts)) {
                ++sel.images;
            }
            continue;
        }
        const wstring ext = lowerExtension(path.c_str());
        if (inList(cfg.imageExts, ext)) {
            ++sel.images;
        } else if (inList(cfg.videoExts, ext)) {
            ++sel.videos;
        } else if (inList(cfg.otherExts, ext)) {
            ++sel.others;
        }
    }
    sel.singleFile = paths.size() == 1 && !anyFolder;
    return sel;
}

bool playlistFits(const PlaylistInfo& p, const Selection& sel) {
    if (sel.images > 0 && sel.videos > 0) {
        return p.kind == L'm'; // mixed selection: only a mixed playlist holds both
    }
    if (sel.images > 0) {
        return p.kind == L'i' || p.kind == L'm';
    }
    if (sel.videos > 0) {
        return p.kind == L'v' || p.kind == L'm';
    }
    return false;
}

// ---- launching the app ----

wstring quote(const wstring& s) {
    return L"\"" + s + L"\"";
}

bool startApp(const wstring& exe, const wstring& args) {
    if (exe.empty()) {
        return false;
    }
    wstring cmd = quote(exe) + L" " + args;
    vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    wstring dir = exe;
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == wstring::npos ? wstring() : dir.substr(0, slash);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// One path per line, UTF-8. A newline cannot occur in a Windows path.
wstring writeSelectionFile(const vector<wstring>& paths) {
    wchar_t tmp[MAX_PATH + 1] = {};
    if (!GetTempPathW(MAX_PATH, tmp)) {
        return {};
    }
    static LONG counter = 0;
    wchar_t name[MAX_PATH + 64] = {};
    swprintf(name, MAX_PATH + 63, L"%lsmotiva-explorer-%lu-%llu-%ld.txt", tmp, GetCurrentProcessId(),
             static_cast<unsigned long long>(GetTickCount64()), InterlockedIncrement(&counter));
    HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    bool ok = true;
    for (const wstring& path : paths) {
        const wstring line = path + L"\n";
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0,
                                              nullptr, nullptr);
        vector<char> utf8(static_cast<size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr,
                            nullptr);
        DWORD written = 0;
        ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && ok;
    }
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(name);
        return {};
    }
    return name;
}

HRESULT dupString(const wstring& s, LPWSTR* out) {
    if (!out) {
        return E_POINTER;
    }
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    *out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!*out) {
        return E_OUTOFMEMORY;
    }
    std::memcpy(*out, s.c_str(), bytes);
    return S_OK;
}

// Menu text: '&' is an accelerator marker, long names are cut.
wstring menuText(wstring name) {
    if (name.size() > 40) {
        name = name.substr(0, 39) + L"\x2026";
    }
    wstring out;
    for (wchar_t c : name) {
        out += c;
        if (c == L'&') {
            out += L'&';
        }
    }
    return out;
}

// ---- commands ----

enum class Role { Root, SetWallpaper, AddMenu, AddTo, CreateNew };

class Cmd final : public IExplorerCommand {
public:
    Cmd(Role role, wstring title, wstring exe) : m_role(role), m_title(std::move(title)), m_exe(std::move(exe)) {
        InterlockedIncrement(&g_dllRefs);
    }
    ~Cmd() {
        for (IExplorerCommand* child : m_children) {
            child->Release();
        }
        InterlockedDecrement(&g_dllRefs);
    }

    void setPlaylistId(LONGLONG id) { m_playlistId = id; }
    void setIcon(wstring icon) { m_icon = std::move(icon); }
    void addChild(Cmd* child) { m_children.push_back(child); } // takes the caller's reference

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IExplorerCommand) {
            *ppv = static_cast<IExplorerCommand*>(this);
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

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override { return dupString(m_title, name); }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
        if (m_icon.empty()) {
            if (icon) {
                *icon = nullptr;
            }
            return E_NOTIMPL;
        }
        return dupString(m_icon, icon);
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        if (tip) {
            *tip = nullptr;
        }
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override {
        if (!guid) {
            return E_POINTER;
        }
        *guid = GUID_NULL;
        return S_OK;
    }

    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        if (!state) {
            return E_POINTER;
        }
        *state = ECS_ENABLED;
        if (m_role != Role::Root) {
            return S_OK;
        }
        // The root is asked first with the selection; remember it for
        // EnumSubCommands(), which gets no items.
        for (IExplorerCommand* child : m_children) {
            child->Release();
        }
        m_children.clear();
        m_config = loadConfig();
        m_selection = analyze(items, m_config);
        if (!m_selection.any() || m_config.exe.empty()) {
            *state = ECS_HIDDEN;
        }
        return S_OK;
    }

    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        if (m_role == Role::Root || m_role == Role::AddMenu) {
            return S_OK;
        }
        const vector<wstring> paths = selectedPaths(items);
        if (paths.empty()) {
            return S_OK;
        }
        bool started = false;
        if (m_role == Role::SetWallpaper) {
            started = startApp(m_exe, L"--set-background " + quote(paths.front()));
        } else {
            const wstring file = writeSelectionFile(paths);
            if (!file.empty()) {
                started = startApp(m_exe, L"--explorer-add " + std::to_wstring(m_role == Role::CreateNew ? 0 : m_playlistId) +
                                              L" " + quote(file));
                if (!started) {
                    DeleteFileW(file.c_str());
                }
            }
        }
        return started ? S_OK : E_FAIL;
    }

    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        if (!flags) {
            return E_POINTER;
        }
        DWORD f = ECF_DEFAULT;
        if (m_role == Role::Root || m_role == Role::AddMenu) {
            f |= ECF_HASSUBCOMMANDS;
        }
        if (m_separatorBefore) {
            f |= ECF_SEPARATORBEFORE;
        }
        *flags = static_cast<EXPCMDFLAGS>(f);
        return S_OK;
    }

    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** out) override;

    void setSeparatorBefore(bool on) { m_separatorBefore = on; }

private:
    void buildRootChildren();

    LONG m_refs = 1;
    Role m_role;
    wstring m_title;
    wstring m_exe;
    wstring m_icon;
    LONGLONG m_playlistId = 0;
    bool m_separatorBefore = false;
    vector<IExplorerCommand*> m_children;
    Config m_config;
    Selection m_selection;
};

class EnumCmds final : public IEnumExplorerCommand {
public:
    explicit EnumCmds(const vector<IExplorerCommand*>& items, size_t start = 0) : m_items(items), m_index(start) {
        for (IExplorerCommand* item : m_items) {
            item->AddRef();
        }
        InterlockedIncrement(&g_dllRefs);
    }
    ~EnumCmds() {
        for (IExplorerCommand* item : m_items) {
            item->Release();
        }
        InterlockedDecrement(&g_dllRefs);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IEnumExplorerCommand) {
            *ppv = static_cast<IEnumExplorerCommand*>(this);
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

    IFACEMETHODIMP Next(ULONG count, IExplorerCommand** out, ULONG* fetched) override {
        ULONG n = 0;
        while (n < count && m_index < m_items.size()) {
            out[n] = m_items[m_index++];
            out[n]->AddRef();
            ++n;
        }
        if (fetched) {
            *fetched = n;
        }
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        m_index += count;
        if (m_index > m_items.size()) {
            m_index = m_items.size();
            return S_FALSE;
        }
        return S_OK;
    }
    IFACEMETHODIMP Reset() override {
        m_index = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumExplorerCommand** out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = new (std::nothrow) EnumCmds(m_items, m_index);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

private:
    LONG m_refs = 1;
    vector<IExplorerCommand*> m_items;
    size_t m_index;
};

void Cmd::buildRootChildren() {
    if (!m_children.empty()) {
        return;
    }
    const Config& cfg = m_config;
    const Selection& sel = m_selection;
    if (sel.canSetWallpaper()) {
        m_children.push_back(new (std::nothrow) Cmd(Role::SetWallpaper, L"Set as wallpaper", cfg.exe));
    }
    if (!sel.playable()) {
        return;
    }
    vector<const PlaylistInfo*> fitting;
    for (const PlaylistInfo& p : cfg.playlists) {
        if (playlistFits(p, sel)) {
            fitting.push_back(&p);
        }
    }
    for (const PlaylistInfo* p : fitting) {
        if (p->id == cfg.lastPlaylistId) {
            Cmd* quick = new (std::nothrow) Cmd(Role::AddTo, L"Add to “" + menuText(p->name) + L"”", cfg.exe);
            if (quick) {
                quick->setPlaylistId(p->id);
                m_children.push_back(quick);
            }
            break;
        }
    }
    Cmd* menu = new (std::nothrow) Cmd(Role::AddMenu, L"Add to playlist", cfg.exe);
    if (!menu) {
        return;
    }
    for (const PlaylistInfo* p : fitting) {
        Cmd* entry = new (std::nothrow) Cmd(Role::AddTo, menuText(p->name), cfg.exe);
        if (entry) {
            entry->setPlaylistId(p->id);
            menu->addChild(entry);
        }
    }
    Cmd* create = new (std::nothrow) Cmd(Role::CreateNew, L"Create New Playlist\x2026", cfg.exe);
    if (create) {
        create->setSeparatorBefore(!fitting.empty());
        menu->addChild(create);
    }
    m_children.push_back(menu);
}

IFACEMETHODIMP Cmd::EnumSubCommands(IEnumExplorerCommand** out) {
    if (!out) {
        return E_POINTER;
    }
    *out = nullptr;
    if (m_role == Role::Root) {
        buildRootChildren();
    } else if (m_role != Role::AddMenu) {
        return E_NOTIMPL;
    }
    *out = new (std::nothrow) EnumCmds(m_children);
    return *out ? S_OK : E_OUTOFMEMORY;
}

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
        Cmd* obj = new (std::nothrow) Cmd(Role::Root, L"Motiva", wstring());
        if (!obj) {
            return E_OUTOFMEMORY;
        }
        const Config cfg = loadConfig();
        if (!cfg.icon.empty()) {
            obj->setIcon(cfg.icon + L",0");
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
    if (clsid != CLSID_MotivaMenu) {
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

// MvView First Plot
// Standalone C++/Win32 tray resident Explorer media preview.
// Runtime dependency: mpv-2.dll next to MvView.exe or in PATH.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shldisp.h>
#include <commctrl.h>
#include <objbase.h>
#include <strsafe.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")

#include "resource.h"

#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT (NIN_SELECT | 0x0001)
#endif

namespace mvview {

constexpr wchar_t kAppName[] = L"MvView";
constexpr wchar_t kAppVersion[] = L"0.20";
constexpr wchar_t kAppDisplayName[] = L"MvView v0.20";
constexpr wchar_t kMainWindowClass[] = L"MvView.MainWindow";
constexpr wchar_t kPreviewWindowClass[] = L"MvView.PreviewWindow";
constexpr wchar_t kMpvHostClass[] = L"MvView.MpvHost";
constexpr wchar_t kSplashWindowClass[] = L"MvView.SplashWindow";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MvView.SingleInstance";
constexpr UINT WM_TRAYICON = WM_APP + 10;
constexpr UINT WM_MV_HOOK_EVENT = WM_APP + 20;
constexpr UINT WM_MV_OPEN_PATH = WM_APP + 21;
constexpr UINT WM_MPV_WAKEUP = WM_APP + 22;
constexpr UINT WM_MV_SHOW_STATUS = WM_APP + 23;
constexpr UINT WM_MV_MOUSE_EVENT = WM_APP + 24;
constexpr UINT TIMER_DEBOUNCE = 1001;
constexpr UINT TIMER_CURSOR_CLOSE = 1002;
constexpr UINT HOTKEY_ESCAPE = 4101;
constexpr int TRAY_UID = 1;

constexpr int CMD_TRAY_ENABLED = 2001;
constexpr int CMD_TRAY_CLOSE_PREVIEW = 2002;
constexpr int CMD_TRAY_OPEN_SETTINGS = 2003;
constexpr int CMD_TRAY_ABOUT = 2004;
constexpr int CMD_TRAY_OPEN_MPV_RUNTIME = 2005;
constexpr int CMD_TRAY_HELP = 2007;
constexpr int CMD_TRAY_OPEN_LOGS = 2006;
constexpr int CMD_TRAY_EXIT = 2099;

constexpr LONG EVENT_OBJECT_FOCUS_VALUE = 0x8005;
constexpr LONG EVENT_OBJECT_SELECTION_VALUE = 0x8006;
constexpr LONG EVENT_OBJECT_SELECTIONADD_VALUE = 0x8007;
constexpr LONG EVENT_OBJECT_SELECTIONREMOVE_VALUE = 0x8008;
constexpr LONG EVENT_OBJECT_SELECTIONWITHIN_VALUE = 0x8009;

// Minimal libmpv declarations loaded dynamically. This keeps the project buildable
// without mpv headers/libs; only mpv-2.dll is required at runtime.
struct mpv_handle;
using mpv_event_id = int;
enum mpv_format {
    MPV_FORMAT_NONE = 0,
    MPV_FORMAT_STRING = 1,
    MPV_FORMAT_OSD_STRING = 2,
    MPV_FORMAT_FLAG = 3,
    MPV_FORMAT_INT64 = 4,
    MPV_FORMAT_DOUBLE = 5,
};
struct mpv_event {
    mpv_event_id event_id;
    int error;
    unsigned long long reply_userdata;
    void* data;
};
constexpr mpv_event_id MPV_EVENT_NONE = 0;
constexpr mpv_event_id MPV_EVENT_SHUTDOWN = 1;
constexpr mpv_event_id MPV_EVENT_END_FILE = 7;

using mpv_create_fn = mpv_handle*(__cdecl*)();
using mpv_initialize_fn = int(__cdecl*)(mpv_handle*);
using mpv_terminate_destroy_fn = void(__cdecl*)(mpv_handle*);
using mpv_set_option_string_fn = int(__cdecl*)(mpv_handle*, const char*, const char*);
using mpv_set_property_string_fn = int(__cdecl*)(mpv_handle*, const char*, const char*);
using mpv_get_property_fn = int(__cdecl*)(mpv_handle*, const char*, mpv_format, void*);
using mpv_command_fn = int(__cdecl*)(mpv_handle*, const char**);
using mpv_wait_event_fn = mpv_event*(__cdecl*)(mpv_handle*, double);
using mpv_set_wakeup_callback_fn = void(__cdecl*)(mpv_handle*, void(__cdecl*)(void*), void*);
using mpv_error_string_fn = const char*(__cdecl*)(int);

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return (wchar_t)towlower(ch); });
    return value;
}

std::wstring Trim(std::wstring value) {
    auto not_space = [](wchar_t c) { return !iswspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out((size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out((size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), out.data(), needed);
    return out;
}

std::wstring GetKnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) {
        result = p;
        CoTaskMemFree(p);
    }
    return result;
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (PathFileExistsW(dir.c_str())) return true;
    std::wstring parent = dir;
    PathRemoveFileSpecW(parent.data());
    if (!parent.empty() && parent != dir && !PathFileExistsW(parent.c_str())) {
        EnsureDir(parent);
    }
    return SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) == ERROR_SUCCESS || PathFileExistsW(dir.c_str());
}

std::wstring AppDataDir() {
    std::wstring base = GetKnownFolder(FOLDERID_RoamingAppData);
    if (base.empty()) base = L".";
    std::wstring dir = base + L"\\MvView";
    EnsureDir(dir);
    return dir;
}

std::wstring SettingsPath() { return AppDataDir() + L"\\settings.json"; }
std::wstring LogsDir() { auto dir = AppDataDir() + L"\\logs"; EnsureDir(dir); return dir; }
std::wstring LogPath() { return LogsDir() + L"\\MvView.log"; }

std::wstring NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64]{};
    StringCchPrintfW(buf, 64, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void Log(const std::wstring& text) {
    std::wstring logPath = LogPath();
    std::wofstream fs(logPath.c_str(), std::ios::app);
    if (fs) fs << L"[" << NowText() << L"] " << text << L"\n";
}

std::wstring FileNameOf(const std::wstring& path) {
    const wchar_t* name = PathFindFileNameW(path.c_str());
    return name ? std::wstring(name) : path;
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring WindowTextOf(HWND hwnd) {
    if (!hwnd) return {};
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(wcslen(text.c_str()));
    return text;
}

bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (haystack.empty() || needle.empty()) return false;
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

std::wstring GetModulePathText() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    while (len >= path.size() - 1) {
        path.resize(path.size() * 2, L'\0');
        len = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    }
    path.resize(wcslen(path.c_str()));
    return path;
}

std::wstring ExeDir() {
    std::wstring path = GetModulePathText();
    if (!path.empty()) PathRemoveFileSpecW(path.data());
    return path.c_str();
}

std::wstring RuntimeDir() {
    std::wstring dir = ExeDir() + L"\\runtime";
    EnsureDir(dir);
    return dir;
}

std::wstring DirOfFile(const std::wstring& path) {
    std::wstring dir = path;
    if (!dir.empty()) PathRemoveFileSpecW(dir.data());
    return dir.c_str();
}

std::wstring FormatWin32Error(DWORD err) {
    if (err == 0) return L"0";
    wchar_t* msg = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring text = L"error=" + std::to_wstring(err);
    if (len && msg) {
        std::wstring m = Trim(msg);
        text += L" (" + m + L")";
    }
    if (msg) LocalFree(msg);
    return text;
}


void TryEnableDarkModeForWindow(HWND hwnd) {
    // Best-effort dark mode. This keeps the app dependency-free and does not fail on older Windows.
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (ux) {
        using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
        using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
        auto allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
        auto setWindowTheme = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(ux, "SetWindowTheme"));
        if (allowDarkModeForWindow) allowDarkModeForWindow(hwnd, TRUE);
        if (setWindowTheme) setWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
        FreeLibrary(ux);
    }

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto setAttr = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (setAttr) {
            BOOL dark = TRUE;
            setAttr(hwnd, 20, &dark, sizeof(dark));
            setAttr(hwnd, 19, &dark, sizeof(dark));
        }
        FreeLibrary(dwm);
    }
}

void TryEnableAppDarkMode() {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    if (setPreferredAppMode) setPreferredAppMode(2); // AllowDark
    FreeLibrary(ux);
}

std::vector<std::wstring> MpvCandidatePaths() {
    std::wstring exe = ExeDir();
    std::wstring runtime = RuntimeDir();
    return {
        exe + L"\\mpv-2.dll",
        exe + L"\\libmpv-2.dll",
        runtime + L"\\mpv-2.dll",
        runtime + L"\\libmpv-2.dll"
    };
}

std::wstring JoinLines(const std::vector<std::wstring>& lines) {
    std::wstring out;
    for (const auto& line : lines) {
        if (!out.empty()) out += L"\n";
        out += line;
    }
    return out;
}

std::wstring ExtOf(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    return ToLower(ext ? std::wstring(ext) : L"");
}

enum class MediaKind { None, Image, Video, Audio };

MediaKind DetectMediaKind(const std::wstring& path) {
    const std::wstring ext = ExtOf(path);
    static const std::vector<std::wstring> images = { L".jpg", L".jpeg", L".png", L".webp", L".bmp", L".gif", L".tif", L".tiff" };
    static const std::vector<std::wstring> videos = { L".mp4", L".mkv", L".mov", L".avi", L".webm", L".m4v", L".wmv", L".flv", L".ts", L".mts", L".m2ts", L".mpg", L".mpeg" };
    static const std::vector<std::wstring> audios = { L".mp3", L".wav", L".flac", L".m4a", L".aac", L".ogg", L".opus", L".wma", L".aiff" };
    if (std::find(images.begin(), images.end(), ext) != images.end()) return MediaKind::Image;
    if (std::find(videos.begin(), videos.end(), ext) != videos.end()) return MediaKind::Video;
    if (std::find(audios.begin(), audios.end(), ext) != audios.end()) return MediaKind::Audio;
    return MediaKind::None;
}

std::wstring MediaKindText(MediaKind kind) {
    switch (kind) {
    case MediaKind::Image: return L"image";
    case MediaKind::Video: return L"video";
    case MediaKind::Audio: return L"audio";
    default: return L"none";
    }
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
        case L'\\': out += L"\\\\"; break;
        case L'\"': out += L"\\\""; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

struct Settings {
    bool enabled = true;
    int previewDelayMs = 1200;
    int previewResolutionP = 360;
    int cursorFarClosePx = 220;
    bool closeWhenForegroundLost = true;
    bool closeWhenSelectionEmpty = true;
    bool closeOnNonMedia = true;
    bool audioOnStart = false;
    int volumePercent = 50;
    int borderColorIndex = 0;
    bool registerEscapeWhilePreviewVisible = true;

    static int ReadInt(const std::wstring& body, const std::wstring& key, int def) {
        std::wstring needle = L"\"" + key + L"\"";
        size_t p = body.find(needle);
        if (p == std::wstring::npos) return def;
        p = body.find(L':', p);
        if (p == std::wstring::npos) return def;
        p++;
        while (p < body.size() && iswspace(body[p])) p++;
        bool neg = false;
        if (p < body.size() && body[p] == L'-') { neg = true; p++; }
        int value = 0;
        bool any = false;
        while (p < body.size() && iswdigit(body[p])) { any = true; value = value * 10 + (body[p++] - L'0'); }
        return any ? (neg ? -value : value) : def;
    }

    static bool ReadBool(const std::wstring& body, const std::wstring& key, bool def) {
        std::wstring needle = L"\"" + key + L"\"";
        size_t p = body.find(needle);
        if (p == std::wstring::npos) return def;
        p = body.find(L':', p);
        if (p == std::wstring::npos) return def;
        std::wstring tail = ToLower(body.substr(p + 1, 16));
        if (tail.find(L"true") != std::wstring::npos) return true;
        if (tail.find(L"false") != std::wstring::npos) return false;
        return def;
    }

    void Clamp() {
        previewDelayMs = std::clamp(previewDelayMs, 0, 5000);
        const int allowed[] = { 720, 480, 360, 240, 180 };
        bool ok = false;
        for (int v : allowed) if (previewResolutionP == v) ok = true;
        if (!ok) previewResolutionP = 360;
        cursorFarClosePx = std::clamp(cursorFarClosePx, 80, 1400);
        volumePercent = std::clamp(volumePercent, 0, 100);
        borderColorIndex = std::clamp(borderColorIndex, 0, 6);
    }

    int PreviewContentWidth() const {
        return std::max<int>(320, (previewResolutionP * 16) / 9);
    }

    int PreviewContentHeight() const {
        return std::max<int>(180, previewResolutionP);
    }

    COLORREF BorderColor() const {
        switch (borderColorIndex) {
        case 1: return RGB(96, 238, 255);   // aqua
        case 2: return RGB(126, 255, 126);  // green
        case 3: return RGB(255, 170, 72);   // orange
        case 4: return RGB(255, 128, 196);  // pink
        case 5: return RGB(235, 235, 235);  // white
        case 6: return RGB(160, 140, 255);  // purple
        default: return RGB(255, 232, 92);  // yellow
        }
    }

    void Load() {
        std::wstring settingsPath = SettingsPath();
        std::wifstream fs(settingsPath.c_str());
        if (!fs) { Save(); return; }
        std::wstringstream ss;
        ss << fs.rdbuf();
        std::wstring body = ss.str();
        enabled = ReadBool(body, L"enabled", enabled);
        previewDelayMs = ReadInt(body, L"previewDelayMs", previewDelayMs);
        previewResolutionP = ReadInt(body, L"previewResolutionP", previewResolutionP);
        if (previewResolutionP == 360) {
            int oldHeight = ReadInt(body, L"maxPreviewHeight", 0);
            if (oldHeight == 720 || oldHeight == 480 || oldHeight == 360 || oldHeight == 240 || oldHeight == 180) previewResolutionP = oldHeight;
        }
        cursorFarClosePx = ReadInt(body, L"cursorFarClosePx", cursorFarClosePx);
        closeWhenForegroundLost = ReadBool(body, L"closeWhenForegroundLost", closeWhenForegroundLost);
        closeWhenSelectionEmpty = ReadBool(body, L"closeWhenSelectionEmpty", closeWhenSelectionEmpty);
        closeOnNonMedia = ReadBool(body, L"closeOnNonMedia", closeOnNonMedia);
        audioOnStart = ReadBool(body, L"audioOnStart", audioOnStart);
        volumePercent = ReadInt(body, L"volumePercent", volumePercent);
        borderColorIndex = ReadInt(body, L"borderColorIndex", borderColorIndex);
        registerEscapeWhilePreviewVisible = ReadBool(body, L"registerEscapeWhilePreviewVisible", registerEscapeWhilePreviewVisible);
        Clamp();
    }

    void Save() const {
        EnsureDir(AppDataDir());
        std::wstring settingsPath = SettingsPath();
        std::wofstream fs(settingsPath.c_str(), std::ios::trunc);
        if (!fs) return;
        fs << L"{\n";
        fs << L"  \"enabled\": " << (enabled ? L"true" : L"false") << L",\n";
        fs << L"  \"previewDelayMs\": " << previewDelayMs << L",\n";
        fs << L"  \"previewResolutionP\": " << previewResolutionP << L",\n";
        fs << L"  \"cursorFarClosePx\": " << cursorFarClosePx << L",\n";
        fs << L"  \"closeWhenForegroundLost\": " << (closeWhenForegroundLost ? L"true" : L"false") << L",\n";
        fs << L"  \"closeWhenSelectionEmpty\": " << (closeWhenSelectionEmpty ? L"true" : L"false") << L",\n";
        fs << L"  \"closeOnNonMedia\": " << (closeOnNonMedia ? L"true" : L"false") << L",\n";
        fs << L"  \"audioOnStart\": " << (audioOnStart ? L"true" : L"false") << L",\n";
        fs << L"  \"volumePercent\": " << volumePercent << L",\n";
        fs << L"  \"borderColorIndex\": " << borderColorIndex << L",\n";
        fs << L"  \"registerEscapeWhilePreviewVisible\": " << (registerEscapeWhilePreviewVisible ? L"true" : L"false") << L"\n";
        fs << L"}\n";
    }
};
template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** operator&() { reset(); return &p; }
    T* operator->() const { return p; }
    operator bool() const { return p != nullptr; }
    T* get() const { return p; }
    void reset(T* v = nullptr) { if (p) p->Release(); p = v; }
};

class ExplorerReader {
public:
    bool IsExplorerWindow(HWND hwnd) const {
        HWND root = GetAncestor(hwnd, GA_ROOT);
        wchar_t cls[256]{};
        GetClassNameW(root ? root : hwnd, cls, 256);
        std::wstring c = cls;
        return c == L"CabinetWClass" || c == L"ExploreWClass";
    }

    std::vector<std::wstring> GetSelectedPathsForForeground(HWND foreground, std::wstring* focusedPath = nullptr) {
        std::vector<std::wstring> result;
        if (focusedPath) focusedPath->clear();
        if (!foreground || !IsExplorerWindow(foreground)) return result;
        HWND root = GetAncestor(foreground, GA_ROOT);
        const std::wstring activeTitle = WindowTextOf(root);
        Log(L"ExplorerReader: foreground title=" + activeTitle);

        struct Candidate {
            long index = -1;
            HWND hwnd = nullptr;
            std::wstring locationName;
            std::wstring locationUrl;
            std::wstring focused;
            std::vector<std::wstring> selected;
            int score = 0;
        };
        std::vector<Candidate> candidates;

        ComPtr<IShellWindows> shellWindows;
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows.p));
        if (FAILED(hr) || !shellWindows) {
            Log(L"ExplorerReader: CoCreateInstance(IShellWindows) failed");
            return result;
        }

        long count = 0;
        shellWindows->get_Count(&count);
        for (long i = 0; i < count; ++i) {
            VARIANT index{};
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            ComPtr<IDispatch> disp;
            if (FAILED(shellWindows->Item(index, &disp.p)) || !disp) continue;

            ComPtr<IWebBrowserApp> web;
            if (FAILED(disp->QueryInterface(IID_PPV_ARGS(&web.p))) || !web) continue;
            SHANDLE_PTR webHwnd = 0;
            if (FAILED(web->get_HWND(&webHwnd))) continue;
            HWND webRoot = GetAncestor((HWND)webHwnd, GA_ROOT);
            if (webRoot != root && (HWND)webHwnd != root && (HWND)webHwnd != foreground) continue;

            Candidate cand;
            cand.index = i;
            cand.hwnd = (HWND)webHwnd;

            BSTR bLocationName = nullptr;
            if (SUCCEEDED(web->get_LocationName(&bLocationName)) && bLocationName) {
                cand.locationName = Trim(bLocationName);
                SysFreeString(bLocationName);
            }
            BSTR bLocationUrl = nullptr;
            if (SUCCEEDED(web->get_LocationURL(&bLocationUrl)) && bLocationUrl) {
                cand.locationUrl = Trim(bLocationUrl);
                SysFreeString(bLocationUrl);
            }

            ComPtr<IDispatch> doc;
            if (FAILED(web->get_Document(&doc.p)) || !doc) continue;
            ComPtr<IShellFolderViewDual> view;
            if (FAILED(doc->QueryInterface(IID_PPV_ARGS(&view.p))) || !view) continue;

            // Keep FocusedItem only as diagnostic information.  Windows 11 Explorer
            // tabs can keep FocusedItem on an old tab, so it must not decide the
            // preview target.
            ComPtr<FolderItem> focusedItem;
            if (SUCCEEDED(view->get_FocusedItem(&focusedItem.p)) && focusedItem) {
                BSTR fpath = nullptr;
                if (SUCCEEDED(focusedItem->get_Path(&fpath)) && fpath) {
                    cand.focused = Trim(fpath);
                    SysFreeString(fpath);
                }
            }

            ComPtr<FolderItems> items;
            if (FAILED(view->SelectedItems(&items.p)) || !items) continue;
            long selectedCount = 0;
            items->get_Count(&selectedCount);
            for (long j = 0; j < selectedCount; ++j) {
                VARIANT itemIndex{};
                VariantInit(&itemIndex);
                itemIndex.vt = VT_I4;
                itemIndex.lVal = j;
                ComPtr<FolderItem> item;
                if (FAILED(items->Item(itemIndex, &item.p)) || !item) continue;
                BSTR bpath = nullptr;
                if (SUCCEEDED(item->get_Path(&bpath)) && bpath) {
                    std::wstring path = Trim(bpath);
                    SysFreeString(bpath);
                    if (!path.empty()) cand.selected.push_back(path);
                }
            }

            if (!cand.selected.empty()) cand.score += 100;
            if (!cand.locationName.empty() && ContainsInsensitive(activeTitle, cand.locationName)) cand.score += 1000;
            if (!cand.locationUrl.empty() && ContainsInsensitive(activeTitle, cand.locationUrl)) cand.score += 300;
            // Prefer candidates with more selected items when the active tab title is
            // not enough to distinguish entries.
            cand.score += static_cast<int>(std::min<size_t>(cand.selected.size(), 20));

            Log(L"ExplorerReader: candidate[" + std::to_wstring(cand.index) +
                L"] locationName=" + cand.locationName +
                L" selected=" + std::to_wstring(cand.selected.size()) +
                L" score=" + std::to_wstring(cand.score) +
                (cand.focused.empty() ? L"" : (L" focused=" + cand.focused)));
            candidates.push_back(std::move(cand));
        }

        const Candidate* best = nullptr;
        for (const auto& cand : candidates) {
            if (cand.selected.empty()) continue;
            if (!best || cand.score > best->score) best = &cand;
        }
        if (!best) {
            Log(L"ExplorerReader: selected items returned=0");
            return result;
        }

        result = best->selected;
        if (focusedPath) *focusedPath = best->focused;
        Log(L"ExplorerReader: active candidate index=" + std::to_wstring(best->index) +
            L" locationName=" + best->locationName +
            L" selected items returned=" + std::to_wstring(result.size()));
        return result;
    }
};

class MpvApi {
public:
    HMODULE dll = nullptr;
    mpv_create_fn create = nullptr;
    mpv_initialize_fn initialize = nullptr;
    mpv_terminate_destroy_fn terminate_destroy = nullptr;
    mpv_set_option_string_fn set_option_string = nullptr;
    mpv_set_property_string_fn set_property_string = nullptr;
    mpv_command_fn command = nullptr;
    mpv_get_property_fn get_property = nullptr;
    mpv_wait_event_fn wait_event = nullptr;
    mpv_set_wakeup_callback_fn set_wakeup_callback = nullptr;
    mpv_error_string_fn error_string = nullptr;
    std::wstring loadedPath;
    std::wstring lastErrorText;

    bool Load() {
        if (dll) return true;
        lastErrorText.clear();
        loadedPath.clear();

        Log(L"MpvApi: begin mpv runtime search");
        auto candidates = MpvCandidatePaths();
        for (const auto& candidate : candidates) {
            Log(L"MpvApi: candidate " + candidate + (FileExists(candidate) ? L" [exists]" : L" [missing]"));
            if (!FileExists(candidate)) continue;
            if (TryLoad(candidate)) break;
        }

        if (!dll) {
            Log(L"MpvApi: trying PATH lookup: mpv-2.dll");
            dll = LoadLibraryW(L"mpv-2.dll");
            if (dll) loadedPath = L"PATH:mpv-2.dll";
            else Log(L"MpvApi: PATH lookup mpv-2.dll failed: " + FormatWin32Error(GetLastError()));
        }
        if (!dll) {
            Log(L"MpvApi: trying PATH lookup: libmpv-2.dll");
            dll = LoadLibraryW(L"libmpv-2.dll");
            if (dll) loadedPath = L"PATH:libmpv-2.dll";
            else Log(L"MpvApi: PATH lookup libmpv-2.dll failed: " + FormatWin32Error(GetLastError()));
        }

        if (!dll) {
            lastErrorText = L"mpv-2.dll / libmpv-2.dll could not be loaded. Check EXE folder, runtime folder, and dependent DLLs.";
            Log(L"MpvApi: runtime load failed. " + lastErrorText);
            return false;
        }

        create = (mpv_create_fn)GetProcAddress(dll, "mpv_create");
        initialize = (mpv_initialize_fn)GetProcAddress(dll, "mpv_initialize");
        terminate_destroy = (mpv_terminate_destroy_fn)GetProcAddress(dll, "mpv_terminate_destroy");
        set_option_string = (mpv_set_option_string_fn)GetProcAddress(dll, "mpv_set_option_string");
        set_property_string = (mpv_set_property_string_fn)GetProcAddress(dll, "mpv_set_property_string");
        command = (mpv_command_fn)GetProcAddress(dll, "mpv_command");
        get_property = (mpv_get_property_fn)GetProcAddress(dll, "mpv_get_property");
        wait_event = (mpv_wait_event_fn)GetProcAddress(dll, "mpv_wait_event");
        set_wakeup_callback = (mpv_set_wakeup_callback_fn)GetProcAddress(dll, "mpv_set_wakeup_callback");
        error_string = (mpv_error_string_fn)GetProcAddress(dll, "mpv_error_string");

        bool ok = create && initialize && terminate_destroy && set_option_string && set_property_string && command && get_property;
        if (!ok) {
            lastErrorText = L"mpv DLL loaded but required C API symbols were missing: " + loadedPath;
            Log(L"MpvApi: " + lastErrorText);
            Unload();
            return false;
        }
        Log(L"MpvApi: runtime loaded: " + loadedPath);
        return true;
    }

    bool TryLoad(const std::wstring& path) {
        std::wstring dir = DirOfFile(path);
        if (!dir.empty()) SetDllDirectoryW(dir.c_str());
        HMODULE h = LoadLibraryW(path.c_str());
        DWORD err = h ? 0 : GetLastError();
        SetDllDirectoryW(nullptr);
        if (!h) {
            Log(L"MpvApi: LoadLibrary failed: " + path + L" / " + FormatWin32Error(err));
            return false;
        }
        dll = h;
        loadedPath = path;
        return true;
    }

    void Unload() {
        if (dll) FreeLibrary(dll);
        dll = nullptr;
        loadedPath.clear();
    }
};

bool ProbeMpvRuntime(std::wstring* statusText = nullptr) {
    Log(L"ProbeMpvRuntime: exe=" + GetModulePathText());
    Log(L"ProbeMpvRuntime: exeDir=" + ExeDir());
    Log(L"ProbeMpvRuntime: runtimeDir=" + RuntimeDir());
    for (const auto& candidate : MpvCandidatePaths()) {
        Log(L"ProbeMpvRuntime: search " + candidate + (FileExists(candidate) ? L" [exists]" : L" [missing]"));
    }
    MpvApi api;
    bool ok = api.Load();
    if (statusText) {
        *statusText = ok ? (L"mpv ok: " + api.loadedPath) : (L"mpv missing: " + api.lastErrorText);
    }
    api.Unload();
    return ok;
}
class MpvPlayer {
    MpvApi api_;
    mpv_handle* handle_ = nullptr;
    HWND owner_ = nullptr;
    HWND host_ = nullptr;
    bool initialized_ = false;

    static void __cdecl Wakeup(void* ctx) {
        auto self = reinterpret_cast<MpvPlayer*>(ctx);
        if (self && self->owner_) PostMessageW(self->owner_, WM_MPV_WAKEUP, 0, 0);
    }

public:
    ~MpvPlayer() { Shutdown(); }

    bool Initialize(HWND owner, HWND host) {
        owner_ = owner;
        host_ = host;
        if (initialized_) return true;
        if (!api_.Load()) {
            Log(L"MpvPlayer: mpv runtime not available");
            return false;
        }
        handle_ = api_.create();
        if (!handle_) return false;

        std::wstring widText = std::to_wstring(reinterpret_cast<uintptr_t>(host_));
        std::string widUtf8 = WideToUtf8(widText);
        api_.set_option_string(handle_, "wid", widUtf8.c_str());
        api_.set_option_string(handle_, "terminal", "no");
        api_.set_option_string(handle_, "msg-level", "all=no");
        api_.set_option_string(handle_, "force-window", "immediate");
        api_.set_option_string(handle_, "keep-open", "yes");
        api_.set_option_string(handle_, "loop-file", "no");
        api_.set_option_string(handle_, "osc", "no");
        api_.set_option_string(handle_, "input-default-bindings", "no");
        api_.set_option_string(handle_, "input-vo-keyboard", "no");
        api_.set_option_string(handle_, "cursor-autohide", "always");
        api_.set_option_string(handle_, "hwdec", "auto-safe");
        if (api_.set_wakeup_callback) api_.set_wakeup_callback(handle_, Wakeup, this);
        int rc = api_.initialize(handle_);
        if (rc < 0) {
            Log(L"MpvPlayer: mpv_initialize failed");
            api_.terminate_destroy(handle_);
            handle_ = nullptr;
            return false;
        }
        initialized_ = true;
        return true;
    }

    void Shutdown() {
        if (handle_ && api_.terminate_destroy) {
            api_.terminate_destroy(handle_);
        }
        handle_ = nullptr;
        initialized_ = false;
        api_.Unload();
    }

    void DrainEvents() {
        if (!handle_ || !api_.wait_event) return;
        while (true) {
            mpv_event* ev = api_.wait_event(handle_, 0.0);
            if (!ev || ev->event_id == MPV_EVENT_NONE) break;
            if (ev->event_id == MPV_EVENT_SHUTDOWN) break;
        }
    }

    bool LoadFile(const std::wstring& path, bool allowAudio, int volumePercent, bool startPaused = false) {
        if (!handle_) return false;
        std::string pathUtf8 = WideToUtf8(path);
        SetVolume(volumePercent);
        api_.set_property_string(handle_, "mute", allowAudio ? "no" : "yes");
        // For multi-preview, load every file paused first and release them together
        // after all preview windows have been created. This avoids the last mpv
        // instance visibly starting while earlier ones are still initializing.
        api_.set_property_string(handle_, "pause", startPaused ? "yes" : "no");
        const char* cmd[] = { "loadfile", pathUtf8.c_str(), "replace", nullptr };
        int rc = api_.command(handle_, cmd);
        if (rc < 0) {
            Log(L"MpvPlayer: loadfile failed: " + path);
            return false;
        }
        if (!startPaused) api_.set_property_string(handle_, "pause", "no");
        return true;
    }

    void SetPause(bool paused) {
        if (!handle_) return;
        api_.set_property_string(handle_, "pause", paused ? "yes" : "no");
    }

    void Stop() {
        if (!handle_) return;
        const char* cmd[] = { "stop", nullptr };
        api_.command(handle_, cmd);
    }

    void TogglePause() {
        if (!handle_) return;
        const char* cmd[] = { "cycle", "pause", nullptr };
        api_.command(handle_, cmd);
    }

    void SeekRelative(double seconds) {
        if (!handle_) return;
        std::string sec = std::to_string(seconds);
        const char* cmd[] = { "seek", sec.c_str(), "relative", "exact", nullptr };
        api_.command(handle_, cmd);
    }

    void SetVolume(int volumePercent) {
        if (!handle_) return;
        std::string volume = std::to_string(std::clamp<int>(volumePercent, 0, 100));
        api_.set_property_string(handle_, "volume", volume.c_str());
    }

    bool GetDouble(const char* prop, double* out) {
        if (!handle_ || !api_.get_property || !out) return false;
        double value = 0.0;
        int rc = api_.get_property(handle_, prop, MPV_FORMAT_DOUBLE, &value);
        if (rc < 0) return false;
        *out = value;
        return true;
    }

    bool GetProgress(double* pos, double* dur) {
        bool any = false;
        double p = 0.0, d = 0.0;
        if (GetDouble("time-pos", &p)) any = true;
        if (GetDouble("duration", &d)) any = true;
        if (pos) *pos = p;
        if (dur) *dur = d;
        return any;
    }
};

std::wstring FormatTimeSeconds(double seconds) {
    if (!(seconds >= 0.0) || seconds > 86400.0 * 10) return L"--:--";
    long long total = (long long)(seconds + 0.5);
    long long h = total / 3600;
    long long m = (total % 3600) / 60;
    long long sec = total % 60;
    wchar_t buf[32]{};
    if (h > 0) StringCchPrintfW(buf, 32, L"%lld:%02lld:%02lld", h, m, sec);
    else StringCchPrintfW(buf, 32, L"%02lld:%02lld", m, sec);
    return buf;
}

bool PointInRect(const RECT& rc, POINT pt) {
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

class PreviewWindow {
    HWND hwnd_ = nullptr;
    HWND host_ = nullptr;
    MpvPlayer player_;
    Settings* settings_ = nullptr;
    std::wstring currentPath_;
    MediaKind currentKind_ = MediaKind::None;
    POINT anchor_{};
    bool escapeRegistered_ = false;
    bool volumePopupVisible_ = false;
    bool draggingVolume_ = false;
    int temporaryResolutionP_ = 0;
    double timePos_ = 0.0;
    double duration_ = 0.0;
    RECT progressRect_{};
    RECT volumeIconRect_{};
    RECT volumeSliderRect_{};
    RECT hostRect_{};

public:
    HWND Hwnd() const { return hwnd_; }
    const std::wstring& CurrentPath() const { return currentPath_; }
    bool IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    POINT Anchor() const { return anchor_; }
    bool ContainsScreenPoint(POINT pt) const {
        if (!IsVisible()) return false;
        RECT wr{};
        GetWindowRect(hwnd_, &wr);
        return PointInRect(wr, pt);
    }

    RECT WindowRect() const {
        RECT wr{};
        if (IsVisible()) GetWindowRect(hwnd_, &wr);
        return wr;
    }

    bool IsAtEnd() const {
        return IsVisible() && duration_ > 0.5 && timePos_ >= (duration_ - 0.25);
    }

    bool Create(HINSTANCE inst, Settings* settings) {
        settings_ = settings;
        WNDCLASSW hostCls{};
        hostCls.lpfnWndProc = PreviewWindow::HostWndProc;
        hostCls.hInstance = inst;
        hostCls.lpszClassName = kMpvHostClass;
        hostCls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hostCls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassW(&hostCls);

        WNDCLASSW cls{};
        cls.lpfnWndProc = PreviewWindow::WndProc;
        cls.hInstance = inst;
        cls.lpszClassName = kPreviewWindowClass;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        cls.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
        RegisterClassW(&cls);

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kPreviewWindowClass,
            kAppName,
            WS_POPUP | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 660, 430,
            nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        TryEnableDarkModeForWindow(hwnd_);

        host_ = CreateWindowExW(0, kMpvHostClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            10, 38, 640, 360, hwnd_, nullptr, inst, nullptr);
        if (!host_) return false;
        return true;
    }

    void Destroy() {
        Hide();
        player_.Shutdown();
        if (hwnd_) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        host_ = nullptr;
    }

    int ActiveResolutionP() const {
        return temporaryResolutionP_ > 0 ? temporaryResolutionP_ : (settings_ ? settings_->previewResolutionP : 360);
    }

    int ContentWidth() const {
        int p = ActiveResolutionP();
        return std::max<int>(320, (p * 16) / 9);
    }

    int ContentHeight() const {
        return std::max<int>(180, ActiveResolutionP());
    }


    int WindowWidth() const { return ContentWidth() + 20; }
    int WindowHeight() const { return ContentHeight() + 10 + 28 + 34 + 10; }

    void Layout(int width, int height) {
        const int margin = 10;
        const int titleH = 28;
        const int controlH = 34;
        hostRect_ = { margin, margin + titleH, width - margin, height - margin - controlH };
        const int hostW = std::max<int>(1, static_cast<int>(hostRect_.right - hostRect_.left));
        const int hostH = std::max<int>(1, static_cast<int>(hostRect_.bottom - hostRect_.top));
        ::MoveWindow(host_, static_cast<int>(hostRect_.left), static_cast<int>(hostRect_.top), hostW, hostH, TRUE);

        const int bottomY = height - margin - controlH + 6;
        progressRect_ = { 72, bottomY + 7, std::max<int>(80, width - 154), bottomY + 17 };
        volumeIconRect_ = { width - 48, bottomY - 3, width - 18, bottomY + 27 };
        volumeSliderRect_ = { volumeIconRect_.left + 6, volumeIconRect_.top - 124, volumeIconRect_.right - 6, volumeIconRect_.top - 4 };
    }

    RECT ClampRectToMonitor(RECT wanted) {
        HMONITOR mon = MonitorFromPoint(anchor_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        RECT work = mi.rcWork;
        const int w = wanted.right - wanted.left;
        const int h = wanted.bottom - wanted.top;
        if (wanted.right > work.right) { wanted.left = work.right - w - 8; wanted.right = wanted.left + w; }
        if (wanted.bottom > work.bottom) { wanted.top = work.bottom - h - 8; wanted.bottom = wanted.top + h; }
        if (wanted.left < work.left) { wanted.left = work.left + 8; wanted.right = wanted.left + w; }
        if (wanted.top < work.top) { wanted.top = work.top + 8; wanted.bottom = wanted.top + h; }
        return wanted;
    }

    bool ShowPath(const std::wstring& path, MediaKind kind, POINT anchor) {
        int width = WindowWidth();
        int height = WindowHeight();
        RECT wanted{ anchor.x + 18, anchor.y + 18, anchor.x + 18 + width, anchor.y + 18 + height };
        wanted = ClampRectToMonitor(wanted);
        POINT topLeft{ wanted.left, wanted.top };
        return ShowPathAt(path, kind, topLeft, anchor);
    }

    bool ShowPathAt(const std::wstring& path, MediaKind kind, POINT topLeft, POINT logicalAnchor, bool startPaused = false) {
        if (!hwnd_ || !host_ || !settings_) return false;
        currentPath_ = path;
        currentKind_ = kind;
        anchor_ = logicalAnchor;
        timePos_ = 0.0;
        duration_ = 0.0;
        volumePopupVisible_ = false;
        draggingVolume_ = false;

        int width = WindowWidth();
        int height = WindowHeight();
        RECT r{ topLeft.x, topLeft.y, topLeft.x + width, topLeft.y + height };
        r = ClampRectToMonitor(r);
        SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Layout(r.right - r.left, r.bottom - r.top);
        InvalidateRect(hwnd_, nullptr, TRUE);

        if (!player_.Initialize(hwnd_, host_)) {
            Log(L"PreviewWindow: player initialization failed; showing diagnostic preview window");
            InvalidateRect(hwnd_, nullptr, TRUE);
            RegisterEscape();
            return false;
        }
        // Always stop the previous file before loading a new path.  With fast
        // Explorer selection changes, mpv can briefly keep the previous frame; this
        // made single preview look like it opened a different file.
        player_.Stop();
        bool allowAudio = settings_->audioOnStart && (kind == MediaKind::Audio || kind == MediaKind::Video);
        const bool loaded = player_.LoadFile(path, allowAudio, settings_->volumePercent, startPaused);
        Log(std::wstring(L"PreviewWindow: load ") + (loaded ? L"ok: " : L"failed: ") + path);
        RegisterEscape();
        return loaded;
    }

    void Hide() {
        UnregisterEscape();
        player_.Stop();
        currentPath_.clear();
        currentKind_ = MediaKind::None;
        volumePopupVisible_ = false;
        draggingVolume_ = false;
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    }

    void RegisterEscape() {
        if (!settings_ || !settings_->registerEscapeWhilePreviewVisible || escapeRegistered_) return;
        if (RegisterHotKey(nullptr, HOTKEY_ESCAPE, MOD_NOREPEAT, VK_ESCAPE)) escapeRegistered_ = true;
    }

    void UnregisterEscape() {
        if (!escapeRegistered_) return;
        UnregisterHotKey(nullptr, HOTKEY_ESCAPE);
        escapeRegistered_ = false;
    }

    void OnMpvWakeup() { player_.DrainEvents(); }

    void Tick() {
        if (!IsVisible()) return;
        player_.DrainEvents();
        player_.GetProgress(&timePos_, &duration_);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        RECT bottom{ 0, rc.bottom - 48, rc.right, rc.bottom };
        InvalidateRect(hwnd_, &bottom, FALSE);
        if (volumePopupVisible_) InvalidateRect(hwnd_, &volumeSliderRect_, FALSE);
    }

    void TogglePause() { player_.TogglePause(); }

    void StartPlayback() { player_.SetPause(false); }

    void SeekRelative(double seconds) { player_.SeekRelative(seconds); }

    void AdjustTemporaryResolution(int wheelDelta) {
        static const int allowed[] = { 180, 240, 360, 480, 720 };
        int current = ActiveResolutionP();
        int idx = 2;
        for (int i = 0; i < 5; ++i) if (allowed[i] == current) idx = i;
        if (wheelDelta > 0 && idx < 4) idx++;
        if (wheelDelta < 0 && idx > 0) idx--;
        temporaryResolutionP_ = allowed[idx];
        if (!IsVisible()) return;
        RECT wr{};
        GetWindowRect(hwnd_, &wr);
        POINT a{ wr.left - 18, wr.top - 18 };
        ShowPath(currentPath_, currentKind_, a);
        Log(L"preview temporary resolution changed: " + std::to_wstring(temporaryResolutionP_) + L"p");
    }

private:
    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        HWND parent = GetParent(hwnd);
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MOUSEMOVE: {
            if (parent) {
                POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                MapWindowPoints(hwnd, parent, &pt, 1);
                SendMessageW(parent, msg, wp, MAKELPARAM(pt.x, pt.y));
                return 0;
            }
            break;
        }
        case WM_MOUSEWHEEL:
            if (parent) { SendMessageW(parent, msg, wp, lp); return 0; }
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        PreviewWindow* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<PreviewWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_SIZE:
            self->Layout(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_MPV_WAKEUP:
            self->OnMpvWakeup();
            return 0;
        case WM_PAINT:
            return self->OnPaint();
        case WM_LBUTTONDOWN:
            return self->OnLeftButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        case WM_LBUTTONUP:
            return self->OnLeftButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        case WM_MOUSEMOVE:
            return self->OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
        case WM_MOUSEWHEEL:
            return self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp), (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    LRESULT OnLeftButtonDown(int x, int y) {
        POINT pt{ x, y };
        RECT hitSlider = volumeSliderRect_;
        InflateRect(&hitSlider, 10, 10);
        RECT hitIcon = volumeIconRect_;
        InflateRect(&hitIcon, 8, 8);
        if (volumePopupVisible_ && PointInRect(hitSlider, pt)) {
            draggingVolume_ = true;
            SetCapture(hwnd_);
            SetVolumeFromPoint(y);
            return 0;
        }
        if (PointInRect(hitIcon, pt)) {
            volumePopupVisible_ = !volumePopupVisible_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        volumePopupVisible_ = false;
        TogglePause();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    LRESULT OnLeftButtonUp(int, int) {
        if (draggingVolume_) {
            draggingVolume_ = false;
            ReleaseCapture();
            settings_->Save();
        }
        return 0;
    }

    LRESULT OnMouseMove(int, int y, WPARAM) {
        if (draggingVolume_) SetVolumeFromPoint(y);
        return 0;
    }

    LRESULT OnMouseWheel(int delta, bool ctrl) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        RECT hitSlider = volumeSliderRect_;
        InflateRect(&hitSlider, 10, 10);
        RECT hitIcon = volumeIconRect_;
        InflateRect(&hitIcon, 8, 8);
        if (volumePopupVisible_ && (PointInRect(hitSlider, pt) || PointInRect(hitIcon, pt))) {
            int step = delta > 0 ? 5 : -5;
            settings_->volumePercent = std::clamp<int>(settings_->volumePercent + step, 0, 100);
            player_.SetVolume(settings_->volumePercent);
            settings_->Save();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (ctrl) {
            AdjustTemporaryResolution(delta);
        } else {
            SeekRelative(delta > 0 ? 5.0 : -5.0);
        }
        return 0;
    }

    void SetVolumeFromPoint(int y) {
        if (!settings_) return;
        int h = std::max<int>(1, static_cast<int>(volumeSliderRect_.bottom - volumeSliderRect_.top));
        int clampedY = std::clamp<int>(y, static_cast<int>(volumeSliderRect_.top), static_cast<int>(volumeSliderRect_.bottom));
        int v = 100 - ((clampedY - volumeSliderRect_.top) * 100 / h);
        settings_->volumePercent = std::clamp<int>(v, 0, 100);
        player_.SetVolume(settings_->volumePercent);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    LRESULT OnPaint() {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 12));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        COLORREF borderColor = settings_ ? settings_->BorderColor() : RGB(255, 232, 92);
        HPEN border = CreatePen(PS_SOLID, 4, borderColor);
        HGDIOBJ oldPen = SelectObject(hdc, border);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, 2, 2, rc.right - 2, rc.bottom - 2, 18, 18);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(border);

        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Meiryo UI");
        HGDIOBJ oldFont = SelectObject(hdc, font);
        SetTextColor(hdc, RGB(255, 250, 210));
        RECT title{ 12, 8, rc.right - 48, 36 };
        std::wstring titleText = currentPath_.empty() ? L"MvView - mpv runtime missing" : (L"MvView  " + MediaKindText(currentKind_) + L"  " + FileNameOf(currentPath_));
        DrawTextW(hdc, titleText.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (currentKind_ == MediaKind::Audio || currentKind_ == MediaKind::Video) {
            RECT note{ rc.right - 40, 8, rc.right - 12, 36 };
            SetTextColor(hdc, settings_ && settings_->audioOnStart ? RGB(255, 250, 210) : RGB(130, 130, 120));
            DrawTextW(hdc, L"♫", -1, &note, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        DrawControls(hdc, rc);

        SelectObject(hdc, oldFont);
        DeleteObject(font);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    void DrawControls(HDC hdc, const RECT& rc) {
        int bottomY = rc.bottom - 38;
        SetTextColor(hdc, RGB(230, 230, 210));
        std::wstring leftTime = FormatTimeSeconds(timePos_);
        std::wstring rightTime = FormatTimeSeconds(duration_);
        RECT left{ 14, bottomY, 68, bottomY + 28 };
        DrawTextW(hdc, leftTime.c_str(), -1, &left, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        HPEN trackPen = CreatePen(PS_SOLID, 8, RGB(76, 76, 58));
        HGDIOBJ oldPen = SelectObject(hdc, trackPen);
        MoveToEx(hdc, progressRect_.left, (progressRect_.top + progressRect_.bottom) / 2, nullptr);
        LineTo(hdc, progressRect_.right, (progressRect_.top + progressRect_.bottom) / 2);
        SelectObject(hdc, oldPen);
        DeleteObject(trackPen);

        double ratio = (duration_ > 0.01) ? std::clamp(timePos_ / duration_, 0.0, 1.0) : 0.0;
        int fillRight = progressRect_.left + (int)((progressRect_.right - progressRect_.left) * ratio);
        HPEN fillPen = CreatePen(PS_SOLID, 8, settings_ ? settings_->BorderColor() : RGB(255, 232, 92));
        oldPen = SelectObject(hdc, fillPen);
        MoveToEx(hdc, progressRect_.left, (progressRect_.top + progressRect_.bottom) / 2, nullptr);
        LineTo(hdc, fillRight, (progressRect_.top + progressRect_.bottom) / 2);
        SelectObject(hdc, oldPen);
        DeleteObject(fillPen);

        RECT right{ progressRect_.right + 8, bottomY, volumeIconRect_.left - 4, bottomY + 28 };
        DrawTextW(hdc, rightTime.c_str(), -1, &right, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT v = volumeIconRect_;
        SetTextColor(hdc, RGB(255, 250, 210));
        DrawTextW(hdc, L"VOL", -1, &v, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (volumePopupVisible_ && settings_) {
            HBRUSH popupBg = CreateSolidBrush(RGB(35, 35, 28));
            FillRect(hdc, &volumeSliderRect_, popupBg);
            DeleteObject(popupBg);
            HPEN p = CreatePen(PS_SOLID, 2, RGB(210, 210, 180));
            oldPen = SelectObject(hdc, p);
            Rectangle(hdc, volumeSliderRect_.left, volumeSliderRect_.top, volumeSliderRect_.right, volumeSliderRect_.bottom);
            int cx = (volumeSliderRect_.left + volumeSliderRect_.right) / 2;
            MoveToEx(hdc, cx, volumeSliderRect_.top + 8, nullptr);
            LineTo(hdc, cx, volumeSliderRect_.bottom - 8);
            int knobY = volumeSliderRect_.bottom - ((volumeSliderRect_.bottom - volumeSliderRect_.top) * settings_->volumePercent / 100);
            HBRUSH knob = CreateSolidBrush(settings_->BorderColor());
            HGDIOBJ oldBrush = SelectObject(hdc, knob);
            Ellipse(hdc, cx - 7, knobY - 7, cx + 7, knobY + 7);
            SelectObject(hdc, oldBrush);
            DeleteObject(knob);
            SelectObject(hdc, oldPen);
            DeleteObject(p);
        }
    }
};

class SettingsDialog {
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HINSTANCE inst_ = nullptr;
    Settings* settings_ = nullptr;
    bool done_ = false;
    bool applied_ = false;
    HBRUSH darkBg_ = nullptr;
    HBRUSH darkEditBg_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT uiFontBold_ = nullptr;

    enum : int {
        IDC_RESOLUTION = 3101,
        IDC_DELAY = 3102,
        IDC_AUDIO = 3103,
        IDC_COLOR = 3104,
        IDC_OK = IDOK,
        IDC_CANCEL = IDCANCEL
    };

public:
    bool Show(HINSTANCE inst, HWND owner, Settings& settings) {
        inst_ = inst;
        owner_ = owner;
        settings_ = &settings;
        done_ = false;
        applied_ = false;

        darkBg_ = CreateSolidBrush(RGB(24, 24, 24));
        darkEditBg_ = CreateSolidBrush(RGB(38, 38, 38));

        WNDCLASSW cls{};
        cls.lpfnWndProc = SettingsDialog::WndProc;
        cls.hInstance = inst_;
        cls.lpszClassName = L"MvView.SettingsDialog";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        cls.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
        RegisterClassW(&cls);

        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            cls.lpszClassName, L"MvView 設定",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, 520, 410,
            owner_, nullptr, inst_, this);
        if (!hwnd_) return false;
        TryEnableDarkModeForWindow(hwnd_);

        CenterToOwner();
        EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        MSG msg{};
        while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
        if (darkBg_) { DeleteObject(darkBg_); darkBg_ = nullptr; }
        if (darkEditBg_) { DeleteObject(darkEditBg_); darkEditBg_ = nullptr; }
        if (uiFont_) { DeleteObject(uiFont_); uiFont_ = nullptr; }
        if (uiFontBold_) { DeleteObject(uiFontBold_); uiFontBold_ = nullptr; }
        return applied_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SettingsDialog* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<SettingsDialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_CREATE:
            self->CreateControls();
            return 0;
        case WM_CTLCOLORDLG:
            return reinterpret_cast<LRESULT>(self->darkBg_ ? self->darkBg_ : (HBRUSH)GetStockObject(BLACK_BRUSH));
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, self->darkBg_ ? self->darkBg_ : (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(245, 245, 245));
            HBRUSH brush = self->darkBg_ ? self->darkBg_ : (HBRUSH)GetStockObject(BLACK_BRUSH);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(38, 38, 38));
            SetTextColor(hdc, RGB(245, 245, 245));
            HBRUSH brush = self->darkEditBg_ ? self->darkEditBg_ : (HBRUSH)GetStockObject(BLACK_BRUSH);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case IDC_OK:
                self->ApplyAndClose();
                return 0;
            case IDC_CANCEL:
                self->Close(false);
                return 0;
            }
            break;
        case WM_CLOSE:
            self->Close(false);
            return 0;
        case WM_DESTROY:
            self->done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void CenterToOwner() {
        RECT rc{}, ow{};
        GetWindowRect(hwnd_, &rc);
        if (owner_ && IsWindowVisible(owner_)) GetWindowRect(owner_, &ow);
        else SystemParametersInfoW(SPI_GETWORKAREA, 0, &ow, 0);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int x = ow.left + ((ow.right - ow.left) - w) / 2;
        int y = ow.top + ((ow.bottom - ow.top) - h) / 2;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, 0);
    }

    HWND AddLabel(const wchar_t* text, int x, int y, int w, int h, bool bold = false) {
        HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
            x, y, w, h, hwnd_, nullptr, inst_, nullptr);
        SendMessageW(label, WM_SETFONT, (WPARAM)(bold && uiFontBold_ ? uiFontBold_ : uiFont_), TRUE);
        return label;
    }

    HWND AddCombo(int id, int x, int y, int w, int h) {
        return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, w, h, hwnd_, (HMENU)(INT_PTR)id, inst_, nullptr);
    }

    void CreateControls() {
        uiFont_ = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Meiryo UI");
        uiFontBold_ = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Meiryo UI");
        if (!uiFont_) uiFont_ = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        if (!uiFontBold_) uiFontBold_ = uiFont_;

        AddLabel(L"Preview 解像度", 24, 28, 150, 28, true);
        HWND res = AddCombo(IDC_RESOLUTION, 190, 24, 250, 180);
        const wchar_t* resolutions[] = { L"720p", L"480p", L"360p", L"240p", L"180p" };
        const int resValues[] = { 720, 480, 360, 240, 180 };
        for (auto text : resolutions) SendMessageW(res, CB_ADDSTRING, 0, (LPARAM)text);
        int resSel = 2;
        for (int i = 0; i < 5; ++i) if (settings_->previewResolutionP == resValues[i]) resSel = i;
        SendMessageW(res, CB_SETCURSEL, resSel, 0);
        SendMessageW(res, WM_SETFONT, (WPARAM)uiFont_, TRUE);

        AddLabel(L"待機時間 秒", 24, 78, 150, 28, true);
        HWND delay = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            190, 74, 100, 28, hwnd_, (HMENU)(INT_PTR)IDC_DELAY, inst_, nullptr);
        wchar_t delayText[32]{};
        StringCchPrintfW(delayText, 32, L"%.1f", settings_->previewDelayMs / 1000.0);
        SetWindowTextW(delay, delayText);
        SendMessageW(delay, WM_SETFONT, (WPARAM)uiFont_, TRUE);
        AddLabel(L"0.0 ～ 5.0", 304, 78, 120, 28);

        HWND audio = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            190, 122, 24, 28, hwnd_, (HMENU)(INT_PTR)IDC_AUDIO, inst_, nullptr);
        SendMessageW(audio, BM_SETCHECK, settings_->audioOnStart ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(audio, WM_SETFONT, (WPARAM)uiFont_, TRUE);
        AddLabel(L"プレビュー開始時に音を出す", 224, 124, 260, 28, true);

        AddLabel(L"プレビュー枠色", 24, 174, 150, 28, true);
        HWND color = AddCombo(IDC_COLOR, 190, 170, 250, 190);
        const wchar_t* colors[] = { L"明るい黄色", L"アクア", L"グリーン", L"オレンジ", L"ピンク", L"ホワイト", L"パープル" };
        for (auto text : colors) SendMessageW(color, CB_ADDSTRING, 0, (LPARAM)text);
        SendMessageW(color, CB_SETCURSEL, settings_->borderColorIndex, 0);
        SendMessageW(color, WM_SETFONT, (WPARAM)uiFont_, TRUE);

        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            240, 312, 92, 34, hwnd_, (HMENU)(INT_PTR)IDC_OK, inst_, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE,
            350, 312, 110, 34, hwnd_, (HMENU)(INT_PTR)IDC_CANCEL, inst_, nullptr);
        SendMessageW(ok, WM_SETFONT, (WPARAM)uiFontBold_, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    }

    void ApplyAndClose() {
        const int resValues[] = { 720, 480, 360, 240, 180 };
        int sel = (int)SendDlgItemMessageW(hwnd_, IDC_RESOLUTION, CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel > 4) sel = 2;
        settings_->previewResolutionP = resValues[sel];

        wchar_t buf[64]{};
        GetDlgItemTextW(hwnd_, IDC_DELAY, buf, 64);
        double sec = _wtof(buf);
        if (sec < 0.0) sec = 0.0;
        if (sec > 5.0) sec = 5.0;
        settings_->previewDelayMs = (int)(sec * 1000.0 + 0.5);

        settings_->audioOnStart = SendDlgItemMessageW(hwnd_, IDC_AUDIO, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int colorSel = (int)SendDlgItemMessageW(hwnd_, IDC_COLOR, CB_GETCURSEL, 0, 0);
        settings_->borderColorIndex = std::clamp<int>(colorSel, 0, 6);
        settings_->Clamp();
        settings_->Save();
        applied_ = true;
        Close(true);
    }

    void Close(bool applied) {
        applied_ = applied_ || applied;
        done_ = true;
        DestroyWindow(hwnd_);
    }
};


class SplashWindow {
    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT subFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HICON icon_ = nullptr;
    static constexpr UINT TIMER_SPLASH_CLOSE_LOCAL = 7201;

public:
    ~SplashWindow() {
        Close();
        if (titleFont_) DeleteObject(titleFont_);
        if (subFont_) DeleteObject(subFont_);
        if (smallFont_) DeleteObject(smallFont_);
    }

    bool Create(HINSTANCE inst) {
        inst_ = inst;
        WNDCLASSW cls{};
        cls.lpfnWndProc = SplashWindow::WndProc;
        cls.hInstance = inst_;
        cls.lpszClassName = kSplashWindowClass;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
        cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (!RegisterClassW(&cls)) {
            DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                Log(L"SplashWindow: RegisterClass failed: " + FormatWin32Error(err));
                return false;
            }
        }

        const int w = 460;
        const int h = 230;
        POINT pt{};
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kSplashWindowClass,
            L"MvView Splash",
            WS_POPUP,
            x, y, w, h,
            nullptr, nullptr, inst_, this);
        if (!hwnd_) {
            Log(L"SplashWindow: CreateWindowEx failed: " + FormatWin32Error(GetLastError()));
            return false;
        }
        TryEnableDarkModeForWindow(hwnd_);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);
        Log(L"SplashWindow: shown");
        return true;
    }

    void Close() {
        if (hwnd_ && IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }

private:
    void EnsureFonts() {
        if (!titleFont_) {
            titleFont_ = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        if (!subFont_) {
            subFont_ = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        if (!smallFont_) {
            smallFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        if (!icon_) icon_ = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        HPEN borderPen = CreatePen(PS_SOLID, 4, RGB(255, 238, 85));
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
        RoundRect(hdc, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2, 18, 18);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(borderPen);

        EnsureFonts();
        SetBkMode(hdc, TRANSPARENT);
        if (icon_) DrawIconEx(hdc, 26, 34, icon_, 72, 72, 0, nullptr, DI_NORMAL);

        SetTextColor(hdc, RGB(255, 248, 150));
        HFONT oldFont = (HFONT)SelectObject(hdc, titleFont_);
        RECT titleRc{ 118, 36, rc.right - 24, 78 };
        DrawTextW(hdc, L"MvView v0.20", -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        SetTextColor(hdc, RGB(235, 235, 235));
        SelectObject(hdc, subFont_);
        RECT subRc{ 120, 84, rc.right - 24, 114 };
        DrawTextW(hdc, L"Explorer Media Preview", -1, &subRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        SetTextColor(hdc, RGB(180, 180, 180));
        SelectObject(hdc, smallFont_);
        RECT msgRc{ 28, 145, rc.right - 28, 174 };
        DrawTextW(hdc, L"起動しています...", -1, &msgRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        HBRUSH barBg = CreateSolidBrush(RGB(70, 70, 56));
        RECT barRc{ 28, rc.bottom - 42, rc.right - 28, rc.bottom - 30 };
        FillRect(hdc, &barRc, barBg);
        DeleteObject(barBg);
        HBRUSH barFg = CreateSolidBrush(RGB(255, 238, 85));
        RECT barFgRc{ 28, rc.bottom - 42, rc.left + 245, rc.bottom - 30 };
        FillRect(hdc, &barFgRc, barFg);
        DeleteObject(barFg);

        SelectObject(hdc, oldFont);
        EndPaint(hwnd_, &ps);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SplashWindow* self = reinterpret_cast<SplashWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<SplashWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = hwnd;
            return TRUE;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, TIMER_SPLASH_CLOSE_LOCAL, 1400, nullptr);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            self->Close();
            return 0;
        case WM_TIMER:
            if (wp == TIMER_SPLASH_CLOSE_LOCAL) {
                KillTimer(hwnd, TIMER_SPLASH_CLOSE_LOCAL);
                self->Close();
                return 0;
            }
            break;
        case WM_PAINT:
            self->Paint();
            return 0;
        case WM_NCDESTROY:
            self->hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

class TrayIcon {
    NOTIFYICONDATAW nid_{};
    HWND owner_ = nullptr;
    HINSTANCE inst_ = nullptr;
    bool added_ = false;
    bool iconOwned_ = false;
public:
    bool Add(HWND owner, HINSTANCE inst, const std::wstring& initialTip = kAppDisplayName) {
        owner_ = owner;
        inst_ = inst;
        if (added_) Remove();

        ZeroMemory(&nid_, sizeof(nid_));
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = owner_;
        nid_.uID = TRAY_UID;
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid_.uCallbackMessage = WM_TRAYICON;

        HICON icon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        if (icon) {
            iconOwned_ = true;
        } else {
            Log(L"TrayIcon: app icon load failed, using IDI_APPLICATION: " + FormatWin32Error(GetLastError()));
            icon = LoadIconW(nullptr, IDI_APPLICATION);
            iconOwned_ = false;
        }
        nid_.hIcon = icon;

        StringCchCopyW(nid_.szTip, ARRAYSIZE(nid_.szTip), initialTip.c_str());
        added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
        if (!added_) {
            DWORD err = GetLastError();
            Log(L"TrayIcon: Shell_NotifyIcon(NIM_ADD) failed: " + FormatWin32Error(err));
            std::wstring msg = L"MvView could not add the task tray icon.\n\n"
                L"Shell_NotifyIcon(NIM_ADD) failed: " + FormatWin32Error(err) + L"\n\n"
                L"Log: " + LogPath();
            MessageBoxW(owner_, msg.c_str(), L"MvView v0.20 tray error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return false;
        }

        nid_.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &nid_)) {
            Log(L"TrayIcon: Shell_NotifyIcon(NIM_SETVERSION) failed: " + FormatWin32Error(GetLastError()));
        }
        Log(L"TrayIcon: added successfully");
        return true;
    }
    bool IsAdded() const { return added_; }
    void UpdateTip(const std::wstring& tip) {
        if (!nid_.hWnd || !added_) return;
        nid_.uFlags = NIF_TIP | NIF_SHOWTIP;
        StringCchCopyW(nid_.szTip, ARRAYSIZE(nid_.szTip), tip.c_str());
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid_)) Log(L"TrayIcon: Shell_NotifyIcon(NIM_MODIFY) failed: " + FormatWin32Error(GetLastError()));
        else Log(L"TrayIcon: tooltip updated: " + tip);
    }
    void Remove() {
        if (added_ && nid_.hWnd) Shell_NotifyIconW(NIM_DELETE, &nid_);
        if (nid_.hIcon && iconOwned_) DestroyIcon(nid_.hIcon);
        ZeroMemory(&nid_, sizeof(nid_));
        added_ = false;
        iconOwned_ = false;
    }
    void ShowMenu(const Settings& settings, bool previewVisible) {
        UNREFERENCED_PARAMETER(settings);
        UNREFERENCED_PARAMETER(previewVisible);

        TryEnableAppDarkMode();
        TryEnableDarkModeForWindow(owner_);
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();

        // Keep the first tray menu intentionally small for v0.12.
        // Detailed folders are still accessible from About/Help text.
        AppendMenuW(menu, MF_STRING, CMD_TRAY_OPEN_SETTINGS, L"設定");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CMD_TRAY_ABOUT, L"このMvViewについて");
        AppendMenuW(menu, MF_STRING, CMD_TRAY_HELP, L"Help");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CMD_TRAY_EXIT, L"終了");

        SetForegroundWindow(owner_);
        UINT cmd = TrackPopupMenu(menu,
            TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RETURNCMD,
            pt.x, pt.y, 0, owner_, nullptr);
        DestroyMenu(menu);

        if (cmd != 0) {
            PostMessageW(owner_, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
        }
        PostMessageW(owner_, WM_NULL, 0, 0);
    }
};

class MvViewApp {
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    Settings settings_;
    ExplorerReader explorer_;
    PreviewWindow preview_;
    std::vector<std::unique_ptr<PreviewWindow>> extraPreviews_;
    TrayIcon tray_;
    SplashWindow splash_;
    HWINEVENTHOOK hookForeground_ = nullptr;
    HWINEVENTHOOK hookSelection_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    POINT pendingAnchor_{};
    std::wstring pendingPath_;
    std::wstring visibleMediaSetKey_;
    bool pendingFromMouseMove_ = false;
    bool pendingFromMouseClick_ = false;
    bool pendingFromSelectionEvent_ = false;
    bool allowMouseReturnStart_ = false;
    bool multiPreviewActive_ = false;
    RECT previewGroupRect_{};
    bool comReady_ = false;
    bool mpvRuntimeReady_ = false;
    std::wstring mpvRuntimeStatus_;
    UINT taskbarCreatedMsg_ = 0;

public:
    int Run(HINSTANCE inst, int show, const std::wstring& initialOpenPath = L"") {
        inst_ = inst;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        TryEnableAppDarkMode();
        taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comReady_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        settings_.Load();
        Log(L"MvView v0.20 started");
        Log(L"EXE path: " + GetModulePathText());
        splash_.Create(inst_);

        if (!CreateMainWindow()) {
            DWORD err = GetLastError();
            Log(L"CreateMainWindow failed: " + FormatWin32Error(err));
            splash_.Close();
            MessageBoxW(nullptr, (L"MvView main window creation failed.\n\n" + FormatWin32Error(err) + L"\n\nLog: " + LogPath()).c_str(),
                L"MvView v0.20 startup error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return 1;
        }

        if (!tray_.Add(hwnd_, inst_, L"MvView v0.20 / starting")) {
            Log(L"startup aborted because tray icon could not be added");
            splash_.Close();
            return 1;
        }

        if (!preview_.Create(inst_, &settings_)) Log(L"PreviewWindow.Create failed: " + FormatWin32Error(GetLastError()));

        mpvRuntimeReady_ = ProbeMpvRuntime(&mpvRuntimeStatus_);
        Log(L"runtime status: " + mpvRuntimeStatus_);
        tray_.UpdateTip(MpvTrayTip());
        InstallHooks();
        if (!initialOpenPath.empty()) {
            OpenExternalPath(initialOpenPath);
        } else {
            ScheduleSelectionCheck(L"startup");
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Shutdown();
        return (int)msg.wParam;
    }

    HWND MainHwnd() const { return hwnd_; }

    void OpenExternalPath(const std::wstring& path) {
        if (path.empty() || !FileExists(path)) return;
        MediaKind kind = DetectMediaKind(path);
        if (kind == MediaKind::None) return;
        POINT pt{};
        GetCursorPos(&pt);
        Log(L"external open: " + path);
        preview_.ShowPath(path, kind, pt);
        SetTimer(hwnd_, TIMER_CURSOR_CLOSE, 250, nullptr);
    }

private:
    bool CreateMainWindow() {
        WNDCLASSW cls{};
        cls.lpfnWndProc = MvViewApp::WndProc;
        cls.hInstance = inst_;
        cls.lpszClassName = kMainWindowClass;
        cls.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));

        ATOM atom = RegisterClassW(&cls);
        if (!atom) {
            DWORD err = GetLastError();
            if (err == ERROR_CLASS_ALREADY_EXISTS) {
                Log(L"RegisterClass skipped: class already exists in this process");
            } else {
                Log(L"RegisterClass failed: " + FormatWin32Error(err));
                return false;
            }
        } else {
            Log(L"RegisterClass succeeded for main window");
        }

        SetLastError(ERROR_SUCCESS);
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainWindowClass, kAppName, WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, inst_, this);
        if (!hwnd_) {
            DWORD err = GetLastError();
            Log(L"CreateWindowEx failed: " + FormatWin32Error(err));
            return false;
        }
        Log(L"Main hidden window created");
        return true;
    }

    void InstallHooks() {
        hookForeground_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        hookSelection_ = SetWinEventHook(EVENT_OBJECT_FOCUS_VALUE, EVENT_OBJECT_SELECTIONWITHIN_VALUE, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);

        if (!hookForeground_) Log(L"SetWinEventHook foreground failed: " + FormatWin32Error(GetLastError()));
        else Log(L"SetWinEventHook foreground installed");

        if (!hookSelection_) Log(L"SetWinEventHook selection/focus failed: " + FormatWin32Error(GetLastError()));
        else Log(L"SetWinEventHook selection/focus installed");

        if (!mouseHook_) Log(L"SetWindowsHookEx(WH_MOUSE_LL) failed: " + FormatWin32Error(GetLastError()));
        else Log(L"SetWindowsHookEx(WH_MOUSE_LL) installed");
    }

    void Shutdown() {
        KillTimer(hwnd_, TIMER_DEBOUNCE);
        KillTimer(hwnd_, TIMER_CURSOR_CLOSE);
        if (hookForeground_) UnhookWinEvent(hookForeground_);
        if (hookSelection_) UnhookWinEvent(hookSelection_);
        if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
        hookForeground_ = nullptr;
        hookSelection_ = nullptr;
        mouseHook_ = nullptr;
        tray_.Remove();
        preview_.Destroy();
        for (auto& w : extraPreviews_) if (w) w->Destroy();
        extraPreviews_.clear();
        settings_.Save();
        Log(L"MvView exited");
        if (comReady_) CoUninitialize();
    }

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
        HWND main = FindWindowW(kMainWindowClass, nullptr);
        if (main) PostMessageW(main, WM_MV_HOOK_EVENT, (WPARAM)event, (LPARAM)hwnd);
    }

    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION) {
            // Mouse clicks are the normal preview trigger.  Mouse move is throttled and
            // used only to re-arm a previously closed multi-preview when the pointer
            // returns near the original Explorer selection anchor.
            bool post = (wp == WM_LBUTTONUP || wp == WM_RBUTTONUP || wp == WM_MBUTTONUP);
            if (!post && wp == WM_MOUSEMOVE) {
                static DWORD lastMovePost = 0;
                DWORD now = GetTickCount();
                if (now - lastMovePost >= 250) {
                    lastMovePost = now;
                    post = true;
                }
            }
            if (post) {
                HWND main = FindWindowW(kMainWindowClass, nullptr);
                if (main) PostMessageW(main, WM_MV_MOUSE_EVENT, wp, 0);
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    bool ForegroundIsExplorer(HWND* outForeground = nullptr) {
        HWND fg = GetForegroundWindow();
        if (outForeground) *outForeground = fg;
        return fg && explorer_.IsExplorerWindow(fg);
    }

    void ScheduleSelectionCheck(const std::wstring& reason) {
        if (!settings_.enabled) return;
        HWND fg = nullptr;
        if (!ForegroundIsExplorer(&fg)) {
            if (reason.find(L"mouse") != std::wstring::npos || reason.find(L"tray") != std::wstring::npos) {
                Log(L"selection check skipped; foreground is not Explorer: " + reason);
            }
            if (settings_.closeWhenForegroundLost && AnyPreviewVisible()) ClosePreview(L"foreground lost");
            return;
        }

        const bool fromMouseMove = reason.find(L"mouse move") != std::wstring::npos;
        const bool fromMouseClick = reason.find(L"mouse click") != std::wstring::npos;
        const bool fromSelectionEvent = reason.find(L"selection/focus") != std::wstring::npos;

        // Important: WH_MOUSE_LL posts mouse-move messages while the user is waiting
        // for the debounce timer.  Do not let those moves replace a real click or
        // Explorer selection event, otherwise a single-selection preview is skipped
        // as a stale mouse-move replay when the pointer moved even slightly.
        if (fromMouseMove && (pendingFromMouseClick_ || pendingFromSelectionEvent_)) {
            Log(L"selection check ignored mouse move during active click/selection debounce");
            return;
        }

        if (fromMouseMove) {
            // Mouse movement must not replay stale single selections, but it is used
            // for the intended Ctrl-click workflow: select several media files, release
            // Ctrl, then move the mouse slightly to show the tiled multi-preview.  The
            // actual single/multi decision is made in CheckSelectionNow().
            if (AnyPreviewVisible() && !multiPreviewActive_) return;
            POINT pt{};
            GetCursorPos(&pt);
            if (allowMouseReturnStart_) {
                const int startPx = std::min<int>(180, std::max<int>(90, settings_.cursorFarClosePx));
                if (std::abs(pt.x - pendingAnchor_.x) > startPx || std::abs(pt.y - pendingAnchor_.y) > startPx) return;
            } else {
                pendingAnchor_ = pt;
            }
        } else {
            GetCursorPos(&pendingAnchor_);
            allowMouseReturnStart_ = false;

            // When the user clicks/selects another file while a tiled multi-preview is
            // running, stop the old group immediately.  The new preview still honors
            // the configured wait time before opening.
            if (AnyPreviewVisible() && multiPreviewActive_) {
                Log(L"multi preview stopped because a new Explorer selection is pending");
                ClosePreview(L"new selection pending", false);
            }
        }

        pendingFromMouseMove_ = fromMouseMove;
        pendingFromMouseClick_ = fromMouseClick;
        pendingFromSelectionEvent_ = fromSelectionEvent;
        KillTimer(hwnd_, TIMER_DEBOUNCE);
        SetTimer(hwnd_, TIMER_DEBOUNCE, (UINT)settings_.previewDelayMs, nullptr);
        Log(L"scheduled selection check: " + reason);
    }

    bool CursorIsOverWindow(HWND hwnd, POINT pt) const {
        if (!hwnd || !IsWindow(hwnd)) return false;
        RECT rc{};
        if (!GetWindowRect(hwnd, &rc)) return false;
        return PointInRect(rc, pt);
    }

    bool CursorStillValidForPreview(HWND explorerWindow, bool fromMouseMove) const {
        POINT pt{};
        GetCursorPos(&pt);
        if (CursorInsideAnyPreview(pt)) return true;
        if (!CursorIsOverWindow(explorerWindow, pt)) return false;

        // A real click/selection event should still be allowed after the preview
        // delay even if the user moved the pointer within Explorer.  The previous
        // anchor-distance check made the preview fail whenever the mouse moved even
        // a little before the wait time elapsed.  Keep the strict suppression only
        // for mouse-move-originated checks, which are the stale replay risk.
        if (!fromMouseMove) return true;

        const int startPx = std::min<int>(220, std::max<int>(120, settings_.cursorFarClosePx));
        return std::abs(pt.x - pendingAnchor_.x) <= startPx && std::abs(pt.y - pendingAnchor_.y) <= startPx;
    }

    void CheckSelectionNow() {
        KillTimer(hwnd_, TIMER_DEBOUNCE);
        const bool checkFromMouseMove = pendingFromMouseMove_;
        const bool checkFromMouseClick = pendingFromMouseClick_;
        const bool checkFromSelectionEvent = pendingFromSelectionEvent_;
        pendingFromMouseMove_ = false;
        pendingFromMouseClick_ = false;
        pendingFromSelectionEvent_ = false;
        UNREFERENCED_PARAMETER(checkFromSelectionEvent);
        if (!settings_.enabled) return;
        HWND fg = nullptr;
        if (!ForegroundIsExplorer(&fg)) {
            if (settings_.closeWhenForegroundLost) ClosePreview(L"foreground lost");
            return;
        }
        std::wstring focusedPath;
        auto selected = explorer_.GetSelectedPathsForForeground(fg, &focusedPath);
        Log(L"selection check now: selectedCount=" + std::to_wstring(selected.size()) +
            (focusedPath.empty() ? L"" : (L" focused=" + focusedPath)));
        if (selected.empty()) {
            pendingPath_.clear();
            Log(L"selection check: empty selection");
            if (settings_.closeWhenSelectionEmpty) ClosePreview(L"selection empty");
            return;
        }
        for (size_t idx = 0; idx < selected.size() && idx < 8; ++idx) {
            Log(L"selection item[" + std::to_wstring(idx) + L"]=" + selected[idx]);
        }

        std::vector<std::pair<std::wstring, MediaKind>> media;
        for (const auto& p : selected) {
            MediaKind k = DetectMediaKind(p);
            if (k != MediaKind::None && FileExists(p)) media.push_back({ p, k });
        }
        if (media.empty()) {
            Log(L"selection check: no supported media in selection");
            if (settings_.closeOnNonMedia) ClosePreview(L"non-media selected");
            return;
        }

        POINT cursorNow{};
        GetCursorPos(&cursorNow);
        const bool cursorOverPreview = CursorInsideAnyPreview(cursorNow);
        const bool cursorOverExplorer = CursorIsOverWindow(fg, cursorNow);
        if (media.size() <= 1) {
            if (!CursorStillValidForPreview(fg, checkFromMouseMove)) {
                Log(L"selection check skipped; cursor is outside Explorer and preview");
                if (AnyPreviewVisible()) ClosePreview(L"cursor outside Explorer/preview");
                return;
            }
        } else {
            // Multiple preview is intentionally allowed after Ctrl-click when the
            // pointer is still anywhere over the Explorer window or over an existing
            // preview.  The previous strict anchor check was preventing multi-preview
            // from opening after the user released Ctrl and moved the mouse.
            if (!cursorOverExplorer && !cursorOverPreview) {
                Log(L"multi selection check skipped; cursor is outside Explorer and preview");
                if (AnyPreviewVisible()) ClosePreview(L"cursor outside Explorer/preview");
                return;
            }
        }

        if (checkFromMouseMove) {
            if (media.size() == 1) {
                Log(L"selection check skipped; mouse-move event with single selection");
                return;
            }
            Log(allowMouseReturnStart_
                ? L"selection check: mouse return multi-preview restart allowed"
                : L"selection check: mouse move multi-selection start allowed");
        }

        const std::wstring newMediaSetKey = MakeMediaSetKey(media);
        if (AnyPreviewVisible() && !visibleMediaSetKey_.empty() && newMediaSetKey == visibleMediaSetKey_) {
            if (multiPreviewActive_ && media.size() > 1 &&
                ((checkFromMouseMove && allowMouseReturnStart_) || checkFromMouseClick)) {
                Log(checkFromMouseClick
                    ? L"selection check: same multi media set clicked again; restart all previews"
                    : L"selection check: same multi media set visible; restart from return event");
                ShowMultiple(media);
            } else {
                Log(L"selection check: same media set already visible; skip reload");
            }
            return;
        }

        if (media.size() == 1) {
            const auto& target = media[0].first;
            if (preview_.IsVisible() && ToLower(preview_.CurrentPath()) == ToLower(target) && extraPreviews_.empty()) {
                Log(L"selection check: same media already visible; skip reload");
                return;
            }
            CloseExtraPreviews();
            multiPreviewActive_ = false;
            allowMouseReturnStart_ = false;
            SetRectEmpty(&previewGroupRect_);
            pendingPath_ = target;
            visibleMediaSetKey_ = newMediaSetKey;
            Log(L"preview open: " + target);
            preview_.ShowPath(target, media[0].second, pendingAnchor_);
        } else {
            Log(L"multi preview open: count=" + std::to_wstring(media.size()));
            ShowMultiple(media);
        }
        SetTimer(hwnd_, TIMER_CURSOR_CLOSE, 250, nullptr);
    }

    std::wstring MakeMediaSetKey(const std::vector<std::pair<std::wstring, MediaKind>>& media) const {
        std::vector<std::wstring> parts;
        const size_t maxCount = std::min<size_t>(media.size(), 9);
        parts.reserve(maxCount);
        for (size_t i = 0; i < maxCount; ++i) parts.push_back(ToLower(media[i].first));
        std::sort(parts.begin(), parts.end());
        std::wstring key;
        for (const auto& p : parts) {
            key += p;
            key += L"\n";
        }
        return key;
    }

    bool AnyPreviewVisible() const {
        if (preview_.IsVisible()) return true;
        for (const auto& w : extraPreviews_) if (w && w->IsVisible()) return true;
        return false;
    }

    bool CursorInsideAnyPreview(POINT pt) const {
        if (preview_.ContainsScreenPoint(pt)) return true;
        for (const auto& w : extraPreviews_) if (w && w->ContainsScreenPoint(pt)) return true;
        return false;
    }

    bool AllMultiPreviewsAtEnd() const {
        if (!multiPreviewActive_ || !preview_.IsVisible() || !preview_.IsAtEnd()) return false;
        for (const auto& w : extraPreviews_) {
            if (w && w->IsVisible() && !w->IsAtEnd()) return false;
        }
        return true;
    }

    void CloseExtraPreviews() {
        for (auto& w : extraPreviews_) if (w) w->Destroy();
        extraPreviews_.clear();
    }

    void ShowMultiple(const std::vector<std::pair<std::wstring, MediaKind>>& media) {
        const std::wstring incomingKey = MakeMediaSetKey(media);
        visibleMediaSetKey_ = incomingKey;
        ClosePreview(L"multi reload", false);
        // ClosePreview clears the visible key by design; restore the incoming key for
        // same-selection suppression after the windows are recreated.
        visibleMediaSetKey_ = incomingKey;
        CloseExtraPreviews();
        multiPreviewActive_ = true;
        allowMouseReturnStart_ = false;
        const size_t maxCount = std::min<size_t>(media.size(), 9);
        const int count = static_cast<int>(maxCount);
        int cols = 1;
        if (count <= 1) cols = 1;
        else if (count <= 3) cols = count;
        else if (count <= 4) cols = 2;
        else cols = 3;
        const int rows = (count + cols - 1) / cols;
        const int gap = 12;
        const int cellW = preview_.WindowWidth();
        const int cellH = preview_.WindowHeight();

        POINT cursor = pendingAnchor_;
        HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        RECT work = mi.rcWork;
        const int gridW = cols * cellW + (cols - 1) * gap;
        const int gridH = rows * cellH + (rows - 1) * gap;
        int left = cursor.x + 18;
        int top = cursor.y + 18;
        if (left + gridW > work.right) left = work.right - gridW - 8;
        if (top + gridH > work.bottom) top = work.bottom - gridH - 8;
        if (left < work.left) left = work.left + 8;
        if (top < work.top) top = work.top + 8;
        previewGroupRect_ = { left, top, left + gridW, top + gridH };

        Log(L"multi preview layout: count=" + std::to_wstring(count) + L" cols=" + std::to_wstring(cols) + L" rows=" + std::to_wstring(rows));
        for (size_t i = 0; i < maxCount; ++i) {
            PreviewWindow* w = nullptr;
            if (i == 0) {
                w = &preview_;
            } else {
                auto extra = std::make_unique<PreviewWindow>();
                if (!extra->Create(inst_, &settings_)) {
                    Log(L"multi preview: extra window create failed");
                    continue;
                }
                w = extra.get();
                extraPreviews_.push_back(std::move(extra));
            }
            int col = static_cast<int>(i) % cols;
            int row = static_cast<int>(i) / cols;
            POINT topLeft{ left + col * (cellW + gap), top + row * (cellH + gap) };
            POINT logicalAnchor{ topLeft.x - 18, topLeft.y - 18 };
            if (w->ShowPathAt(media[i].first, media[i].second, topLeft, logicalAnchor, true)) {
                Log(L"multi preview queued paused: " + media[i].first);
            }
        }
        // Start every mpv instance after all preview windows and files are queued.
        // This gives actual simultaneous playback instead of only the last window
        // looking active while earlier windows are still loading.
        preview_.StartPlayback();
        for (auto& w : extraPreviews_) if (w) w->StartPlayback();
        Log(L"multi preview playback started together");
    }

    void ClosePreview(const std::wstring& reason, bool allowReturnReopen = true) {
        const bool hadMulti = multiPreviewActive_ && AnyPreviewVisible();
        if (AnyPreviewVisible()) Log(L"preview close: " + reason);
        visibleMediaSetKey_.clear();
        preview_.Hide();
        for (auto& w : extraPreviews_) if (w) w->Hide();
        if (hadMulti && allowReturnReopen &&
            (reason.find(L"cursor") != std::wstring::npos || reason.find(L"selection") != std::wstring::npos)) {
            allowMouseReturnStart_ = true;
            Log(L"multi preview return-reopen armed");
        }
        if (!AnyPreviewVisible()) KillTimer(hwnd_, TIMER_CURSOR_CLOSE);
    }

    void CheckCursorClose() {
        if (!AnyPreviewVisible()) { KillTimer(hwnd_, TIMER_CURSOR_CLOSE); return; }
        preview_.Tick();
        for (auto& w : extraPreviews_) if (w) w->Tick();
        if (multiPreviewActive_ && !allowMouseReturnStart_ && AllMultiPreviewsAtEnd()) {
            allowMouseReturnStart_ = true;
            Log(L"multi preview ended; return-reopen armed");
        }
        POINT pt{};
        GetCursorPos(&pt);
        if (CursorInsideAnyPreview(pt)) return;

        if (multiPreviewActive_) {
            // Multiple windows must not vanish while the pointer is travelling from
            // the Explorer selection toward one of the tiled previews.  Keep the
            // group alive while the pointer is near the grid or near the selection
            // anchor, and close only after it is clearly away from both.
            const int margin = std::max<int>(220, settings_.cursorFarClosePx);
            RECT expanded = previewGroupRect_;
            InflateRect(&expanded, margin, margin);
            if (PointInRect(expanded, pt)) return;
            if (std::abs(pt.x - pendingAnchor_.x) <= margin && std::abs(pt.y - pendingAnchor_.y) <= margin) return;
            ClosePreview(L"cursor moved far from multi preview");
            return;
        }

        POINT a = preview_.IsVisible() ? preview_.Anchor() : pt;
        if (preview_.IsVisible() && (std::abs(pt.x - a.x) >= settings_.cursorFarClosePx || std::abs(pt.y - a.y) >= settings_.cursorFarClosePx)) {
            ClosePreview(L"cursor moved far");
        }
    }

    void ToggleEnabled() {
        settings_.enabled = !settings_.enabled;
        settings_.Save();
        if (!settings_.enabled) ClosePreview(L"disabled");
        else ScheduleSelectionCheck(L"enabled");
    }

    void OpenSettingsDialog() {
        SettingsDialog dlg;
        if (dlg.Show(inst_, hwnd_, settings_)) {
            settings_.Save();
            Log(L"settings updated from dialog");
            if (AnyPreviewVisible()) ClosePreview(L"settings changed");
        }
    }

    void OpenMpvRuntimeFolder() {
        ShellExecuteW(nullptr, L"open", RuntimeDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenLogsFolder() {
        ShellExecuteW(nullptr, L"open", LogsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    std::wstring MpvTrayTip() const {
        return std::wstring(kAppDisplayName) + (mpvRuntimeReady_ ? L" / mpv ok" : L" / mpv missing");
    }

    void RefreshMpvRuntimeStatus() {
        mpvRuntimeReady_ = ProbeMpvRuntime(&mpvRuntimeStatus_);
        tray_.UpdateTip(MpvTrayTip());
        Log(L"runtime status refreshed: " + mpvRuntimeStatus_);
    }

    void ShowAbout() {
        RefreshMpvRuntimeStatus();
        std::wstring msg = std::wstring(L"MvView v0.20 First Plot\n\n") +
            L"Explorerで選択した画像・動画・音声を、libmpvで小窓プレビューします。\n\n" +
            L"状態: " + mpvRuntimeStatus_ + L"\n\n" +
            L"EXE: " + GetModulePathText() + L"\n" +
            L"mpv runtime: " + RuntimeDir() + L"\n" +
            L"設定: " + SettingsPath() + L"\n" +
            L"ログ: " + LogPath();
        MessageBoxW(hwnd_, msg.c_str(), L"MvView v0.20 について", MB_OK | MB_ICONINFORMATION);
    }

    void ShowHelp() {
        std::wstring msg = std::wstring(L"MvView Help\n\n") +
            L"基本操作:\n" +
            L"- Explorerで画像・動画・音声ファイルをクリック選択するとプレビューします。\n" +
            L"- プレビュー小窓をクリック、またはESCで閉じます。\n" +
            L"- トレイアイコンを右クリックするとメニューを表示します。\n\n" +
            L"mpv runtime:\n" +
            L"- EXE横の mpv-2.dll / libmpv-2.dll を探します。\n" +
            L"- EXE横の runtime\\mpv-2.dll / runtime\\libmpv-2.dll も探します。\n\n" +
            L"設定フォルダ:\n" + AppDataDir() + L"\n\n" +
            L"ログ:\n" + LogPath();
        MessageBoxW(hwnd_, msg.c_str(), L"MvView v0.20 Help", MB_OK | MB_ICONINFORMATION);
    }

    LRESULT OnCopyData(const COPYDATASTRUCT* cds) {
        if (!cds || !cds->lpData || cds->cbData < sizeof(wchar_t)) return 0;
        std::wstring path((const wchar_t*)cds->lpData, (cds->cbData / sizeof(wchar_t)) - 1);
        OpenExternalPath(path);
        return 1;
    }

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (taskbarCreatedMsg_ && msg == taskbarCreatedMsg_) {
            Log(L"TaskbarCreated received; re-adding tray icon");
            tray_.Add(hwnd_, inst_, MpvTrayTip());
            tray_.UpdateTip(MpvTrayTip());
            return 0;
        }

        switch (msg) {
        case WM_MV_HOOK_EVENT:
            if (wp == EVENT_SYSTEM_FOREGROUND) {
                HWND eventHwnd = reinterpret_cast<HWND>(lp);
                if (!explorer_.IsExplorerWindow(eventHwnd)) {
                    if (settings_.closeWhenForegroundLost && AnyPreviewVisible()) ClosePreview(L"foreground lost");
                } else {
                    Log(L"foreground Explorer event observed; no preview start");
                }
            } else if (wp == EVENT_OBJECT_FOCUS_VALUE || wp == EVENT_OBJECT_SELECTION_VALUE || wp == EVENT_OBJECT_SELECTIONADD_VALUE || wp == EVENT_OBJECT_SELECTIONREMOVE_VALUE || wp == EVENT_OBJECT_SELECTIONWITHIN_VALUE) {
                ScheduleSelectionCheck(L"selection/focus event");
            }
            return 0;
        case WM_MV_MOUSE_EVENT:
            if (wp == WM_MOUSEMOVE) ScheduleSelectionCheck(L"mouse move return event");
            else ScheduleSelectionCheck(L"mouse click event");
            return 0;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { CheckSelectionNow(); return 0; }
            if (wp == TIMER_CURSOR_CLOSE) { CheckCursorClose(); return 0; }
            return 0;
        case WM_HOTKEY:
            if (wp == HOTKEY_ESCAPE) { ClosePreview(L"escape"); return 0; }
            return 0;
        case WM_TRAYICON: {
            const UINT trayEvent = LOWORD(lp);
            Log(L"TrayIcon: event=" + std::to_wstring(trayEvent));

            if (trayEvent == WM_RBUTTONDOWN || trayEvent == WM_RBUTTONUP || trayEvent == WM_CONTEXTMENU) {
                tray_.ShowMenu(settings_, AnyPreviewVisible());
            } else if (trayEvent == WM_LBUTTONUP || trayEvent == NIN_SELECT || trayEvent == NIN_KEYSELECT) {
                if (AnyPreviewVisible()) ClosePreview(L"tray left click");
                else ScheduleSelectionCheck(L"tray left click");
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case CMD_TRAY_ENABLED: ToggleEnabled(); break;
            case CMD_TRAY_CLOSE_PREVIEW: ClosePreview(L"tray command"); break;
            case CMD_TRAY_OPEN_SETTINGS: OpenSettingsDialog(); break;
            case CMD_TRAY_OPEN_MPV_RUNTIME: OpenMpvRuntimeFolder(); break;
            case CMD_TRAY_OPEN_LOGS: OpenLogsFolder(); break;
            case CMD_TRAY_ABOUT: ShowAbout(); break;
            case CMD_TRAY_HELP: ShowHelp(); break;
            case CMD_TRAY_EXIT: DestroyWindow(hwnd_); break;
            }
            return 0;
        case WM_MV_SHOW_STATUS:
            ShowAbout();
            return 0;
        case WM_COPYDATA:
            return OnCopyData(reinterpret_cast<COPYDATASTRUCT*>(lp));
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        MvViewApp* self = reinterpret_cast<MvViewApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<MvViewApp*>(cs->lpCreateParams);
            if (self) {
                self->hwnd_ = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                Log(L"Main WndProc: WM_NCCREATE accepted");
                return TRUE;
            }
            return FALSE;
        }
        if (self) return self->HandleMessage(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

std::wstring GetArgValue(int argc, wchar_t** argv, const std::wstring& key) {
    for (int i = 1; i < argc; ++i) {
        if (ToLower(argv[i]) == ToLower(key) && i + 1 < argc) return argv[i + 1];
    }
    return L"";
}

bool HasArg(int argc, wchar_t** argv, const std::wstring& key) {
    for (int i = 1; i < argc; ++i) if (ToLower(argv[i]) == ToLower(key)) return true;
    return false;
}

bool SendOpenPathToExisting(const std::wstring& path) {
    HWND existing = FindWindowW(kMainWindowClass, nullptr);
    if (!existing || path.empty()) return false;
    COPYDATASTRUCT cds{};
    cds.dwData = 1;
    cds.cbData = (DWORD)((path.size() + 1) * sizeof(wchar_t));
    cds.lpData = (void*)path.c_str();
    SendMessageW(existing, WM_COPYDATA, 0, (LPARAM)&cds);
    return true;
}

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* info) {
    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void* address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    std::wstringstream ss;
    ss << L"Unhandled exception: code=0x" << std::hex << code << L" address=" << address;
    Log(ss.str());
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace mvview

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    using namespace mvview;

    SetUnhandledExceptionFilter(UnhandledExceptionHandler);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring openPath = argv ? GetArgValue(argc, argv, L"--open") : L"";
    bool noSingle = argv ? HasArg(argc, argv, L"--no-single-instance") : false;

    HANDLE mutex = nullptr;
    if (!noSingle) {
        mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(kMainWindowClass, nullptr);
            if (!openPath.empty()) {
                if (!SendOpenPathToExisting(openPath)) {
                    MessageBoxW(nullptr, L"MvView is already running, but the main window was not found.\n\nPlease run:\nGet-Process MvView -ErrorAction SilentlyContinue | Stop-Process -Force",
                        L"MvView v0.20 already running", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                }
            } else if (existing) {
                PostMessageW(existing, WM_MV_SHOW_STATUS, 0, 0);
            } else {
                MessageBoxW(nullptr, L"MvView mutex already exists, but the tray/main window was not found.\n\nAn old hidden process may be running. Please run:\nGet-Process MvView -ErrorAction SilentlyContinue | Stop-Process -Force",
                    L"MvView v0.20 hidden instance", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            }
            if (argv) LocalFree(argv);
            CloseHandle(mutex);
            return 0;
        }
    }

    MvViewApp app;
    int rc = app.Run(hInstance, nCmdShow, openPath);

    if (argv) LocalFree(argv);
    if (mutex) CloseHandle(mutex);
    return rc;
}

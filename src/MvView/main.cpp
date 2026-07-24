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
#include <shlguid.h>
#include <servprov.h>
#include <UIAutomation.h>
#include <UIAutomationClient.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shldisp.h>
#include <commctrl.h>
#include <objbase.h>
#include <strsafe.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <optional>
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
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "winhttp.lib")

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
constexpr wchar_t kAppVersion[] = MVVIEW_VERSION_TEXT_W;
constexpr wchar_t kAppDisplayName[] = MVVIEW_APP_DISPLAY_NAME_W;
constexpr wchar_t kMainWindowClass[] = L"MvView.MainWindow";
constexpr wchar_t kPreviewWindowClass[] = L"MvView.PreviewWindow";
constexpr wchar_t kMpvHostClass[] = L"MvView.MpvHost";
constexpr wchar_t kSplashWindowClass[] = L"MvView.SplashWindow";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MvView.SingleInstance";
constexpr UINT WM_TRAYICON = WM_APP + 10;
constexpr ULONG_PTR MVVIEW_HOVER_COPYDATA_ID = 0x4D564831; // "MVH1"
constexpr int MVVIEW_HOVER_PROTOCOL_VERSION = 1;
constexpr UINT WM_MV_HOOK_EVENT = WM_APP + 20;
constexpr UINT WM_MV_OPEN_PATH = WM_APP + 21;
constexpr UINT WM_MPV_WAKEUP = WM_APP + 22;
constexpr UINT WM_MV_SHOW_STATUS = WM_APP + 23;
constexpr UINT WM_MV_MOUSE_EVENT = WM_APP + 24;
constexpr UINT WM_MV_MULTI_CONFIRM = WM_APP + 25;
constexpr UINT WM_MV_UPDATE_AVAILABLE = WM_APP + 26;
constexpr UINT TIMER_CURSOR_CLOSE = 1002; // preview progress tick
constexpr UINT TIMER_HOVER_MONITOR = 1003;
constexpr UINT TIMER_SELECTION_VALIDATE = 1004;
constexpr UINT HOTKEY_ESCAPE = 4101;
constexpr int TRAY_UID = 1;
constexpr wchar_t kLatestReleaseApiHost[] = L"api.github.com";
constexpr wchar_t kLatestReleaseApiPath[] = L"/repos/cyfomix-ui/MvView/releases/latest";

struct UpdateInfo {
    std::wstring version;
    std::wstring downloadUrl;
};

std::vector<int> ParseVersionParts(std::wstring version) {
    while (!version.empty() && (version.front() == L'v' || version.front() == L'V' || iswspace(version.front()))) {
        version.erase(version.begin());
    }
    std::vector<int> parts;
    std::wstringstream stream(version);
    std::wstring part;
    while (std::getline(stream, part, L'.')) {
        wchar_t* end = nullptr;
        long value = wcstol(part.c_str(), &end, 10);
        if (end == part.c_str()) return {};
        parts.push_back(static_cast<int>(value));
    }
    return parts;
}

bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current) {
    auto left = ParseVersionParts(candidate);
    auto right = ParseVersionParts(current);
    if (left.empty() || right.empty()) return false;
    const size_t count = std::max(left.size(), right.size());
    left.resize(count);
    right.resize(count);
    return left > right;
}

// Hook callbacks run outside the application message dispatch. Keep them minimal:
// they only post the event and full 32-bit screen coordinates to this window.
HWND gHookTargetWindow = nullptr;
static_assert(sizeof(LPARAM) >= sizeof(std::uint64_t), "MvView hover coordinate transport requires an x64 build.");

LPARAM PackScreenPoint(POINT pt) {
    const std::uint64_t x = static_cast<std::uint32_t>(pt.x);
    const std::uint64_t y = static_cast<std::uint32_t>(pt.y);
    return static_cast<LPARAM>((x << 32) | y);
}

POINT UnpackScreenPoint(LPARAM packed) {
    const std::uint64_t value = static_cast<std::uint64_t>(packed);
    POINT pt{};
    pt.x = static_cast<LONG>(static_cast<std::int32_t>(value >> 32));
    pt.y = static_cast<LONG>(static_cast<std::int32_t>(value & 0xffffffffULL));
    return pt;
}

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

enum class PreviewTrigger {
    None,
    HoverSingle,
    HoverMultiConfirmed,
    DirectOpen,
    ExternalHover
};

enum class HoverState {
    Idle,
    WaitingSingle,
    WaitingMulti,
    ConfirmingMulti,
    PlayingSingle,
    PlayingMulti
};

enum class PreviewCloseReason {
    HoverLeft,
    HoverTargetChanged,
    SelectionChanged,
    ForegroundLost,
    Escape,
    TrayCommand,
    Disabled,
    SettingsChanged,
    DirectOpenReplaced,
    ExternalHoverReplaced,
    Reload,
    Shutdown
};

std::wstring PreviewCloseReasonText(PreviewCloseReason reason) {
    switch (reason) {
    case PreviewCloseReason::HoverLeft: return L"hover target left";
    case PreviewCloseReason::HoverTargetChanged: return L"hover target changed";
    case PreviewCloseReason::SelectionChanged: return L"selection changed";
    case PreviewCloseReason::ForegroundLost: return L"foreground lost";
    case PreviewCloseReason::Escape: return L"escape";
    case PreviewCloseReason::TrayCommand: return L"tray command";
    case PreviewCloseReason::Disabled: return L"disabled";
    case PreviewCloseReason::SettingsChanged: return L"settings changed";
    case PreviewCloseReason::DirectOpenReplaced: return L"direct open replaced current preview";
    case PreviewCloseReason::ExternalHoverReplaced: return L"external hover replaced current preview";
    case PreviewCloseReason::Reload: return L"reload";
    case PreviewCloseReason::Shutdown: return L"shutdown";
    default: return L"unknown";
    }
}

MediaKind DetectMediaKind(const std::wstring& path);


struct ExternalHoverRequest {
    std::wstring action;
    std::wstring requestId;
    unsigned long long generation = 0;
    DWORD sourcePid = 0;
    std::vector<std::wstring> paths;
    std::wstring audiblePath;
    int volumePercent = 0;
    RECT geometry{ 0, 0, 640, 360 };
    POINT cursor{ 0, 0 };
    std::wstring anchorMode;
};

void SkipJsonSpace(const std::wstring& text, size_t& pos) {
    while (pos < text.size() && iswspace(text[pos])) ++pos;
}

bool ParseJsonStringAt(const std::wstring& text, size_t& pos, std::wstring& out) {
    SkipJsonSpace(text, pos);
    if (pos >= text.size() || text[pos] != L'"') return false;
    ++pos;
    out.clear();
    while (pos < text.size()) {
        wchar_t ch = text[pos++];
        if (ch == L'"') return true;
        if (ch != L'\\') { out.push_back(ch); continue; }
        if (pos >= text.size()) return false;
        wchar_t esc = text[pos++];
        switch (esc) {
        case L'"': out.push_back(L'"'); break;
        case L'\\': out.push_back(L'\\'); break;
        case L'/': out.push_back(L'/'); break;
        case L'b': out.push_back(L'\b'); break;
        case L'f': out.push_back(L'\f'); break;
        case L'n': out.push_back(L'\n'); break;
        case L'r': out.push_back(L'\r'); break;
        case L't': out.push_back(L'\t'); break;
        case L'u': {
            if (pos + 4 > text.size()) return false;
            unsigned value = 0;
            for (int i = 0; i < 4; ++i) {
                wchar_t h = text[pos++];
                value <<= 4;
                if (h >= L'0' && h <= L'9') value += h - L'0';
                else if (h >= L'a' && h <= L'f') value += 10 + h - L'a';
                else if (h >= L'A' && h <= L'F') value += 10 + h - L'A';
                else return false;
            }
            out.push_back(static_cast<wchar_t>(value));
            break;
        }
        default: return false;
        }
    }
    return false;
}

std::optional<size_t> FindJsonValue(const std::wstring& text, const std::wstring& key) {
    std::wstring token = L"\"" + key + L"\"";
    size_t pos = text.find(token);
    if (pos == std::wstring::npos) return std::nullopt;
    pos = text.find(L':', pos + token.size());
    if (pos == std::wstring::npos) return std::nullopt;
    ++pos;
    SkipJsonSpace(text, pos);
    return pos;
}

bool JsonString(const std::wstring& text, const std::wstring& key, std::wstring& out) {
    auto found = FindJsonValue(text, key);
    if (!found) return false;
    size_t pos = *found;
    return ParseJsonStringAt(text, pos, out);
}

bool JsonInt64(const std::wstring& text, const std::wstring& key, long long& out) {
    auto found = FindJsonValue(text, key);
    if (!found) return false;
    size_t pos = *found;
    bool negative = false;
    if (pos < text.size() && text[pos] == L'-') { negative = true; ++pos; }
    if (pos >= text.size() || !iswdigit(text[pos])) return false;
    unsigned long long value = 0;
    while (pos < text.size() && iswdigit(text[pos])) {
        value = value * 10 + static_cast<unsigned>(text[pos++] - L'0');
        if (value > 0x7fffffffffffffffULL) return false;
    }
    out = negative ? -static_cast<long long>(value) : static_cast<long long>(value);
    return true;
}

bool JsonStringArray(const std::wstring& text, const std::wstring& key, std::vector<std::wstring>& out) {
    auto found = FindJsonValue(text, key);
    if (!found) return false;
    size_t pos = *found;
    if (pos >= text.size() || text[pos] != L'[') return false;
    ++pos;
    out.clear();
    while (true) {
        SkipJsonSpace(text, pos);
        if (pos >= text.size()) return false;
        if (text[pos] == L']') return true;
        std::wstring value;
        if (!ParseJsonStringAt(text, pos, value)) return false;
        out.push_back(std::move(value));
        SkipJsonSpace(text, pos);
        if (pos < text.size() && text[pos] == L',') { ++pos; continue; }
        if (pos < text.size() && text[pos] == L']') return true;
        return false;
    }
}

bool ParseExternalHoverJson(const std::wstring& json, ExternalHoverRequest& out, std::wstring& error) {
    std::wstring protocol;
    long long version = 0, generation = 0, sourcePid = 0;
    if (!JsonString(json, L"protocol", protocol) || protocol != L"mvview-hover") { error = L"invalid protocol"; return false; }
    if (!JsonInt64(json, L"version", version) || version != MVVIEW_HOVER_PROTOCOL_VERSION) { error = L"unsupported version"; return false; }
    if (!JsonString(json, L"action", out.action)) { error = L"missing action"; return false; }
    if (out.action != L"hover_open" && out.action != L"hover_close" && out.action != L"hover_move" && out.action != L"hover_update") { error = L"unknown action"; return false; }
    if (!JsonString(json, L"request_id", out.requestId) || out.requestId.empty()) { error = L"missing request_id"; return false; }
    if (!JsonInt64(json, L"generation", generation) || generation < 0) { error = L"invalid generation"; return false; }
    if (!JsonInt64(json, L"source_pid", sourcePid) || sourcePid <= 0 || sourcePid > 0xffffffffLL) { error = L"invalid source_pid"; return false; }
    out.generation = static_cast<unsigned long long>(generation);
    out.sourcePid = static_cast<DWORD>(sourcePid);
    if (out.action == L"hover_close") return true;
    if (!JsonStringArray(json, L"paths", out.paths) || out.paths.empty()) { error = L"missing paths"; return false; }
    JsonString(json, L"audible_path", out.audiblePath);
    JsonString(json, L"anchor_mode", out.anchorMode);
    long long value = 0;
    if (JsonInt64(json, L"volume_percent", value)) out.volumePercent = std::clamp<int>(static_cast<int>(value), 0, 100);
    long long x=0,y=0,w=640,h=360,cx=0,cy=0;
    JsonInt64(json,L"x",x); JsonInt64(json,L"y",y); JsonInt64(json,L"width",w); JsonInt64(json,L"height",h); JsonInt64(json,L"cursor_x",cx); JsonInt64(json,L"cursor_y",cy);
    out.geometry = { static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + std::clamp<long long>(w, 160, 8192)), static_cast<LONG>(y + std::clamp<long long>(h, 90, 8192)) };
    out.cursor = { static_cast<LONG>(cx), static_cast<LONG>(cy) };
    std::vector<std::wstring> valid;
    for (const auto& path : out.paths) if (FileExists(path) && DetectMediaKind(path) != MediaKind::None) valid.push_back(path);
    out.paths.swap(valid);
    if (out.paths.empty()) { error = L"no valid media paths"; return false; }
    return true;
}

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
    int previewDelayMs = 1200; // legacy compatibility only
    int hoverPreviewDelayMs = 800;
    bool hoverPreviewEnabled = true;
    bool stopImmediatelyOnHoverLeave = true;
    bool confirmMultipleSelection = true;
    bool allowPointerOverPreview = true;
    int previewResolutionP = 360;
    int cursorFarClosePx = 220; // retained for backward-compatible settings files
    bool closeWhenForegroundLost = true;
    bool closeWhenSelectionEmpty = true;
    bool closeOnNonMedia = true;
    bool audioOnStart = false;
    int volumePercent = 50;
    int borderColorIndex = 0;
    bool registerEscapeWhilePreviewVisible = true;

    static bool HasKey(const std::wstring& body, const std::wstring& key) {
        return body.find(L"\"" + key + L"\"") != std::wstring::npos;
    }

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
        hoverPreviewDelayMs = std::clamp(hoverPreviewDelayMs, 0, 5000);
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
        case 1: return RGB(96, 238, 255);
        case 2: return RGB(126, 255, 126);
        case 3: return RGB(255, 170, 72);
        case 4: return RGB(255, 128, 196);
        case 5: return RGB(235, 235, 235);
        case 6: return RGB(160, 140, 255);
        default: return RGB(255, 232, 92);
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
        if (HasKey(body, L"hoverPreviewDelayMs")) {
            hoverPreviewDelayMs = ReadInt(body, L"hoverPreviewDelayMs", hoverPreviewDelayMs);
        } else if (HasKey(body, L"previewDelayMs")) {
            hoverPreviewDelayMs = previewDelayMs;
            Log(L"Settings: migrated previewDelayMs to hoverPreviewDelayMs");
        }
        hoverPreviewEnabled = ReadBool(body, L"hoverPreviewEnabled", hoverPreviewEnabled);
        stopImmediatelyOnHoverLeave = ReadBool(body, L"stopImmediatelyOnHoverLeave", stopImmediatelyOnHoverLeave);
        confirmMultipleSelection = ReadBool(body, L"confirmMultipleSelection", confirmMultipleSelection);
        allowPointerOverPreview = ReadBool(body, L"allowPointerOverPreview", allowPointerOverPreview);
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
        Save(); // persist migrated/new keys once
    }

    void Save() const {
        EnsureDir(AppDataDir());
        std::wstring settingsPath = SettingsPath();
        std::wofstream fs(settingsPath.c_str(), std::ios::trunc);
        if (!fs) return;
        fs << L"{\n";
        fs << L"  \"enabled\": " << (enabled ? L"true" : L"false") << L",\n";
        fs << L"  \"hoverPreviewEnabled\": " << (hoverPreviewEnabled ? L"true" : L"false") << L",\n";
        fs << L"  \"hoverPreviewDelayMs\": " << hoverPreviewDelayMs << L",\n";
        fs << L"  \"previewDelayMs\": " << hoverPreviewDelayMs << L",\n";
        fs << L"  \"stopImmediatelyOnHoverLeave\": " << (stopImmediatelyOnHoverLeave ? L"true" : L"false") << L",\n";
        fs << L"  \"confirmMultipleSelection\": " << (confirmMultipleSelection ? L"true" : L"false") << L",\n";
        fs << L"  \"allowPointerOverPreview\": " << (allowPointerOverPreview ? L"true" : L"false") << L",\n";
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
    ComPtr(ComPtr&& other) noexcept : p(other.p) { other.p = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        // ComPtr defines operator&() for COM out-parameters, so using &other
        // here would return T** instead of the actual ComPtr address.
        if (this != std::addressof(other)) {
            reset();
            p = other.p;
            other.p = nullptr;
        }
        return *this;
    }
    T** operator&() { reset(); return &p; }
    T* operator->() const { return p; }
    operator bool() const { return p != nullptr; }
    T* get() const { return p; }
    void reset(T* v = nullptr) { if (p) p->Release(); p = v; }
};

struct ShellViewContext {
    long candidateIndex = -1;
    HWND explorerRoot = nullptr;
    HWND viewHwnd = nullptr;
    std::wstring locationName;
    std::wstring locationUrl;
    std::wstring tabKey;
    int score = 0;
    ComPtr<IShellView> shellView;
    ComPtr<IFolderView2> folderView;
};

struct HoverTarget {
    std::wstring path;
    std::wstring key;
    std::wstring displayName;
    RECT itemRect{};
    HWND explorerRoot = nullptr;
    HWND viewHwnd = nullptr;
    std::wstring tabKey;
    MediaKind kind = MediaKind::None;

    bool IsValid() const {
        return !path.empty() && kind != MediaKind::None && explorerRoot != nullptr;
    }
};

class ExplorerReader {
    struct CachedShellItem {
        std::vector<std::wstring> normalizedNames;
        std::wstring path;
    };

    ComPtr<IUIAutomation> automation_;
    std::wstring lastLoggedTabKey_;
    std::wstring lastFailure_;
    ULONGLONG lastFailureTick_ = 0;
    std::wstring itemCacheTabKey_;
    ULONGLONG itemCacheTick_ = 0;
    std::vector<CachedShellItem> itemCache_;

public:
    bool IsExplorerWindow(HWND hwnd) const {
        HWND root = GetAncestor(hwnd, GA_ROOT);
        wchar_t cls[256]{};
        GetClassNameW(root ? root : hwnd, cls, 256);
        std::wstring c = cls;
        return c == L"CabinetWClass" || c == L"ExploreWClass";
    }

    bool ResolveHoveredItem(HWND foreground, POINT screenPoint, HoverTarget& target) {
        target = {};
        if (!foreground || !IsExplorerWindow(foreground)) return false;
        HWND root = GetAncestor(foreground, GA_ROOT);
        HWND pointWindow = WindowFromPoint(screenPoint);
        if (!pointWindow || GetAncestor(pointWindow, GA_ROOT) != root) return false;
        if (!PointIsInShellFileView(pointWindow, root)) return false;

        ShellViewContext context;
        if (!ResolveActiveTab(foreground, &screenPoint, context, false)) {
            LogResolutionFailure(L"Shell active tab could not be resolved");
            return false;
        }
        if (context.viewHwnd && pointWindow != context.viewHwnd && !IsChild(context.viewHwnd, pointWindow)) {
            LogResolutionFailure(L"Pointer is not inside the active Shell View");
            return false;
        }

        ComPtr<IUIAutomationElement> itemElement;
        std::wstring displayName;
        RECT itemRect{};
        if (!ResolveUiaItem(screenPoint, root, itemElement, displayName, itemRect)) return false;

        std::wstring path;
        if (!MapDisplayNameToUniqueShellItem(context, displayName, path)) return false;
        MediaKind kind = DetectMediaKind(path);
        if (kind == MediaKind::None || !FileExists(path)) return false;

        target.path = path;
        target.key = ToLower(path);
        target.displayName = displayName;
        target.itemRect = itemRect;
        target.explorerRoot = root;
        target.viewHwnd = context.viewHwnd;
        target.tabKey = context.tabKey;
        target.kind = kind;
        return true;
    }

    std::vector<std::wstring> GetSelectedPathsForForeground(
        HWND foreground,
        std::wstring* tabKey = nullptr,
        HWND* explorerRoot = nullptr,
        bool forceCandidateLog = false) {
        std::vector<std::wstring> result;
        if (tabKey) tabKey->clear();
        if (explorerRoot) *explorerRoot = nullptr;
        if (!foreground || !IsExplorerWindow(foreground)) return result;

        ShellViewContext context;
        if (!ResolveActiveTab(foreground, nullptr, context, forceCandidateLog)) return result;
        if (tabKey) *tabKey = context.tabKey;
        if (explorerRoot) *explorerRoot = context.explorerRoot;
        return GetPathsFromView(context, SVGIO_SELECTION);
    }

private:
    bool EnsureAutomation() {
        if (automation_) return true;
        HRESULT hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation_.p));
        if (FAILED(hr) || !automation_) {
            hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation_.p));
        }
        if (FAILED(hr) || !automation_) {
            LogResolutionFailure(L"UI Automation initialization failed: HRESULT=" + std::to_wstring((long)hr));
            return false;
        }
        Log(L"ExplorerReader: UI Automation initialized");
        return true;
    }

    void LogResolutionFailure(const std::wstring& text) {
        ULONGLONG now = GetTickCount64();
        if (text != lastFailure_ || now - lastFailureTick_ >= 2000) {
            lastFailure_ = text;
            lastFailureTick_ = now;
            Log(L"Explorer hover resolve failed: " + text);
        }
    }

    static bool PointIsInShellFileView(HWND pointWindow, HWND root) {
        bool shellDefViewFound = false;
        for (HWND current = pointWindow; current; current = GetParent(current)) {
            wchar_t cls[256]{};
            GetClassNameW(current, cls, 256);
            if (wcscmp(cls, L"SHELLDLL_DefView") == 0) shellDefViewFound = true;
            if (current == root) break;
        }
        return shellDefViewFound;
    }

    bool ResolveUiaItem(
        POINT screenPoint,
        HWND explorerRoot,
        ComPtr<IUIAutomationElement>& itemElement,
        std::wstring& displayName,
        RECT& itemRect) {
        if (!EnsureAutomation()) return false;
        ComPtr<IUIAutomationElement> current;
        HRESULT hr = automation_->ElementFromPoint(screenPoint, &current.p);
        if (FAILED(hr) || !current) {
            LogResolutionFailure(L"UI Automation ElementFromPoint failed");
            return false;
        }

        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation_->get_ControlViewWalker(&walker.p)) || !walker) {
            LogResolutionFailure(L"UI Automation ControlViewWalker unavailable");
            return false;
        }

        for (int depth = 0; depth < 14 && current; ++depth) {
            CONTROLTYPEID controlType = 0;
            current->get_CurrentControlType(&controlType);
            if (controlType == UIA_TreeItemControlTypeId) return false;
            if (controlType == UIA_DataItemControlTypeId || controlType == UIA_ListItemControlTypeId) {
                RECT bounds{};
                if (FAILED(current->get_CurrentBoundingRectangle(&bounds)) || IsRectEmpty(&bounds) || !::PtInRect(&bounds, screenPoint)) {
                    LogResolutionFailure(L"UI Automation item has invalid bounds");
                    return false;
                }
                BSTR name = nullptr;
                if (FAILED(current->get_CurrentName(&name)) || !name) {
                    LogResolutionFailure(L"UI Automation item name unavailable");
                    return false;
                }
                displayName = Trim(name);
                SysFreeString(name);
                if (displayName.empty()) return false;

                UIA_HWND nativeHwnd = 0;
                current->get_CurrentNativeWindowHandle(&nativeHwnd);
                if (nativeHwnd && GetAncestor(reinterpret_cast<HWND>((INT_PTR)nativeHwnd), GA_ROOT) != explorerRoot) return false;
                itemRect = bounds;
                itemElement.reset(current.p);
                current.p = nullptr;
                return true;
            }

            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(current.get(), &parent.p)) || !parent) break;
            current.reset(parent.p);
            parent.p = nullptr;
        }
        return false;
    }

    bool ResolveActiveTab(HWND foreground, const POINT* screenPoint, ShellViewContext& out, bool forceLog) {
        out = ShellViewContext{};
        HWND root = GetAncestor(foreground, GA_ROOT);
        if (!root || !IsExplorerWindow(root)) return false;
        const std::wstring activeTitle = WindowTextOf(root);
        HWND pointWindow = screenPoint ? WindowFromPoint(*screenPoint) : nullptr;

        ComPtr<IShellWindows> shellWindows;
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows.p));
        if (FAILED(hr) || !shellWindows) {
            LogResolutionFailure(L"CoCreateInstance(IShellWindows) failed");
            return false;
        }

        long count = 0;
        shellWindows->get_Count(&count);
        std::vector<std::wstring> candidateLogs;
        bool found = false;
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
            if (webRoot != root && (HWND)webHwnd != root) continue;

            ShellViewContext candidate;
            candidate.candidateIndex = i;
            candidate.explorerRoot = root;
            BSTR value = nullptr;
            if (SUCCEEDED(web->get_LocationName(&value)) && value) {
                candidate.locationName = Trim(value);
                SysFreeString(value);
            }
            value = nullptr;
            if (SUCCEEDED(web->get_LocationURL(&value)) && value) {
                candidate.locationUrl = Trim(value);
                SysFreeString(value);
            }

            ComPtr<::IServiceProvider> provider;
            if (FAILED(web->QueryInterface(IID_PPV_ARGS(&provider.p))) || !provider) continue;
            ComPtr<IShellBrowser> browser;
            if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser.p))) || !browser) continue;
            if (FAILED(browser->QueryActiveShellView(&candidate.shellView.p)) || !candidate.shellView) continue;
            candidate.shellView->GetWindow(&candidate.viewHwnd);
            if (FAILED(candidate.shellView->QueryInterface(IID_PPV_ARGS(&candidate.folderView.p))) || !candidate.folderView) continue;

            if (candidate.viewHwnd && IsWindowVisible(candidate.viewHwnd)) candidate.score += 5000;
            if (screenPoint && pointWindow && candidate.viewHwnd &&
                (pointWindow == candidate.viewHwnd || IsChild(candidate.viewHwnd, pointWindow))) candidate.score += 4000;
            if (!candidate.locationName.empty() && ContainsInsensitive(activeTitle, candidate.locationName)) candidate.score += 1000;
            if (!candidate.locationUrl.empty() && ContainsInsensitive(activeTitle, candidate.locationUrl)) candidate.score += 300;

            candidate.tabKey = ToLower(candidate.locationUrl) + L"|" +
                std::to_wstring((uintptr_t)candidate.viewHwnd) + L"|" + std::to_wstring(candidate.candidateIndex);
            candidateLogs.push_back(L"candidate[" + std::to_wstring(i) + L"] location=" + candidate.locationName +
                L" view=" + std::to_wstring((uintptr_t)candidate.viewHwnd) +
                L" visible=" + (candidate.viewHwnd && IsWindowVisible(candidate.viewHwnd) ? L"true" : L"false") +
                L" score=" + std::to_wstring(candidate.score));

            if (!found || candidate.score > out.score) {
                out = std::move(candidate);
                found = true;
            }
        }

        if (!found) {
            LogResolutionFailure(L"No Explorer Shell View candidate matched the foreground tab");
            return false;
        }
        if (forceLog || out.tabKey != lastLoggedTabKey_) {
            Log(L"ExplorerReader: foreground title=" + activeTitle);
            for (const auto& line : candidateLogs) Log(L"ExplorerReader: " + line);
            Log(L"ExplorerReader: active tab candidate=" + std::to_wstring(out.candidateIndex) +
                L" tabKey=" + out.tabKey);
            lastLoggedTabKey_ = out.tabKey;
        }
        return true;
    }

    std::vector<std::wstring> GetPathsFromView(const ShellViewContext& context, UINT flags) {
        std::vector<std::wstring> paths;
        if (!context.folderView) return paths;
        ComPtr<IShellItemArray> items;
        if (FAILED(context.folderView->Items(flags, IID_PPV_ARGS(&items.p))) || !items) return paths;
        DWORD count = 0;
        if (FAILED(items->GetCount(&count))) return paths;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(i, &item.p)) || !item) continue;
            PWSTR rawPath = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                std::wstring path = Trim(rawPath);
                CoTaskMemFree(rawPath);
                if (!path.empty() && FileExists(path)) paths.push_back(path);
            }
        }
        return paths;
    }

    static void AddDisplayName(IShellItem* item, SIGDN sigdn, std::vector<std::wstring>& names) {
        if (!item) return;
        PWSTR raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(sigdn, &raw)) && raw) {
            std::wstring name = Trim(raw);
            CoTaskMemFree(raw);
            if (!name.empty()) names.push_back(name);
        }
    }

    bool RebuildItemCache(const ShellViewContext& context) {
        const bool tabChanged = itemCacheTabKey_ != context.tabKey;
        itemCache_.clear();
        itemCacheTabKey_ = context.tabKey;
        itemCacheTick_ = GetTickCount64();
        if (!context.folderView) return false;

        ComPtr<IShellItemArray> items;
        if (FAILED(context.folderView->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items.p))) || !items) {
            LogResolutionFailure(L"IFolderView2::Items(SVGIO_ALLVIEW) failed");
            return false;
        }
        DWORD count = 0;
        if (FAILED(items->GetCount(&count))) return false;
        itemCache_.reserve(count);
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(i, &item.p)) || !item) continue;

            PWSTR rawPath = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) continue;
            std::wstring candidatePath = Trim(rawPath);
            CoTaskMemFree(rawPath);
            if (candidatePath.empty() || !FileExists(candidatePath)) continue;

            std::vector<std::wstring> displayNames;
            AddDisplayName(item.get(), SIGDN_NORMALDISPLAY, displayNames);
            AddDisplayName(item.get(), SIGDN_PARENTRELATIVEEDITING, displayNames);
            AddDisplayName(item.get(), SIGDN_PARENTRELATIVEPARSING, displayNames);
            CachedShellItem cached;
            cached.path = std::move(candidatePath);
            for (const auto& display : displayNames) {
                std::wstring normalized = ToLower(Trim(display));
                if (!normalized.empty() &&
                    std::find(cached.normalizedNames.begin(), cached.normalizedNames.end(), normalized) == cached.normalizedNames.end()) {
                    cached.normalizedNames.push_back(std::move(normalized));
                }
            }
            if (!cached.normalizedNames.empty()) itemCache_.push_back(std::move(cached));
        }
        if (tabChanged) {
            Log(L"ExplorerReader: Shell item cache rebuilt: tab=" + context.tabKey +
                L" count=" + std::to_wstring(itemCache_.size()));
        }
        return true;
    }

    bool MapDisplayNameToUniqueShellItem(
        const ShellViewContext& context,
        const std::wstring& uiaName,
        std::wstring& path) {
        path.clear();
        const ULONGLONG now = GetTickCount64();
        if (itemCacheTabKey_ != context.tabKey || itemCache_.empty() || now - itemCacheTick_ >= 2000) {
            if (!RebuildItemCache(context)) return false;
        }

        const std::wstring wanted = ToLower(Trim(uiaName));
        std::vector<std::wstring> matches;
        for (const auto& item : itemCache_) {
            if (std::find(item.normalizedNames.begin(), item.normalizedNames.end(), wanted) != item.normalizedNames.end())
                matches.push_back(item.path);
        }
        std::sort(matches.begin(), matches.end(), [](const std::wstring& a, const std::wstring& b) {
            return ToLower(a) < ToLower(b);
        });
        matches.erase(std::unique(matches.begin(), matches.end(), [](const std::wstring& a, const std::wstring& b) {
            return ToLower(a) == ToLower(b);
        }), matches.end());
        if (matches.size() != 1) {
            LogResolutionFailure(L"Shell item mapping was not unique for UIA name '" + uiaName +
                L"' (matches=" + std::to_wstring(matches.size()) + L")");
            return false;
        }
        path = matches[0];
        return true;
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

bool PointInTransferBridge(const RECT& from, const RECT& to, POINT pt, LONG margin = 8) {
    RECT bridge{};
    if (from.right <= to.left || to.right <= from.left) {
        const bool toRight = from.right <= to.left;
        bridge.left = (toRight ? from.right : to.right) - margin;
        bridge.right = (toRight ? to.left : from.left) + margin;
        bridge.top = std::max<LONG>(from.top, to.top) - margin;
        bridge.bottom = std::min<LONG>(from.bottom, to.bottom) + margin;
    } else if (from.bottom <= to.top || to.bottom <= from.top) {
        const bool toBelow = from.bottom <= to.top;
        bridge.top = (toBelow ? from.bottom : to.bottom) - margin;
        bridge.bottom = (toBelow ? to.top : from.top) + margin;
        bridge.left = std::max<LONG>(from.left, to.left) - margin;
        bridge.right = std::min<LONG>(from.right, to.right) + margin;
    } else {
        return false;
    }
    return bridge.left < bridge.right && bridge.top < bridge.bottom && PointInRect(bridge, pt);
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

    RECT ClampRectToMonitor(RECT wanted, POINT monitorPoint) {
        HMONITOR mon = MonitorFromPoint(monitorPoint, MONITOR_DEFAULTTONEAREST);
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
        wanted = ClampRectToMonitor(wanted, anchor);
        POINT topLeft{ wanted.left, wanted.top };
        return ShowPathAt(path, kind, topLeft, anchor);
    }

    bool ShowPathNearItem(const std::wstring& path, MediaKind kind, const RECT& itemRect, POINT cursor) {
        const int width = WindowWidth();
        const int height = WindowHeight();
        const int gap = 14;
        POINT center{ (itemRect.left + itemRect.right) / 2, (itemRect.top + itemRect.bottom) / 2 };
        HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        const RECT work = mi.rcWork;
        const POINT candidates[] = {
            { itemRect.right + gap, itemRect.top },
            { itemRect.left - width - gap, itemRect.top },
            { itemRect.left, itemRect.bottom + gap },
            { itemRect.left, itemRect.top - height - gap }
        };
        for (POINT topLeft : candidates) {
            RECT candidate{ topLeft.x, topLeft.y, topLeft.x + width, topLeft.y + height };
            RECT intersection{};
            if (candidate.left >= work.left && candidate.top >= work.top &&
                candidate.right <= work.right && candidate.bottom <= work.bottom &&
                !IntersectRect(&intersection, &candidate, &itemRect)) {
                return ShowPathAt(path, kind, topLeft, cursor);
            }
        }
        RECT fallback{ itemRect.right + gap, itemRect.top, itemRect.right + gap + width, itemRect.top + height };
        fallback = ClampRectToMonitor(fallback, center);
        RECT intersection{};
        if (IntersectRect(&intersection, &fallback, &itemRect)) {
            fallback.left = std::max<LONG>(work.left + 8, itemRect.left - width - gap);
            fallback.right = fallback.left + width;
        }
        return ShowPathAt(path, kind, POINT{ fallback.left, fallback.top }, cursor);
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
        r = ClampRectToMonitor(r, logicalAnchor);
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

    bool ShowPathAtExplicit(const std::wstring& path, MediaKind kind, RECT requested, POINT logicalAnchor, bool allowAudio, int volumePercent, bool startPaused = false) {
        if (!hwnd_ || !host_ || !settings_) return false;
        currentPath_ = path;
        currentKind_ = kind;
        anchor_ = logicalAnchor;
        timePos_ = 0.0;
        duration_ = 0.0;
        volumePopupVisible_ = false;
        draggingVolume_ = false;
        const int width = std::clamp<int>(requested.right - requested.left, 160, 8192);
        const int height = std::clamp<int>(requested.bottom - requested.top, 90, 8192);
        RECT r{ requested.left, requested.top, requested.left + width, requested.top + height };
        r = ClampRectToMonitor(r, logicalAnchor);
        SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right-r.left, r.bottom-r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Layout(r.right-r.left, r.bottom-r.top);
        InvalidateRect(hwnd_, nullptr, TRUE);
        if (!player_.Initialize(hwnd_, host_)) { RegisterEscape(); return false; }
        player_.Stop();
        const bool loaded = player_.LoadFile(path, allowAudio && (kind == MediaKind::Audio || kind == MediaKind::Video), volumePercent, startPaused);
        Log(std::wstring(L"PreviewWindow: external load ") + (loaded ? L"ok: " : L"failed: ") + path);
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
        IDC_HOVER_DELAY = 3102,
        IDC_AUDIO = 3103,
        IDC_COLOR = 3104,
        IDC_HOVER_ENABLED = 3105,
        IDC_STOP_ON_LEAVE = 3106,
        IDC_CONFIRM_MULTI = 3107,
        IDC_ALLOW_PREVIEW_POINTER = 3108,
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
            CW_USEDEFAULT, CW_USEDEFAULT, 700, 650,
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
        if (darkBg_) DeleteObject(darkBg_);
        if (darkEditBg_) DeleteObject(darkEditBg_);
        if (uiFont_) DeleteObject(uiFont_);
        if (uiFontBold_ && uiFontBold_ != uiFont_) DeleteObject(uiFontBold_);
        darkBg_ = nullptr;
        darkEditBg_ = nullptr;
        uiFont_ = nullptr;
        uiFontBold_ = nullptr;
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
            return reinterpret_cast<LRESULT>(self->darkBg_ ? self->darkBg_ : (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(38, 38, 38));
            SetTextColor(hdc, RGB(245, 245, 245));
            return reinterpret_cast<LRESULT>(self->darkEditBg_ ? self->darkEditBg_ : (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_OK) { self->ApplyAndClose(); return 0; }
            if (LOWORD(wp) == IDC_CANCEL) { self->Close(false); return 0; }
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
        SendMessageW(label, WM_SETFONT, (WPARAM)(bold ? uiFontBold_ : uiFont_), TRUE);
        return label;
    }

    HWND AddCombo(int id, int x, int y, int w, int h) {
        HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, w, h, hwnd_, (HMENU)(INT_PTR)id, inst_, nullptr);
        SendMessageW(combo, WM_SETFONT, (WPARAM)uiFont_, TRUE);
        return combo;
    }

    void AddCheck(int id, const wchar_t* text, int x, int y, bool checked) {
        HWND check = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, 28, 30, hwnd_, (HMENU)(INT_PTR)id, inst_, nullptr);
        SendMessageW(check, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        AddLabel(text, x + 38, y + 1, 560, 30, true);
    }

    void CreateControls() {
        uiFont_ = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Meiryo UI");
        uiFontBold_ = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Meiryo UI");
        if (!uiFont_) uiFont_ = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        if (!uiFontBold_) uiFontBold_ = uiFont_;

        AddCheck(IDC_HOVER_ENABLED, L"ホバーでプレビュー", 28, 24, settings_->hoverPreviewEnabled);

        AddLabel(L"ホバー開始までの時間", 28, 76, 225, 30, true);
        HWND delay = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            270, 72, 110, 32, hwnd_, (HMENU)(INT_PTR)IDC_HOVER_DELAY, inst_, nullptr);
        wchar_t delayText[32]{};
        StringCchPrintfW(delayText, 32, L"%.1f", settings_->hoverPreviewDelayMs / 1000.0);
        SetWindowTextW(delay, delayText);
        SendMessageW(delay, WM_SETFONT, (WPARAM)uiFont_, TRUE);
        AddLabel(L"秒（0.0 ～ 5.0）", 394, 76, 220, 30);

        AddCheck(IDC_STOP_ON_LEAVE, L"対象から外れたらすぐ停止", 28, 124, settings_->stopImmediatelyOnHoverLeave);
        AddCheck(IDC_CONFIRM_MULTI, L"複数選択時に確認する", 28, 170, settings_->confirmMultipleSelection);
        AddCheck(IDC_ALLOW_PREVIEW_POINTER, L"プレビュー上への移動を許可", 28, 216, settings_->allowPointerOverPreview);
        AddCheck(IDC_AUDIO, L"プレビュー開始時に音を出す", 28, 262, settings_->audioOnStart);

        AddLabel(L"Preview 解像度", 28, 318, 190, 30, true);
        HWND res = AddCombo(IDC_RESOLUTION, 270, 314, 300, 190);
        const wchar_t* resolutions[] = { L"720p", L"480p", L"360p", L"240p", L"180p" };
        const int resValues[] = { 720, 480, 360, 240, 180 };
        for (auto text : resolutions) SendMessageW(res, CB_ADDSTRING, 0, (LPARAM)text);
        int resSel = 2;
        for (int i = 0; i < 5; ++i) if (settings_->previewResolutionP == resValues[i]) resSel = i;
        SendMessageW(res, CB_SETCURSEL, resSel, 0);

        AddLabel(L"プレビュー枠色", 28, 372, 190, 30, true);
        HWND color = AddCombo(IDC_COLOR, 270, 368, 300, 200);
        const wchar_t* colors[] = { L"明るい黄色", L"アクア", L"グリーン", L"オレンジ", L"ピンク", L"ホワイト", L"パープル" };
        for (auto text : colors) SendMessageW(color, CB_ADDSTRING, 0, (LPARAM)text);
        SendMessageW(color, CB_SETCURSEL, settings_->borderColorIndex, 0);

        AddLabel(L"Explorer のファイル一覧項目だけを対象にします。クリックや選択変更だけでは開始しません。",
            28, 438, 620, 58, false);

        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            410, 535, 100, 38, hwnd_, (HMENU)(INT_PTR)IDC_OK, inst_, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE,
            530, 535, 120, 38, hwnd_, (HMENU)(INT_PTR)IDC_CANCEL, inst_, nullptr);
        SendMessageW(ok, WM_SETFONT, (WPARAM)uiFontBold_, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    }

    void ApplyAndClose() {
        settings_->hoverPreviewEnabled = SendDlgItemMessageW(hwnd_, IDC_HOVER_ENABLED, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings_->stopImmediatelyOnHoverLeave = SendDlgItemMessageW(hwnd_, IDC_STOP_ON_LEAVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings_->confirmMultipleSelection = SendDlgItemMessageW(hwnd_, IDC_CONFIRM_MULTI, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings_->allowPointerOverPreview = SendDlgItemMessageW(hwnd_, IDC_ALLOW_PREVIEW_POINTER, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings_->audioOnStart = SendDlgItemMessageW(hwnd_, IDC_AUDIO, BM_GETCHECK, 0, 0) == BST_CHECKED;

        wchar_t buf[64]{};
        GetDlgItemTextW(hwnd_, IDC_HOVER_DELAY, buf, 64);
        double sec = _wtof(buf);
        sec = std::clamp(sec, 0.0, 5.0);
        settings_->hoverPreviewDelayMs = (int)(sec * 1000.0 + 0.5);
        settings_->previewDelayMs = settings_->hoverPreviewDelayMs;

        const int resValues[] = { 720, 480, 360, 240, 180 };
        int sel = (int)SendDlgItemMessageW(hwnd_, IDC_RESOLUTION, CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel > 4) sel = 2;
        settings_->previewResolutionP = resValues[sel];
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

class MultiConfirmWindow {
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HWND message_ = nullptr;
    HWND playButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HBRUSH darkBg_ = nullptr;
    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;

    enum : int { IDC_PLAY = IDOK, IDC_CANCEL = IDCANCEL };

public:
    ~MultiConfirmWindow() { Destroy(); }
    HWND Hwnd() const { return hwnd_; }
    bool IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    bool ContainsScreenPoint(POINT pt) const {
        if (!IsVisible()) return false;
        RECT rc{};
        GetWindowRect(hwnd_, &rc);
        return PointInRect(rc, pt);
    }

    RECT WindowRect() const {
        RECT rc{};
        if (IsVisible()) GetWindowRect(hwnd_, &rc);
        return rc;
    }

    bool Show(HINSTANCE inst, HWND owner, int count, const RECT& avoidRect, COLORREF borderColor) {
        inst_ = inst;
        owner_ = owner;
        if (!CreateIfNeeded()) return false;
        wchar_t text[160]{};
        StringCchPrintfW(text, 160, L"%d個のメディアをプレビューしますか？", count);
        SetWindowTextW(message_, text);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetPropW(hwnd_, L"MvView.BorderColor", (HANDLE)(ULONG_PTR)borderColor);

        const int width = 456;
        const int height = 176;
        POINT center{ (avoidRect.left + avoidRect.right) / 2, (avoidRect.top + avoidRect.bottom) / 2 };
        HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        RECT work = mi.rcWork;
        int x = avoidRect.right + 14;
        int y = avoidRect.top;
        if (x + width > work.right) x = avoidRect.left - width - 14;
        if (x < work.left) x = std::clamp(center.x - width / 2, work.left + 8, work.right - width - 8);
        if (y + height > work.bottom) y = work.bottom - height - 8;
        if (y < work.top) y = work.top + 8;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetFocus(playButton_);
        InvalidateRect(hwnd_, nullptr, TRUE);
        return true;
    }

    void Hide() {
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    }

    void Destroy() {
        if (hwnd_) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (darkBg_) DeleteObject(darkBg_);
        if (font_) DeleteObject(font_);
        if (boldFont_ && boldFont_ != font_) DeleteObject(boldFont_);
        darkBg_ = nullptr;
        font_ = nullptr;
        boldFont_ = nullptr;
    }

private:
    bool CreateIfNeeded() {
        if (hwnd_) return true;
        WNDCLASSW cls{};
        cls.lpfnWndProc = MultiConfirmWindow::WndProc;
        cls.hInstance = inst_;
        cls.lpszClassName = L"MvView.MultiConfirmWindow";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        cls.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
        RegisterClassW(&cls);
        darkBg_ = CreateSolidBrush(RGB(22, 22, 22));
        font_ = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Meiryo UI");
        boldFont_ = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Meiryo UI");
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
            cls.lpszClassName, L"",
            WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, 456, 176,
            owner_, nullptr, inst_, this);
        if (!hwnd_) return false;
        TryEnableDarkModeForWindow(hwnd_);
        message_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
            24, 30, 408, 44, hwnd_, nullptr, inst_, nullptr);
        playButton_ = CreateWindowExW(0, L"BUTTON", L"再生", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            124, 104, 92, 38, hwnd_, (HMENU)(INT_PTR)IDC_PLAY, inst_, nullptr);
        cancelButton_ = CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE,
            242, 104, 104, 38, hwnd_, (HMENU)(INT_PTR)IDC_CANCEL, inst_, nullptr);
        SendMessageW(message_, WM_SETFONT, (WPARAM)boldFont_, TRUE);
        SendMessageW(playButton_, WM_SETFONT, (WPARAM)boldFont_, TRUE);
        SendMessageW(cancelButton_, WM_SETFONT, (WPARAM)font_, TRUE);
        return true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        MultiConfirmWindow* self = reinterpret_cast<MultiConfirmWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<MultiConfirmWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_PLAY) {
                self->Hide();
                PostMessageW(self->owner_, WM_MV_MULTI_CONFIRM, 1, 0);
                return 0;
            }
            if (LOWORD(wp) == IDC_CANCEL) {
                self->Hide();
                PostMessageW(self->owner_, WM_MV_MULTI_CONFIRM, 0, 0);
                return 0;
            }
            break;
        case WM_CLOSE:
            self->Hide();
            PostMessageW(self->owner_, WM_MV_MULTI_CONFIRM, 0, 0);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(248, 248, 238));
            return reinterpret_cast<LRESULT>(self->darkBg_);
        }
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, self->darkBg_);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, self->darkBg_);
            COLORREF color = (COLORREF)(ULONG_PTR)GetPropW(hwnd, L"MvView.BorderColor");
            HPEN pen = CreatePen(PS_SOLID, 3, color ? color : RGB(255, 232, 92));
            HGDIOBJ old = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, 1, 1, rc.right - 1, rc.bottom - 1, 16, 16);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, old);
            DeleteObject(pen);
            EndPaint(hwnd, &ps);
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};


class SplashWindow {
    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT subFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HICON icon_ = nullptr;
    HBITMAP splashBitmap_ = nullptr;
    static constexpr UINT TIMER_SPLASH_CLOSE_LOCAL = 7201;

public:
    ~SplashWindow() {
        Close();
        if (titleFont_) DeleteObject(titleFont_);
        if (subFont_) DeleteObject(subFont_);
        if (smallFont_) DeleteObject(smallFont_);
        if (splashBitmap_) DeleteObject(splashBitmap_);
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

        const int w = 720;
        const int h = 405;
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
        if (!splashBitmap_) {
            splashBitmap_ = (HBITMAP)LoadImageW(inst_, MAKEINTRESOURCEW(IDB_SPLASH), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
            if (!splashBitmap_) Log(L"SplashWindow: splash bitmap load failed: " + FormatWin32Error(GetLastError()));
        }
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        EnsureFonts();
        if (splashBitmap_) {
            BITMAP bm{};
            GetObjectW(splashBitmap_, sizeof(bm), &bm);
            HDC memDc = CreateCompatibleDC(hdc);
            HGDIOBJ oldBitmap = SelectObject(memDc, splashBitmap_);
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, nullptr);
            StretchBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                memDc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(memDc, oldBitmap);
            DeleteDC(memDc);

            // The version remains live text rather than being baked into the image.
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 239, 94));
            HFONT oldVersionFont = (HFONT)SelectObject(hdc, smallFont_);
            RECT versionRc{ rc.right - 150, 20, rc.right - 28, 52 };
            std::wstring versionText = std::wstring(L"v") + kAppVersion;
            DrawTextW(hdc, versionText.c_str(), -1, &versionRc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(hdc, oldVersionFont);
            EndPaint(hwnd_, &ps);
            return;
        }

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

        SetBkMode(hdc, TRANSPARENT);
        if (icon_) DrawIconEx(hdc, 26, 34, icon_, 72, 72, 0, nullptr, DI_NORMAL);

        SetTextColor(hdc, RGB(255, 248, 150));
        HFONT oldFont = (HFONT)SelectObject(hdc, titleFont_);
        RECT titleRc{ 118, 36, rc.right - 24, 78 };
        DrawTextW(hdc, kAppDisplayName, -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

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
            MessageBoxW(owner_, msg.c_str(), MVVIEW_APP_DISPLAY_NAME_W L" tray error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
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
    MultiConfirmWindow multiConfirm_;
    TrayIcon tray_;
    SplashWindow splash_;
    HWINEVENTHOOK hookForeground_ = nullptr;
    HWINEVENTHOOK hookSelection_ = nullptr;
    HHOOK mouseHook_ = nullptr;

    PreviewTrigger previewTrigger_ = PreviewTrigger::None;
    HoverState hoverState_ = HoverState::Idle;
    HoverTarget hoverTarget_;
    POINT latestMousePoint_{};
    bool latestMousePointKnown_ = false;
    POINT hoverStartPosition_{};
    ULONGLONG hoverStartTick_ = 0;
    ULONGLONG hoverLeaveTick_ = 0;
    ULONGLONG exitGraceUntil_ = 0;
    ULONGLONG lastFullResolveTick_ = 0;
    ULONGLONG lastSuccessfulResolveTick_ = 0;
    ULONGLONG lastSelectionValidationTick_ = 0;
    ULONGLONG confirmMoveGraceUntil_ = 0;
    POINT lastFullResolvePoint_{};
    bool lastFullResolvePointKnown_ = false;
    bool forceHoverResolve_ = true;

    HWND currentExplorerRoot_ = nullptr;
    std::wstring currentTabKey_;
    std::vector<std::pair<std::wstring, MediaKind>> pendingMultiMedia_;
    std::vector<std::pair<std::wstring, MediaKind>> playingMedia_;
    std::wstring pendingSelectionKey_;
    std::wstring playingSelectionKey_;
    std::wstring suppressedConfirmationKey_;
    bool confirmationRequiresLeave_ = false;

    bool comReady_ = false;
    bool mpvRuntimeReady_ = false;
    std::wstring mpvRuntimeStatus_;
    UINT taskbarCreatedMsg_ = 0;
    DWORD externalHoverSourcePid_ = 0;
    unsigned long long externalHoverGeneration_ = 0;
    std::wstring externalHoverRequestId_;

public:
    int Run(HINSTANCE inst, int, const std::wstring& initialOpenPath = L"") {
        inst_ = inst;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        TryEnableAppDarkMode();
        taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comReady_ = SUCCEEDED(hr);
        if (hr == RPC_E_CHANGED_MODE) Log(L"COM was already initialized with a different apartment model");
        else if (FAILED(hr)) Log(L"CoInitializeEx failed: HRESULT=" + std::to_wstring((long)hr));
        settings_.Load();
        Log(std::wstring(kAppDisplayName) + L" started");
        Log(L"EXE path: " + GetModulePathText());
        splash_.Create(inst_);

        if (!CreateMainWindow()) {
            DWORD err = GetLastError();
            Log(L"CreateMainWindow failed: " + FormatWin32Error(err));
            splash_.Close();
            MessageBoxW(nullptr, (L"MvView main window creation failed.\n\n" + FormatWin32Error(err) + L"\n\nLog: " + LogPath()).c_str(),
                MVVIEW_APP_DISPLAY_NAME_W L" startup error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return 1;
        }
        if (!tray_.Add(hwnd_, inst_, MVVIEW_APP_DISPLAY_NAME_W L" / starting")) {
            Log(L"startup aborted because tray icon could not be added");
            splash_.Close();
            return 1;
        }
        if (!preview_.Create(inst_, &settings_)) Log(L"PreviewWindow.Create failed: " + FormatWin32Error(GetLastError()));

        mpvRuntimeReady_ = ProbeMpvRuntime(&mpvRuntimeStatus_);
        Log(L"runtime status: " + mpvRuntimeStatus_);
        tray_.UpdateTip(MpvTrayTip());
        InstallHooks();
        BeginUpdateCheck();

        if (!initialOpenPath.empty()) {
            OpenExternalPath(initialOpenPath);
        } else if (settings_.enabled && settings_.hoverPreviewEnabled) {
            // A foreground/focus change alone must not start preview. Capture the
            // current point and wait for an actual mouse movement notification.
            GetCursorPos(&latestMousePoint_);
            latestMousePointKnown_ = true;
            forceHoverResolve_ = true;
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (multiConfirm_.IsVisible() && IsDialogMessageW(multiConfirm_.Hwnd(), &msg)) continue;
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
        ClosePreview(PreviewCloseReason::DirectOpenReplaced);
        POINT pt{};
        GetCursorPos(&pt);
        Log(L"external open: " + path);
        if (preview_.ShowPath(path, kind, pt)) {
            previewTrigger_ = PreviewTrigger::DirectOpen;
            hoverState_ = HoverState::PlayingSingle;
            playingMedia_ = { { path, kind } };
            SetTimer(hwnd_, TIMER_CURSOR_CLOSE, 200, nullptr);
        }
    }

    bool IsExternalHoverCurrent(const ExternalHoverRequest& request) const {
        if (externalHoverSourcePid_ == 0) return true;
        if (request.sourcePid != externalHoverSourcePid_) return request.generation > externalHoverGeneration_;
        return request.generation >= externalHoverGeneration_;
    }

    void OpenExternalHover(const ExternalHoverRequest& request) {
        if (!IsExternalHoverCurrent(request)) { Log(L"external hover ignored: stale open"); return; }
        ClosePreview(PreviewCloseReason::ExternalHoverReplaced);
        externalHoverSourcePid_ = request.sourcePid;
        externalHoverGeneration_ = request.generation;
        externalHoverRequestId_ = request.requestId;
        std::vector<std::pair<std::wstring, MediaKind>> media;
        for (const auto& path : request.paths) {
            MediaKind kind = DetectMediaKind(path);
            if (kind != MediaKind::None) media.push_back({path, kind});
            if (media.size() >= 9) break;
        }
        if (media.empty()) return;
        const int count = static_cast<int>(media.size());
        const int cols = count <= 3 ? count : (count <= 4 ? 2 : 3);
        const int rows = (count + cols - 1) / cols;
        const int gap = count > 1 ? 8 : 0;
        const int totalW = std::max<int>(160, request.geometry.right-request.geometry.left);
        const int totalH = std::max<int>(90, request.geometry.bottom-request.geometry.top);
        const int cellW = std::max<int>(160, (totalW - (cols-1)*gap) / std::max(1, cols));
        const int cellH = std::max<int>(90, (totalH - (rows-1)*gap) / std::max(1, rows));
        preview_.Hide(); CloseExtraPreviews();
        for (int i=0; i<count; ++i) {
            PreviewWindow* window = nullptr;
            if (i == 0) window = &preview_;
            else { auto extra=std::make_unique<PreviewWindow>(); if (!extra->Create(inst_, &settings_)) continue; window=extra.get(); extraPreviews_.push_back(std::move(extra)); }
            int col=i%cols, row=i/cols;
            RECT cell{ request.geometry.left + col*(cellW+gap), request.geometry.top + row*(cellH+gap), request.geometry.left + col*(cellW+gap)+cellW, request.geometry.top + row*(cellH+gap)+cellH };
            const bool audible = !request.audiblePath.empty() && _wcsicmp(request.audiblePath.c_str(), media[i].first.c_str()) == 0;
            window->ShowPathAtExplicit(media[i].first, media[i].second, cell, request.cursor, audible, request.volumePercent, count > 1);
        }
        if (count > 1) { preview_.StartPlayback(); for (auto& w:extraPreviews_) if(w) w->StartPlayback(); }
        previewTrigger_ = PreviewTrigger::ExternalHover;
        hoverState_ = count > 1 ? HoverState::PlayingMulti : HoverState::PlayingSingle;
        playingMedia_ = media;
        SetTimer(hwnd_, TIMER_CURSOR_CLOSE, 200, nullptr);
        Log(L"external hover open request=" + request.requestId + L" generation=" + std::to_wstring(request.generation));
    }

    void CloseExternalHover(const ExternalHoverRequest& request) {
        if (previewTrigger_ != PreviewTrigger::ExternalHover) return;
        if (request.sourcePid != externalHoverSourcePid_ || request.generation < externalHoverGeneration_) { Log(L"external hover ignored: stale close"); return; }
        externalHoverGeneration_ = request.generation;
        Log(L"external hover close request=" + request.requestId + L" generation=" + std::to_wstring(request.generation));
        ClosePreview(PreviewCloseReason::HoverLeft);
        externalHoverSourcePid_ = 0; externalHoverRequestId_.clear();
    }

    void MoveExternalHover(const ExternalHoverRequest& request) {
        if (previewTrigger_ != PreviewTrigger::ExternalHover || request.sourcePid != externalHoverSourcePid_ || request.generation < externalHoverGeneration_) return;
        externalHoverGeneration_ = request.generation;
        if (!request.paths.empty()) OpenExternalHover(request);
    }

private:
    static DWORD WINAPI CheckForUpdatesThread(void* parameter) {
        HWND target = static_cast<HWND>(parameter);
        HINTERNET session = WinHttpOpen(L"MvView/" MVVIEW_VERSION_TEXT_W,
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return 0;
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);

        HINTERNET connection = WinHttpConnect(session, kLatestReleaseApiHost,
            INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET request = connection
            ? WinHttpOpenRequest(connection, L"GET", kLatestReleaseApiPath, nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
            : nullptr;
        std::wstring response;
        if (request) {
            const wchar_t headers[] =
                L"Accept: application/vnd.github+json\r\n"
                L"X-GitHub-Api-Version: 2022-11-28\r\n";
            if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    std::string bytes;
                    for (;;) {
                        DWORD available = 0;
                        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                        const size_t oldSize = bytes.size();
                        bytes.resize(oldSize + available);
                        DWORD read = 0;
                        if (!WinHttpReadData(request, bytes.data() + oldSize, available, &read)) break;
                        bytes.resize(oldSize + read);
                        if (read == 0) break;
                    }
                    response = Utf8ToWide(bytes);
                }
            }
        }
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);

        std::wstring tag;
        std::wstring url;
        if (!response.empty() && JsonString(response, L"tag_name", tag) &&
            IsNewerVersion(tag, kAppVersion)) {
            if (!JsonString(response, L"browser_download_url", url)) {
                JsonString(response, L"html_url", url);
            }
            if (!url.empty()) {
                auto* info = new UpdateInfo{tag, url};
                if (!IsWindow(target) || !PostMessageW(target, WM_MV_UPDATE_AVAILABLE, 0,
                        reinterpret_cast<LPARAM>(info))) {
                    delete info;
                }
            }
        }
        return 0;
    }

    void BeginUpdateCheck() {
        HANDLE thread = CreateThread(nullptr, 0, CheckForUpdatesThread, hwnd_, 0, nullptr);
        if (thread) CloseHandle(thread);
        else Log(L"update check thread could not be started");
    }

    void ShowUpdateDialog(UpdateInfo* info) {
        std::unique_ptr<UpdateInfo> owned(info);
        if (!owned) return;
        const std::wstring message = L"新しいバージョン " + owned->version +
            L" が GitHub で公開されています。\n現在のバージョン: v" + kAppVersion;
        TASKDIALOG_BUTTON buttons[] = {
            { 100, L"Download" },
            { IDCANCEL, L"後で" }
        };
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hwndParent = hwnd_;
        config.hInstance = inst_;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        config.pszWindowTitle = L"MvView Update";
        config.pszMainIcon = TD_INFORMATION_ICON;
        config.pszMainInstruction = L"MvView の更新があります";
        config.pszContent = message.c_str();
        config.cButtons = ARRAYSIZE(buttons);
        config.pButtons = buttons;
        config.nDefaultButton = 100;
        int selected = IDCANCEL;
        if (SUCCEEDED(TaskDialogIndirect(&config, &selected, nullptr, nullptr)) && selected == 100) {
            ShellExecuteW(hwnd_, L"open", owned->downloadUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    bool CreateMainWindow() {
        WNDCLASSW cls{};
        cls.lpfnWndProc = MvViewApp::WndProc;
        cls.hInstance = inst_;
        cls.lpszClassName = kMainWindowClass;
        cls.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
        ATOM atom = RegisterClassW(&cls);
        if (!atom) {
            DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                Log(L"RegisterClass failed: " + FormatWin32Error(err));
                return false;
            }
        }
        SetLastError(ERROR_SUCCESS);
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainWindowClass, kAppName, WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, inst_, this);
        if (!hwnd_) {
            Log(L"CreateWindowEx failed: " + FormatWin32Error(GetLastError()));
            return false;
        }
        gHookTargetWindow = hwnd_;
        Log(L"Main hidden window created");
        return true;
    }

    void InstallHooks() {
        hookForeground_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        hookSelection_ = SetWinEventHook(EVENT_OBJECT_FOCUS_VALUE, EVENT_OBJECT_SELECTIONWITHIN_VALUE, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
        Log(hookForeground_ ? L"SetWinEventHook foreground installed" : L"SetWinEventHook foreground failed");
        Log(hookSelection_ ? L"SetWinEventHook selection/focus installed" : L"SetWinEventHook selection/focus failed");
        Log(mouseHook_ ? L"SetWindowsHookEx(WH_MOUSE_LL) installed" : L"SetWindowsHookEx(WH_MOUSE_LL) failed");
    }

    void Shutdown() {
        gHookTargetWindow = nullptr;
        KillTimer(hwnd_, TIMER_CURSOR_CLOSE);
        KillTimer(hwnd_, TIMER_HOVER_MONITOR);
        KillTimer(hwnd_, TIMER_SELECTION_VALIDATE);
        if (hookForeground_) UnhookWinEvent(hookForeground_);
        if (hookSelection_) UnhookWinEvent(hookSelection_);
        if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
        hookForeground_ = nullptr;
        hookSelection_ = nullptr;
        mouseHook_ = nullptr;
        ClosePreview(PreviewCloseReason::Shutdown);
        multiConfirm_.Destroy();
        tray_.Remove();
        preview_.Destroy();
        for (auto& w : extraPreviews_) if (w) w->Destroy();
        extraPreviews_.clear();
        settings_.Save();
        Log(L"MvView exited");
        if (comReady_) CoUninitialize();
    }

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
        HWND main = gHookTargetWindow;
        if (main) PostMessageW(main, WM_MV_HOOK_EVENT, (WPARAM)event, (LPARAM)hwnd);
    }

    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION && lp) {
            const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            bool post = wp != WM_MOUSEMOVE;
            if (wp == WM_MOUSEMOVE) {
                static DWORD lastMovePost = 0;
                DWORD now = GetTickCount();
                if (now - lastMovePost >= 50) {
                    lastMovePost = now;
                    post = true;
                }
            }
            if (post) {
                HWND main = gHookTargetWindow;
                if (main) PostMessageW(main, WM_MV_MOUSE_EVENT, wp, PackScreenPoint(info->pt));
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    bool ForegroundIsExplorer(HWND* outRoot = nullptr) const {
        HWND fg = GetForegroundWindow();
        if (!fg || !explorer_.IsExplorerWindow(fg)) return false;
        HWND root = GetAncestor(fg, GA_ROOT);
        if (outRoot) *outRoot = root;
        return true;
    }

    bool IsAppInteractionWindow(HWND hwnd) const {
        if (!hwnd) return false;
        if (multiConfirm_.IsVisible() && (hwnd == multiConfirm_.Hwnd() || GetAncestor(hwnd, GA_ROOT) == multiConfirm_.Hwnd())) return true;
        if (preview_.IsVisible() && (hwnd == preview_.Hwnd() || GetAncestor(hwnd, GA_ROOT) == preview_.Hwnd())) return true;
        for (const auto& w : extraPreviews_) {
            if (w && w->IsVisible() && (hwnd == w->Hwnd() || GetAncestor(hwnd, GA_ROOT) == w->Hwnd())) return true;
        }
        return false;
    }

    void EnsureHoverMonitor() {
        if ((previewTrigger_ == PreviewTrigger::DirectOpen || previewTrigger_ == PreviewTrigger::ExternalHover)) return;
        if (hoverState_ != HoverState::Idle || multiConfirm_.IsVisible() || confirmationRequiresLeave_) {
            SetTimer(hwnd_, TIMER_HOVER_MONITOR, 50, nullptr);
        }
    }

    void StopHoverMonitorIfIdle() {
        if (hoverState_ == HoverState::Idle && !multiConfirm_.IsVisible() && !confirmationRequiresLeave_)
            KillTimer(hwnd_, TIMER_HOVER_MONITOR);
    }

    bool SameTarget(const HoverTarget& a, const HoverTarget& b) const {
        return a.explorerRoot == b.explorerRoot && a.tabKey == b.tabKey && a.key == b.key;
    }

    bool PathInMedia(const std::wstring& path, const std::vector<std::pair<std::wstring, MediaKind>>& media) const {
        const std::wstring key = ToLower(path);
        for (const auto& entry : media) if (ToLower(entry.first) == key) return true;
        return false;
    }

    std::wstring MakeMediaSetKey(const std::vector<std::pair<std::wstring, MediaKind>>& media) const {
        std::vector<std::wstring> parts;
        for (const auto& entry : media) parts.push_back(ToLower(entry.first));
        std::sort(parts.begin(), parts.end());
        std::wstring key;
        for (const auto& p : parts) key += p + L"\n";
        return key;
    }

    std::vector<std::pair<std::wstring, MediaKind>> ReadSelectedMedia(
        HWND explorerRoot,
        std::wstring& selectionKey,
        std::wstring* tabKey = nullptr) {
        selectionKey.clear();
        std::wstring resolvedTab;
        auto selected = explorer_.GetSelectedPathsForForeground(explorerRoot, &resolvedTab, nullptr, false);
        std::vector<std::pair<std::wstring, MediaKind>> media;
        for (const auto& path : selected) {
            MediaKind kind = DetectMediaKind(path);
            if (kind != MediaKind::None && FileExists(path)) media.push_back({ path, kind });
        }
        selectionKey = MakeMediaSetKey(media);
        if (tabKey) *tabKey = resolvedTab;
        return media;
    }

    void BeginHover(const HoverTarget& target) {
        hoverTarget_ = target;
        currentExplorerRoot_ = target.explorerRoot;
        currentTabKey_ = target.tabKey;
        hoverStartTick_ = GetTickCount64();
        hoverStartPosition_ = latestMousePoint_;
        hoverLeaveTick_ = 0;
        lastFullResolveTick_ = hoverStartTick_;
        lastSuccessfulResolveTick_ = hoverStartTick_;
        pendingMultiMedia_.clear();
        pendingSelectionKey_.clear();

        std::wstring selectedTab;
        auto selectedMedia = ReadSelectedMedia(target.explorerRoot, pendingSelectionKey_, &selectedTab);
        const bool hasMultipleMediaSelection = selectedMedia.size() > 1;
        const bool targetInMulti = hasMultipleMediaSelection && PathInMedia(target.path, selectedMedia);
        if (hasMultipleMediaSelection && !targetInMulti) {
            hoverState_ = HoverState::Idle;
            Log(L"hover wait cancelled: multiple media are selected but hovered item is not in the selection");
            EnsureHoverMonitor();
            return;
        }
        if (targetInMulti) {
            if (!selectedTab.empty() && selectedTab != target.tabKey) {
                Log(L"hover wait cancelled: Explorer tab identity changed during selection read");
                ResetHoverTarget();
                return;
            }
            pendingMultiMedia_ = std::move(selectedMedia);
            hoverState_ = HoverState::WaitingMulti;
        } else {
            hoverState_ = HoverState::WaitingSingle;
        }
        Log(L"hover enter: " + target.path);
        Log(L"hover wait started: " + target.path + L" delay=" + std::to_wstring(settings_.hoverPreviewDelayMs) +
            L"ms position=" + std::to_wstring(hoverStartPosition_.x) + L"," + std::to_wstring(hoverStartPosition_.y));
        EnsureHoverMonitor();
    }

    void ResetHoverTarget() {
        hoverTarget_ = {};
        currentExplorerRoot_ = nullptr;
        currentTabKey_.clear();
        hoverStartTick_ = 0;
        hoverLeaveTick_ = 0;
        pendingMultiMedia_.clear();
        pendingSelectionKey_.clear();
        if (!AnyPreviewVisible() && !multiConfirm_.IsVisible()) hoverState_ = HoverState::Idle;
        StopHoverMonitorIfIdle();
    }

    void CancelHoverWait(const std::wstring& reason) {
        if (hoverState_ == HoverState::WaitingSingle || hoverState_ == HoverState::WaitingMulti) {
            Log(L"hover wait cancelled: " + reason);
        }
        hoverState_ = HoverState::Idle;
        ResetHoverTarget();
    }

    void ProcessHoverSample(POINT pt, bool forceResolve = false) {
        latestMousePoint_ = pt;
        latestMousePointKnown_ = true;
        if (!settings_.enabled || !settings_.hoverPreviewEnabled) return;
        if ((previewTrigger_ == PreviewTrigger::DirectOpen || previewTrigger_ == PreviewTrigger::ExternalHover)) return;

        HWND fg = GetForegroundWindow();
        const bool explorerForeground = explorer_.IsExplorerWindow(fg);
        const bool appInteractionForeground = IsAppInteractionWindow(fg);
        if (!explorerForeground && !appInteractionForeground) {
            // Hover discovery is allowed only while Explorer is foreground. Existing
            // playback follows the legacy closeWhenForegroundLost setting.
            if (hoverState_ == HoverState::ConfirmingMulti) {
                multiConfirm_.Hide();
                Log(L"multi confirmation cancelled: Explorer is not foreground");
                hoverState_ = HoverState::Idle;
                ResetHoverTarget();
            } else if (hoverState_ == HoverState::WaitingSingle || hoverState_ == HoverState::WaitingMulti) {
                CancelHoverWait(L"Explorer is not foreground");
            } else if (settings_.closeWhenForegroundLost && hoverState_ != HoverState::Idle) {
                ClosePreview(PreviewCloseReason::ForegroundLost);
            }
            return;
        }

        ULONGLONG now = GetTickCount64();
        if ((hoverState_ == HoverState::WaitingMulti || hoverState_ == HoverState::ConfirmingMulti || hoverState_ == HoverState::PlayingMulti) &&
            now - lastSelectionValidationTick_ >= 500) {
            lastSelectionValidationTick_ = now;
            ValidateExplorerContext();
            if (hoverState_ == HoverState::Idle && !multiConfirm_.IsVisible()) return;
        }

        if (multiConfirm_.IsVisible() &&
            (multiConfirm_.ContainsScreenPoint(pt) ||
             PointInTransferBridge(hoverTarget_.itemRect, multiConfirm_.WindowRect(), pt))) {
            hoverLeaveTick_ = 0;
            EnsureHoverMonitor();
            return;
        }
        if ((hoverState_ == HoverState::PlayingSingle || hoverState_ == HoverState::PlayingMulti) &&
            settings_.allowPointerOverPreview &&
            (CursorInsideAnyPreview(pt) || CursorInPreviewTransferCorridor(pt))) {
            hoverLeaveTick_ = 0;
            EnsureHoverMonitor();
            return;
        }

        HWND explorerRoot = nullptr;
        if (explorerForeground) explorerRoot = GetAncestor(fg, GA_ROOT);
        else if (appInteractionForeground && currentExplorerRoot_ && IsWindow(currentExplorerRoot_)) explorerRoot = currentExplorerRoot_;
        if (!explorerRoot) {
            HandleNoHoverTarget(L"Explorer is not foreground");
            return;
        }

        // Avoid a UI Automation + Shell enumeration for every mouse packet. A
        // cached item is valid for 500 ms; unresolved/small movements are sampled
        // at most every 100 ms. This bounds CPU usage without delaying hover leave
        // beyond the required 100 ms window.
        const std::int64_t dx = lastFullResolvePointKnown_ ?
            static_cast<std::int64_t>(pt.x) - lastFullResolvePoint_.x : 1000;
        const std::int64_t dy = lastFullResolvePointKnown_ ?
            static_cast<std::int64_t>(pt.y) - lastFullResolvePoint_.y : 1000;
        const bool tinyMove = (dx * dx + dy * dy) <= 16;
        if (!forceResolve && !forceHoverResolve_ && lastFullResolvePointKnown_ &&
            now - lastFullResolveTick_ < 100 && tinyMove &&
            !(hoverTarget_.IsValid() && PointInRect(hoverTarget_.itemRect, pt))) {
            return;
        }

        bool mayUseCache = hoverTarget_.IsValid() && hoverTarget_.explorerRoot == explorerRoot &&
            PointInRect(hoverTarget_.itemRect, pt) && !forceResolve && !forceHoverResolve_ &&
            now - lastFullResolveTick_ < 500;
        HoverTarget resolved;
        bool hasTarget = false;
        if (mayUseCache) {
            resolved = hoverTarget_;
            hasTarget = true;
        } else {
            hasTarget = explorer_.ResolveHoveredItem(explorerRoot, pt, resolved);
            forceHoverResolve_ = false;
            lastFullResolveTick_ = now;
            lastFullResolvePoint_ = pt;
            lastFullResolvePointKnown_ = true;
            if (hasTarget) lastSuccessfulResolveTick_ = now;
        }

        // UI Automation can transiently return no element while Explorer redraws a
        // thumbnail. If the pointer is still inside the last confirmed item rect,
        // retain that exact path briefly instead of cancelling/reopening the preview.
        if (!hasTarget && hoverTarget_.IsValid() && hoverTarget_.explorerRoot == explorerRoot &&
            PointInRect(hoverTarget_.itemRect, pt) && now - lastSuccessfulResolveTick_ < 1000) {
            resolved = hoverTarget_;
            hasTarget = true;
        }

        if (!hasTarget) {
            HandleNoHoverTarget(L"pointer left Explorer media item");
            return;
        }

        if (!hoverTarget_.IsValid()) {
            BeginHover(resolved);
            return;
        }

        if (!SameTarget(hoverTarget_, resolved) && hoverState_ == HoverState::Idle &&
            confirmationRequiresLeave_ && resolved.explorerRoot == currentExplorerRoot_ &&
            resolved.tabKey == currentTabKey_ && PathInMedia(resolved.path, pendingMultiMedia_)) {
            // Cancellation suppression belongs to the whole selected set, not to a
            // single selected item. Moving among selected items must not show the
            // same confirmation again.
            hoverTarget_ = resolved;
            hoverLeaveTick_ = 0;
            return;
        }

        if (!SameTarget(hoverTarget_, resolved) &&
            (hoverState_ == HoverState::PlayingMulti || hoverState_ == HoverState::ConfirmingMulti) &&
            resolved.explorerRoot == currentExplorerRoot_ && resolved.tabKey == currentTabKey_ &&
            PathInMedia(resolved.path, hoverState_ == HoverState::PlayingMulti ? playingMedia_ : pendingMultiMedia_)) {
            hoverTarget_ = resolved;
            hoverLeaveTick_ = 0;
            return;
        }

        if (!SameTarget(hoverTarget_, resolved)) {
            Log(L"hover target changed: " + hoverTarget_.path + L" -> " + resolved.path);
            if (hoverState_ == HoverState::PlayingSingle || hoverState_ == HoverState::PlayingMulti) {
                ClosePreview(PreviewCloseReason::HoverTargetChanged);
            } else if (hoverState_ == HoverState::ConfirmingMulti) {
                multiConfirm_.Hide();
                Log(L"multi confirmation cancelled: hover target changed");
                hoverState_ = HoverState::Idle;
            } else {
                CancelHoverWait(L"hover target changed");
            }
            confirmationRequiresLeave_ = false;
            suppressedConfirmationKey_.clear();
            BeginHover(resolved);
            return;
        }

        hoverTarget_.itemRect = resolved.itemRect;
        hoverLeaveTick_ = 0;

        if (hoverState_ == HoverState::Idle) {
            if (confirmationRequiresLeave_ && pendingSelectionKey_ == suppressedConfirmationKey_) return;
            BeginHover(resolved);
            return;
        }
        if (hoverState_ == HoverState::WaitingSingle || hoverState_ == HoverState::WaitingMulti) {
            if (now - hoverStartTick_ >= (ULONGLONG)settings_.hoverPreviewDelayMs) CompleteHoverWait();
            return;
        }
        if (hoverState_ == HoverState::PlayingMulti && now >= exitGraceUntil_ &&
            !PathInMedia(resolved.path, playingMedia_)) {
            ClosePreview(PreviewCloseReason::HoverTargetChanged);
            BeginHover(resolved);
        }
    }

    void HandleNoHoverTarget(const std::wstring& reason) {
        ULONGLONG now = GetTickCount64();
        if (confirmationRequiresLeave_) {
            confirmationRequiresLeave_ = false;
            suppressedConfirmationKey_.clear();
        }
        if (hoverState_ == HoverState::WaitingSingle || hoverState_ == HoverState::WaitingMulti) {
            Log(L"hover target left: " + hoverTarget_.path);
            CancelHoverWait(reason);
            return;
        }
        if (hoverState_ == HoverState::ConfirmingMulti) {
            // Allow the pointer to cross the small gap between the Explorer item
            // and the non-modal confirmation window without cancelling it.
            if (now < confirmMoveGraceUntil_) return;
            multiConfirm_.Hide();
            Log(L"multi confirmation cancelled: hover target left");
            hoverState_ = HoverState::Idle;
            ResetHoverTarget();
            return;
        }
        if (hoverState_ == HoverState::Idle) {
            ResetHoverTarget();
            return;
        }
        if (hoverState_ == HoverState::PlayingSingle || hoverState_ == HoverState::PlayingMulti) {
            if (now < exitGraceUntil_) return;
            if (!settings_.stopImmediatelyOnHoverLeave) {
                if (hoverLeaveTick_ == 0) hoverLeaveTick_ = now;
                if (now - hoverLeaveTick_ < 450) return;
            }
            // stopImmediatelyOnHoverLeave closes on the first confirmed sample.
            // Resolution is sampled at 50-100 ms, so this stays within the target
            // response time without a distance-based heuristic.
            Log(L"hover target left: " + hoverTarget_.path);
            ClosePreview(PreviewCloseReason::HoverLeft);
        }
    }

    void CompleteHoverWait() {
        if (!hoverTarget_.IsValid()) return;
        if (hoverState_ == HoverState::WaitingSingle) {
            OpenSingleHoverPreview();
            return;
        }
        if (hoverState_ == HoverState::WaitingMulti) {
            std::wstring currentKey;
            std::wstring tabKey;
            auto selectedMedia = ReadSelectedMedia(hoverTarget_.explorerRoot, currentKey, &tabKey);
            if (currentKey.empty() || currentKey != pendingSelectionKey_ || tabKey != hoverTarget_.tabKey ||
                !PathInMedia(hoverTarget_.path, selectedMedia)) {
                CancelHoverWait(L"multiple selection changed during hover wait");
                return;
            }
            pendingMultiMedia_ = std::move(selectedMedia);
            if (settings_.confirmMultipleSelection) ShowMultiConfirmation();
            else AcceptMultiConfirmation();
        }
    }

    void OpenSingleHoverPreview() {
        if (!hoverTarget_.IsValid()) return;
        HoverTarget target = hoverTarget_;
        ClosePreview(PreviewCloseReason::Reload);
        hoverTarget_ = target;
        currentExplorerRoot_ = target.explorerRoot;
        currentTabKey_ = target.tabKey;
        Log(L"hover preview opened: " + target.path);
        if (preview_.ShowPathNearItem(target.path, target.kind, target.itemRect, latestMousePoint_)) {
            previewTrigger_ = PreviewTrigger::HoverSingle;
            hoverState_ = HoverState::PlayingSingle;
            playingMedia_ = { { target.path, target.kind } };
            currentExplorerRoot_ = target.explorerRoot;
            currentTabKey_ = target.tabKey;
            SetTimer(hwnd_, TIMER_CURSOR_CLOSE, 200, nullptr);
            EnsureHoverMonitor();
        }
    }

    void ShowMultiConfirmation() {
        if (pendingMultiMedia_.size() <= 1) return;
        if (confirmationRequiresLeave_ && pendingSelectionKey_ == suppressedConfirmationKey_) {
            hoverState_ = HoverState::Idle;
            return;
        }
        if (multiConfirm_.Show(inst_, hwnd_, (int)std::min<size_t>(pendingMultiMedia_.size(), 9),
            hoverTarget_.itemRect, settings_.BorderColor())) {
            hoverState_ = HoverState::ConfirmingMulti;
            suppressedConfirmationKey_ = pendingSelectionKey_;
            confirmationRequiresLeave_ = false;
            confirmMoveGraceUntil_ = GetTickCount64() + 900;
            Log(L"multi confirmation shown: count=" + std::to_wstring(pendingMultiMedia_.size()));
            EnsureHoverMonitor();
        }
    }

    void AcceptMultiConfirmation() {
        std::wstring currentKey;
        std::wstring tabKey;
        auto selectedMedia = ReadSelectedMedia(hoverTarget_.explorerRoot, currentKey, &tabKey);
        if (currentKey.empty() || currentKey != pendingSelectionKey_ || tabKey != hoverTarget_.tabKey) {
            multiConfirm_.Hide();
            Log(L"multi confirmation cancelled: selection changed before acceptance");
            hoverState_ = HoverState::Idle;
            ResetHoverTarget();
            return;
        }
        multiConfirm_.Hide();
        confirmationRequiresLeave_ = false;
        confirmMoveGraceUntil_ = 0;
        suppressedConfirmationKey_.clear();
        Log(L"multi confirmation accepted: count=" + std::to_wstring(selectedMedia.size()));
        playingSelectionKey_ = currentKey;
        playingMedia_ = selectedMedia;
        ShowMultiple(selectedMedia, hoverTarget_.itemRect);
        previewTrigger_ = PreviewTrigger::HoverMultiConfirmed;
        hoverState_ = HoverState::PlayingMulti;
        exitGraceUntil_ = GetTickCount64() + 500;
        SetTimer(hwnd_, TIMER_CURSOR_CLOSE, 200, nullptr);
        EnsureHoverMonitor();
        if (currentExplorerRoot_) SetForegroundWindow(currentExplorerRoot_);
    }

    void CancelMultiConfirmation(bool fromUser) {
        multiConfirm_.Hide();
        confirmMoveGraceUntil_ = 0;
        Log(fromUser ? L"multi confirmation cancelled" : L"multi confirmation closed");
        hoverState_ = HoverState::Idle;
        suppressedConfirmationKey_ = pendingSelectionKey_;
        confirmationRequiresLeave_ = true;
        EnsureHoverMonitor();
        if (currentExplorerRoot_) SetForegroundWindow(currentExplorerRoot_);
    }

    void ValidateExplorerContext() {
        if ((previewTrigger_ == PreviewTrigger::DirectOpen || previewTrigger_ == PreviewTrigger::ExternalHover) || hoverState_ == HoverState::Idle) return;
        HWND root = currentExplorerRoot_ ? currentExplorerRoot_ : hoverTarget_.explorerRoot;
        if (!root || !IsWindow(root)) {
            ClosePreview(PreviewCloseReason::SelectionChanged);
            return;
        }

        std::wstring activeTabKey;
        explorer_.GetSelectedPathsForForeground(root, &activeTabKey, nullptr, false);
        if (activeTabKey.empty() || (!currentTabKey_.empty() && activeTabKey != currentTabKey_)) {
            Log(L"Explorer tab changed: expected=" + currentTabKey_ + L" actual=" + activeTabKey);
            if (hoverState_ == HoverState::WaitingSingle || hoverState_ == HoverState::WaitingMulti) {
                CancelHoverWait(L"Explorer tab changed");
            } else {
                ClosePreview(PreviewCloseReason::SelectionChanged);
            }
            return;
        }
        ValidateMultiSelection();
    }

    void ValidateMultiSelection() {
        if (hoverState_ != HoverState::WaitingMulti && hoverState_ != HoverState::ConfirmingMulti && hoverState_ != HoverState::PlayingMulti) return;
        HWND root = currentExplorerRoot_ ? currentExplorerRoot_ : hoverTarget_.explorerRoot;
        if (!root || !IsWindow(root)) {
            ClosePreview(PreviewCloseReason::SelectionChanged);
            return;
        }
        std::wstring key;
        std::wstring tabKey;
        auto media = ReadSelectedMedia(root, key, &tabKey);
        const std::wstring expected = hoverState_ == HoverState::PlayingMulti ? playingSelectionKey_ : pendingSelectionKey_;
        if (key.empty() || key != expected || (!currentTabKey_.empty() && tabKey != currentTabKey_)) {
            multiConfirm_.Hide();
            Log(L"multiple selection changed");
            ClosePreview(PreviewCloseReason::SelectionChanged);
        } else if (hoverState_ != HoverState::PlayingMulti) {
            pendingMultiMedia_ = std::move(media);
        }
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

    bool CursorInPreviewTransferCorridor(POINT pt) const {
        if (!hoverTarget_.IsValid()) return false;
        if (preview_.IsVisible() && PointInTransferBridge(hoverTarget_.itemRect, preview_.WindowRect(), pt)) return true;
        for (const auto& w : extraPreviews_) {
            if (w && w->IsVisible() && PointInTransferBridge(hoverTarget_.itemRect, w->WindowRect(), pt)) return true;
        }
        return false;
    }

    void CloseExtraPreviews() {
        for (auto& w : extraPreviews_) if (w) w->Destroy();
        extraPreviews_.clear();
    }

    POINT PlaceGridAvoidingRect(const RECT& avoidRect, int gridW, int gridH) const {
        POINT center{ (avoidRect.left + avoidRect.right) / 2, (avoidRect.top + avoidRect.bottom) / 2 };
        HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        RECT work = mi.rcWork;
        const int gap = 14;
        std::vector<POINT> choices = {
            { avoidRect.right + gap, avoidRect.top },
            { avoidRect.left - gridW - gap, avoidRect.top },
            { avoidRect.left, avoidRect.bottom + gap },
            { avoidRect.left, avoidRect.top - gridH - gap }
        };
        for (POINT p : choices) {
            RECT r{ p.x, p.y, p.x + gridW, p.y + gridH };
            if (r.left >= work.left && r.top >= work.top && r.right <= work.right && r.bottom <= work.bottom && !IntersectRects(r, avoidRect)) return p;
        }
        const LONG minX = work.left + 8;
        const LONG maxX = std::max<LONG>(minX, work.right - gridW - 8);
        const LONG minY = work.top + 8;
        const LONG maxY = std::max<LONG>(minY, work.bottom - gridH - 8);
        POINT p{ std::clamp<LONG>(avoidRect.right + gap, minX, maxX),
            std::clamp<LONG>(avoidRect.top, minY, maxY) };
        return p;
    }

    static bool IntersectRects(const RECT& a, const RECT& b) {
        RECT intersection{};
        return IntersectRect(&intersection, &a, &b) != FALSE;
    }

    void ShowMultiple(const std::vector<std::pair<std::wstring, MediaKind>>& media, const RECT& avoidRect) {
        preview_.Hide();
        CloseExtraPreviews();
        const size_t maxCount = std::min<size_t>(media.size(), 9);
        const int count = static_cast<int>(maxCount);
        int cols = count <= 3 ? count : (count <= 4 ? 2 : 3);
        cols = std::max(1, cols);
        const int rows = (count + cols - 1) / cols;
        const int gap = 12;
        const int cellW = preview_.WindowWidth();
        const int cellH = preview_.WindowHeight();
        const int gridW = cols * cellW + (cols - 1) * gap;
        const int gridH = rows * cellH + (rows - 1) * gap;
        POINT origin = PlaceGridAvoidingRect(avoidRect, gridW, gridH);

        Log(L"multi preview layout: count=" + std::to_wstring(count) + L" cols=" + std::to_wstring(cols) + L" rows=" + std::to_wstring(rows));
        for (size_t i = 0; i < maxCount; ++i) {
            PreviewWindow* window = nullptr;
            if (i == 0) window = &preview_;
            else {
                auto extra = std::make_unique<PreviewWindow>();
                if (!extra->Create(inst_, &settings_)) continue;
                window = extra.get();
                extraPreviews_.push_back(std::move(extra));
            }
            int col = static_cast<int>(i) % cols;
            int row = static_cast<int>(i) / cols;
            POINT topLeft{ origin.x + col * (cellW + gap), origin.y + row * (cellH + gap) };
            POINT logicalAnchor{ (avoidRect.left + avoidRect.right) / 2, (avoidRect.top + avoidRect.bottom) / 2 };
            window->ShowPathAt(media[i].first, media[i].second, topLeft, logicalAnchor, true);
        }
        preview_.StartPlayback();
        for (auto& w : extraPreviews_) if (w) w->StartPlayback();
        Log(L"multi preview playback started together");
    }

    void ClosePreview(PreviewCloseReason reason) {
        const bool closingExternalHover = previewTrigger_ == PreviewTrigger::ExternalHover;
        if (AnyPreviewVisible() || multiConfirm_.IsVisible()) {
            Log(L"preview closed: " + PreviewCloseReasonText(reason));
        }
        multiConfirm_.Hide();
        preview_.Hide();
        for (auto& w : extraPreviews_) if (w) w->Hide();
        playingMedia_.clear();
        playingSelectionKey_.clear();
        pendingMultiMedia_.clear();
        pendingSelectionKey_.clear();
        confirmationRequiresLeave_ = false;
        suppressedConfirmationKey_.clear();
        KillTimer(hwnd_, TIMER_CURSOR_CLOSE);
        previewTrigger_ = PreviewTrigger::None;
        if (closingExternalHover) {
            externalHoverSourcePid_ = 0;
            externalHoverRequestId_.clear();
        }
        hoverState_ = HoverState::Idle;
        hoverLeaveTick_ = 0;
        exitGraceUntil_ = 0;
        confirmMoveGraceUntil_ = 0;
        ResetHoverTarget();
    }

    void TickPreviewWindows() {
        if (!AnyPreviewVisible()) {
            KillTimer(hwnd_, TIMER_CURSOR_CLOSE);
            return;
        }
        preview_.Tick();
        for (auto& w : extraPreviews_) if (w) w->Tick();
    }

    void ToggleEnabled() {
        settings_.enabled = !settings_.enabled;
        settings_.Save();
        if (!settings_.enabled) ClosePreview(PreviewCloseReason::Disabled);
        else if (settings_.hoverPreviewEnabled) {
            GetCursorPos(&latestMousePoint_);
            latestMousePointKnown_ = true;
            forceHoverResolve_ = true;
        }
    }

    void OpenSettingsDialog() {
        SettingsDialog dialog;
        if (dialog.Show(inst_, hwnd_, settings_)) {
            Log(L"settings updated from dialog");
            ClosePreview(PreviewCloseReason::SettingsChanged);
            if (settings_.enabled && settings_.hoverPreviewEnabled) {
                GetCursorPos(&latestMousePoint_);
                latestMousePointKnown_ = true;
                forceHoverResolve_ = true;
            }
        }
    }

    void OpenMpvRuntimeFolder() { ShellExecuteW(nullptr, L"open", RuntimeDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    void OpenLogsFolder() { ShellExecuteW(nullptr, L"open", LogsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

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
        std::wstring msg = std::wstring(kAppDisplayName) + L"\n\n" +
            L"Explorerのメディア項目をホバーすると、libmpvで小窓プレビューします。\n\n" +
            L"状態: " + mpvRuntimeStatus_ + L"\n\n" +
            L"EXE: " + GetModulePathText() + L"\n" +
            L"mpv runtime: " + RuntimeDir() + L"\n" +
            L"設定: " + SettingsPath() + L"\n" +
            L"ログ: " + LogPath();
        MessageBoxW(hwnd_, msg.c_str(), MVVIEW_APP_DISPLAY_NAME_W L" について", MB_OK | MB_ICONINFORMATION);
    }

    void ShowHelp() {
        std::wstring msg = std::wstring(L"MvView Help\n\n") +
            L"- Explorerのファイル一覧でメディア項目にカーソルを置くとプレビューします。\n" +
            L"- クリックや選択変更だけではプレビューを開始しません。\n" +
            L"- 複数選択時は、選択項目のホバー後に確認画面を表示します。\n" +
            L"- プレビューをクリックすると一時停止/再生、ホイールでシークします。\n" +
            L"- Ctrl+ホイールで一時的に表示解像度を変更します。\n" +
            L"- ESCでプレビューを閉じます。\n\n" +
            L"設定: " + SettingsPath() + L"\n" +
            L"ログ: " + LogPath();
        MessageBoxW(hwnd_, msg.c_str(), MVVIEW_APP_DISPLAY_NAME_W L" Help", MB_OK | MB_ICONINFORMATION);
    }

    LRESULT OnCopyData(const COPYDATASTRUCT* cds) {
        if (!cds || !cds->lpData || cds->cbData < sizeof(wchar_t) || (cds->cbData % sizeof(wchar_t)) != 0) return 0;
        const size_t chars = cds->cbData / sizeof(wchar_t);
        const wchar_t* data = static_cast<const wchar_t*>(cds->lpData);
        std::wstring text(data, chars > 0 && data[chars-1] == L'\0' ? chars-1 : chars);
        if (cds->dwData == 1) { OpenExternalPath(text); return 1; }
        if (cds->dwData != MVVIEW_HOVER_COPYDATA_ID) return 0;
        ExternalHoverRequest request; std::wstring error;
        if (!ParseExternalHoverJson(text, request, error)) { Log(L"external hover rejected: " + error); return 0; }
        if (request.action == L"hover_open" || request.action == L"hover_update") OpenExternalHover(request);
        else if (request.action == L"hover_close") CloseExternalHover(request);
        else if (request.action == L"hover_move") MoveExternalHover(request);
        return 1;
    }

    void HandleMouseEvent(WPARAM eventType, LPARAM packedPoint) {
        POINT pt = UnpackScreenPoint(packedPoint);
        latestMousePoint_ = pt;
        latestMousePointKnown_ = true;
        if (eventType == WM_MOUSEMOVE) {
            ProcessHoverSample(pt, false);
        } else {
            // Clicks and selection changes invalidate cached Shell/UIA data but do
            // not start hover preview by themselves. They do validate the active
            // Explorer tab/selection so an existing preview stops on a tab switch.
            forceHoverResolve_ = true;
            if (hoverState_ != HoverState::Idle && (previewTrigger_ != PreviewTrigger::DirectOpen && previewTrigger_ != PreviewTrigger::ExternalHover)) {
                KillTimer(hwnd_, TIMER_SELECTION_VALIDATE);
                SetTimer(hwnd_, TIMER_SELECTION_VALIDATE, 60, nullptr);
            }
        }
    }

    void HandleForegroundEvent(HWND eventHwnd) {
        if ((previewTrigger_ == PreviewTrigger::DirectOpen || previewTrigger_ == PreviewTrigger::ExternalHover)) return;
        HWND fg = eventHwnd ? GetAncestor(eventHwnd, GA_ROOT) : GetForegroundWindow();
        if (explorer_.IsExplorerWindow(fg)) {
            // Foreground/focus events invalidate the tab cache but never start a
            // preview by themselves. The next mouse movement performs resolution.
            forceHoverResolve_ = true;
            return;
        }
        if (IsAppInteractionWindow(fg)) return;
        if (settings_.closeWhenForegroundLost) ClosePreview(PreviewCloseReason::ForegroundLost);
        else if (hoverState_ == HoverState::ConfirmingMulti) {
            multiConfirm_.Hide();
            Log(L"multi confirmation cancelled: foreground lost");
            hoverState_ = HoverState::Idle;
            ResetHoverTarget();
        } else CancelHoverWait(L"foreground lost");
    }

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (taskbarCreatedMsg_ && msg == taskbarCreatedMsg_) {
            tray_.Add(hwnd_, inst_, MpvTrayTip());
            tray_.UpdateTip(MpvTrayTip());
            return 0;
        }
        switch (msg) {
        case WM_MV_HOOK_EVENT:
            if (wp == EVENT_SYSTEM_FOREGROUND) HandleForegroundEvent(reinterpret_cast<HWND>(lp));
            else {
                forceHoverResolve_ = true;
                if (hoverState_ != HoverState::Idle && (previewTrigger_ != PreviewTrigger::DirectOpen && previewTrigger_ != PreviewTrigger::ExternalHover)) {
                    KillTimer(hwnd_, TIMER_SELECTION_VALIDATE);
                    SetTimer(hwnd_, TIMER_SELECTION_VALIDATE, 60, nullptr);
                } else if (confirmationRequiresLeave_ && currentExplorerRoot_) {
                    // A changed selection is a new confirmation context even when the
                    // pointer has not yet left the old item. Do not start here; only
                    // release the old suppression key.
                    std::wstring newKey;
                    std::wstring newTab;
                    ReadSelectedMedia(currentExplorerRoot_, newKey, &newTab);
                    if (newKey != suppressedConfirmationKey_ || newTab != currentTabKey_) {
                        confirmationRequiresLeave_ = false;
                        suppressedConfirmationKey_.clear();
                        pendingSelectionKey_.clear();
                    }
                }
            }
            return 0;
        case WM_MV_MOUSE_EVENT:
            HandleMouseEvent(wp, lp);
            return 0;
        case WM_MV_MULTI_CONFIRM:
            if (wp) AcceptMultiConfirmation();
            else CancelMultiConfirmation(true);
            return 0;
        case WM_MV_UPDATE_AVAILABLE:
            ShowUpdateDialog(reinterpret_cast<UpdateInfo*>(lp));
            return 0;
        case WM_TIMER:
            if (wp == TIMER_HOVER_MONITOR) {
                POINT pt{};
                GetCursorPos(&pt);
                ProcessHoverSample(pt, false);
                return 0;
            }
            if (wp == TIMER_CURSOR_CLOSE) { TickPreviewWindows(); return 0; }
            if (wp == TIMER_SELECTION_VALIDATE) {
                KillTimer(hwnd_, TIMER_SELECTION_VALIDATE);
                ValidateExplorerContext();
                return 0;
            }
            return 0;
        case WM_HOTKEY:
            if (wp == HOTKEY_ESCAPE) { ClosePreview(PreviewCloseReason::Escape); return 0; }
            return 0;
        case WM_TRAYICON: {
            const UINT trayEvent = LOWORD(lp);
            if (trayEvent == WM_RBUTTONDOWN || trayEvent == WM_RBUTTONUP || trayEvent == WM_CONTEXTMENU) {
                tray_.ShowMenu(settings_, AnyPreviewVisible());
            } else if (trayEvent == WM_LBUTTONUP || trayEvent == NIN_SELECT || trayEvent == NIN_KEYSELECT) {
                if (AnyPreviewVisible() || multiConfirm_.IsVisible()) ClosePreview(PreviewCloseReason::TrayCommand);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case CMD_TRAY_ENABLED: ToggleEnabled(); break;
            case CMD_TRAY_CLOSE_PREVIEW: ClosePreview(PreviewCloseReason::TrayCommand); break;
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
            gHookTargetWindow = nullptr;
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
                return TRUE;
            }
            return FALSE;
        }
        return self ? self->HandleMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
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
                        MVVIEW_APP_DISPLAY_NAME_W L" already running", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                }
            } else if (existing) {
                PostMessageW(existing, WM_MV_SHOW_STATUS, 0, 0);
            } else {
                MessageBoxW(nullptr, L"MvView mutex already exists, but the tray/main window was not found.\n\nAn old hidden process may be running. Please run:\nGet-Process MvView -ErrorAction SilentlyContinue | Stop-Process -Force",
                    MVVIEW_APP_DISPLAY_NAME_W L" hidden instance", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
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

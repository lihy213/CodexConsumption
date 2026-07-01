#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace
{
constexpr UINT_PTR kRefreshTimer = 1001;
constexpr UINT_PTR kPlacementTimer = 1002;
constexpr UINT_PTR kHoverDelayTimer = 1003;
constexpr UINT kRefreshDone = WM_APP + 10;
constexpr UINT kShowExisting = WM_APP + 11;
constexpr UINT kMenuRefresh = 2001;
constexpr UINT kMenuTogglePin = 2002;
constexpr UINT kMenuExit = 2003;
constexpr COLORREF kPanelBack = RGB(232, 246, 255);
constexpr COLORREF kPanelBorder = RGB(134, 190, 226);
constexpr COLORREF kPanelText = RGB(22, 70, 108);
constexpr int kPanelRadius = 8;
constexpr int kStatusPadX = 5;
constexpr int kStatusPadY = 2;
constexpr int kHoverPadX = 6;
constexpr int kHoverPadY = 4;

struct UsageItem
{
    int remainingPercent = -1;
    long long resetAt = 0;
};

struct UsageState
{
    UsageItem fiveHour;
    UsageItem weekly;
    std::wstring status = L"starting";
};

struct Config
{
    int refreshSeconds = 120;
    bool pinned = false;
    bool nativeEmbed = false;
    int floatingWidth = 250;
    int floatingHeight = 34;
    int pinnedWidth = 112;
    int marginRight = 16;
    int marginBottom = 6;
};

HINSTANCE g_instance = nullptr;
HWND g_owner = nullptr;
HWND g_status = nullptr;
HWND g_tooltip = nullptr;
HWND g_hoverTip = nullptr;
bool g_trackingMouse = false;
POINT g_hoverPoint{};
bool g_hoverTipVisible = false;
bool g_contextMenuOpen = false;
UINT g_taskbarCreated = 0;
UsageState g_usage;
Config g_config;
std::atomic_bool g_refreshing{false};
std::wstring g_appDir;
std::wstring g_configPath;
HANDLE g_singleInstance = nullptr;

struct NativeTaskbarEmbed
{
    HWND shell = nullptr;
    HWND host = nullptr;
    HWND taskList = nullptr;
    HWND originalParent = nullptr;
    RECT originalTaskRect{};
    bool hasOriginalTaskRect = false;
    bool embedded = false;

    bool Find()
    {
        shell = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!shell)
            return false;

        host = FindWindowExW(shell, nullptr, L"ReBarWindow32", nullptr);
        if (!host)
            host = FindWindowExW(shell, nullptr, L"WorkerW", nullptr);
        if (!host)
            host = shell;

        taskList = FindWindowExW(host, nullptr, L"MSTaskSwWClass", nullptr);
        if (!taskList)
            taskList = FindWindowExW(host, nullptr, L"MSTaskListWClass", nullptr);
        return true;
    }

    static int Width(const RECT& rc) { return rc.right - rc.left; }
    static int Height(const RECT& rc) { return rc.bottom - rc.top; }

    bool Embed(HWND child, int width, int height)
    {
        if (embedded)
            Restore(child);
        if (!Find())
            return false;

        RECT hostRect{};
        GetWindowRect(host, &hostRect);
        originalParent = GetParent(child);

        LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
        style &= ~WS_POPUP;
        style |= WS_CHILD;
        SetWindowLongPtrW(child, GWL_STYLE, style);
        SetParent(child, host);

        int x = 0;
        int y = std::max(0, (Height(hostRect) - height) / 2);

        if (taskList)
        {
            RECT taskRect{};
            GetWindowRect(taskList, &taskRect);
            if (!hasOriginalTaskRect)
            {
                originalTaskRect = taskRect;
                hasOriginalTaskRect = true;
            }
            int taskX = taskRect.left - hostRect.left;
            int taskY = taskRect.top - hostRect.top;
            int newTaskWidth = std::max(120, Width(taskRect) - width - 8);
            SetWindowPos(taskList, nullptr, taskX, taskY, newTaskWidth, Height(taskRect), SWP_NOZORDER | SWP_NOACTIVATE);
            x = taskX + newTaskWidth + 4;
        }
        else
        {
            HWND notify = FindWindowExW(shell, nullptr, L"TrayNotifyWnd", nullptr);
            RECT notifyRect{};
            if (notify && GetWindowRect(notify, &notifyRect))
                x = notifyRect.left - hostRect.left - width - 4;
            else
                x = Width(hostRect) - width - 96;
        }

        int maxX = std::max(0, Width(hostRect) - width);
        int maxY = std::max(0, Height(hostRect) - height);
        x = std::clamp(x, 0, maxX);
        y = std::clamp(y, 0, maxY);

        SetWindowPos(child, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(child, SW_SHOWNOACTIVATE);
        UpdateWindow(child);

        RECT childRect{};
        if (!GetWindowRect(child, &childRect) || Width(childRect) <= 0 || Height(childRect) <= 0)
        {
            Restore(child);
            return false;
        }

        embedded = true;
        return true;
    }

    void Restore(HWND child)
    {
        if (!embedded)
            return;

        if (taskList && hasOriginalTaskRect)
        {
            RECT hostRect{};
            GetWindowRect(host, &hostRect);
            SetWindowPos(
                taskList,
                nullptr,
                originalTaskRect.left - hostRect.left,
                originalTaskRect.top - hostRect.top,
                Width(originalTaskRect),
                Height(originalTaskRect),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (originalParent)
            SetParent(child, originalParent);
        LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
        style &= ~WS_CHILD;
        style |= WS_POPUP;
        SetWindowLongPtrW(child, GWL_STYLE, style);

        embedded = false;
        hasOriginalTaskRect = false;
        originalParent = nullptr;
    }
};

NativeTaskbarEmbed g_embed;

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

std::wstring UserProfileCodexAuth()
{
    wchar_t profile[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return {};
    return std::wstring(profile) + L"\\.codex\\auth.json";
}

std::string ReadFileUtf8(const std::wstring& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteDebug(const std::string& message)
{
    if (g_appDir.empty())
        return;
    std::ofstream out(g_appDir + L"\\usage-debug.log", std::ios::binary | std::ios::app);
    if (!out)
        return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[128]{};
    std::snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    out << line << message << "\n";
}

bool FindBool(const std::string& text, const std::string& key, bool fallback)
{
    size_t pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos)
        return fallback;
    size_t colon = text.find(':', pos);
    if (colon == std::string::npos)
        return fallback;
    size_t first = text.find_first_not_of(" \t\r\n", colon + 1);
    if (first == std::string::npos)
        return fallback;
    if (text.compare(first, 4, "true") == 0)
        return true;
    if (text.compare(first, 5, "false") == 0)
        return false;
    return fallback;
}

int FindInt(const std::string& text, const std::string& key, int fallback)
{
    size_t pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos)
        return fallback;
    size_t colon = text.find(':', pos);
    if (colon == std::string::npos)
        return fallback;
    size_t first = text.find_first_of("-0123456789", colon + 1);
    if (first == std::string::npos)
        return fallback;
    return std::atoi(text.c_str() + first);
}

void LoadConfig()
{
    g_configPath = g_appDir + L"\\config.json";
    std::string text = ReadFileUtf8(g_configPath);
    g_config.refreshSeconds = FindInt(text, "refresh_seconds", g_config.refreshSeconds);
    g_config.pinned = FindBool(text, "taskbar_status_pinned", g_config.pinned);
    g_config.nativeEmbed = FindBool(text, "taskbar_status_native_embed", g_config.nativeEmbed);
    g_config.floatingWidth = FindInt(text, "taskbar_status_width", g_config.floatingWidth);
    g_config.floatingHeight = FindInt(text, "taskbar_status_height", g_config.floatingHeight);
    g_config.pinnedWidth = FindInt(text, "taskbar_status_pinned_width", g_config.pinnedWidth);
    g_config.marginRight = FindInt(text, "taskbar_status_margin_right", g_config.marginRight);
    g_config.marginBottom = FindInt(text, "taskbar_status_margin_bottom", g_config.marginBottom);
}

void SaveConfig()
{
    std::ofstream output(g_configPath, std::ios::binary);
    if (!output)
        return;
    output
        << "{\n"
        << "  \"refresh_seconds\": " << g_config.refreshSeconds << ",\n"
        << "  \"show_desktop_window\": false,\n"
        << "  \"show_taskbar_status\": true,\n"
        << "  \"taskbar_status_width\": " << g_config.floatingWidth << ",\n"
        << "  \"taskbar_status_height\": " << g_config.floatingHeight << ",\n"
        << "  \"taskbar_status_pinned_width\": " << g_config.pinnedWidth << ",\n"
        << "  \"taskbar_status_pinned\": " << (g_config.pinned ? "true" : "false") << ",\n"
        << "  \"taskbar_status_native_embed\": " << (g_config.nativeEmbed ? "true" : "false") << ",\n"
        << "  \"taskbar_status_margin_right\": " << g_config.marginRight << ",\n"
        << "  \"taskbar_status_margin_bottom\": " << g_config.marginBottom << "\n"
        << "}\n";
}

std::string ExtractString(const std::string& text, const std::string& key)
{
    size_t pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos)
        return {};
    size_t colon = text.find(':', pos);
    size_t quote = text.find('"', colon + 1);
    if (quote == std::string::npos)
        return {};
    size_t end = text.find('"', quote + 1);
    if (end == std::string::npos)
        return {};
    return text.substr(quote + 1, end - quote - 1);
}

double ExtractNumberAfter(const std::string& text, size_t start, const std::string& key, double fallback)
{
    size_t pos = text.find("\"" + key + "\"", start);
    if (pos == std::string::npos)
        return fallback;
    size_t colon = text.find(':', pos);
    if (colon == std::string::npos)
        return fallback;
    size_t first = text.find_first_of("-0123456789", colon + 1);
    if (first == std::string::npos)
        return fallback;
    return std::atof(text.c_str() + first);
}

UsageItem ParseWindow(const std::string& json, const std::string& windowKey)
{
    UsageItem item;
    size_t start = json.find("\"" + windowKey + "\"");
    if (start == std::string::npos)
        return item;
    double used = ExtractNumberAfter(json, start, "used_percent", -1);
    double reset = ExtractNumberAfter(json, start, "reset_at", 0);
    if (used >= 0)
        item.remainingPercent = std::clamp(100 - static_cast<int>(used + 0.5), 0, 100);
    item.resetAt = static_cast<long long>(reset);
    return item;
}

std::string HttpGetUsage(const std::string& accessToken, DWORD& statusCode)
{
    statusCode = 0;
    std::string body;
    HINTERNET session = WinHttpOpen(L"CodexAssumptionNative/0.3", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        WriteDebug("winhttp_open_error=" + std::to_string(GetLastError()));
        return body;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connect = WinHttpConnect(session, L"chatgpt.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect)
    {
        WriteDebug("winhttp_connect_error=" + std::to_string(GetLastError()));
        WinHttpCloseHandle(session);
        return body;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/backend-api/wham/usage", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request)
    {
        WriteDebug("winhttp_request_error=" + std::to_string(GetLastError()));
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return body;
    }

    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

    std::wstring headers = L"Accept: application/json\r\nAuthorization: Bearer " + Utf8ToWide(accessToken) + L"\r\n";
    BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok)
        ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok)
        WriteDebug("winhttp_send_receive_error=" + std::to_string(GetLastError()));
    if (ok)
    {
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                break;
            chunk.resize(read);
            body += chunk;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}
UsageState FetchUsage()
{
    UsageState state;
    std::wstring authPath = UserProfileCodexAuth();
    std::string auth = ReadFileUtf8(authPath);
    WriteDebug("auth_len=" + std::to_string(auth.size()));
    std::string token = ExtractString(auth, "access_token");
    WriteDebug("token_len=" + std::to_string(token.size()));
    if (token.empty())
    {
        state.status = L"no token";
        return state;
    }

    DWORD statusCode = 0;
    std::string json = HttpGetUsage(token, statusCode);
    WriteDebug("http_status=" + std::to_string(statusCode) + " body_len=" + std::to_string(json.size()));
    if (json.empty())
    {
        state.status = L"offline";
        return state;
    }
    if (statusCode != 200)
    {
        state.status = L"http " + std::to_wstring(statusCode);
        return state;
    }

    state.fiveHour = ParseWindow(json, "primary_window");
    state.weekly = ParseWindow(json, "secondary_window");
    WriteDebug("parsed 5h=" + std::to_string(state.fiveHour.remainingPercent) + " 1w=" + std::to_string(state.weekly.remainingPercent));
    if (state.fiveHour.remainingPercent < 0 && state.weekly.remainingPercent < 0)
        state.status = L"parse error";
    else
        state.status = L"online";
    return state;
}
std::wstring PercentText(const UsageItem& item)
{
    if (item.remainingPercent < 0)
        return L"--";
    return std::to_wstring(item.remainingPercent) + L"%";
}

std::wstring FormatReset(long long resetAt)
{
    if (resetAt <= 0)
        return L"unknown";
    std::time_t raw = static_cast<std::time_t>(resetAt);
    std::tm local{};
    localtime_s(&local, &raw);
    wchar_t buffer[64]{};
    wcsftime(buffer, 64, L"%m-%d %H:%M", &local);
    return buffer;
}

std::wstring TooltipText()
{
    return L"下一次刷新时间\r\n5h: " + FormatReset(g_usage.fiveHour.resetAt) + L"\r\n1w: " + FormatReset(g_usage.weekly.resetAt);
}

std::wstring StatusLineText()
{
    if (g_usage.fiveHour.remainingPercent < 0 && g_usage.weekly.remainingPercent < 0 && g_usage.status != L"starting")
        return L"Codex  " + g_usage.status;
    return L"Codex  5h " + PercentText(g_usage.fiveHour) + L"  1w " + PercentText(g_usage.weekly);
}

HFONT CreateUiFont(int height, int weight)
{
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

SIZE MeasureSingleLine(const std::wstring& text, HFONT font)
{
    HDC hdc = GetDC(nullptr);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(hdc, oldFont);
    ReleaseDC(nullptr, hdc);
    return size;
}

SIZE MeasureStatusSize()
{
    HFONT font = CreateUiFont(g_config.pinned ? 16 : 20, FW_BOLD);
    SIZE size{};
    if (g_config.pinned)
    {
        SIZE first = MeasureSingleLine(L"5h  " + PercentText(g_usage.fiveHour), font);
        SIZE second = MeasureSingleLine(L"1w  " + PercentText(g_usage.weekly), font);
        size.cx = std::max(static_cast<int>(first.cx), static_cast<int>(second.cx)) + kStatusPadX * 2;
        size.cy = first.cy + second.cy + kStatusPadY * 2;
        size.cx = std::max(static_cast<int>(size.cx), 76);
        size.cy = std::max(static_cast<int>(size.cy), 34);
    }
    else
    {
        SIZE text = MeasureSingleLine(StatusLineText(), font);
        size.cx = text.cx + kStatusPadX * 2;
        size.cy = text.cy + kStatusPadY * 2;
        size.cx = std::max(static_cast<int>(size.cx), 150);
        size.cy = std::max(static_cast<int>(size.cy), 30);
    }
    DeleteObject(font);
    return size;
}

SIZE MeasureHoverTipSize()
{
    HFONT font = CreateUiFont(16, FW_NORMAL);
    HDC hdc = GetDC(nullptr);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
    RECT calc{0, 0, 320, 0};
    std::wstring text = TooltipText();
    DrawTextW(hdc, text.c_str(), -1, &calc, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(hdc, oldFont);
    ReleaseDC(nullptr, hdc);
    DeleteObject(font);
    return SIZE{(calc.right - calc.left) + kHoverPadX * 2, (calc.bottom - calc.top) + kHoverPadY * 2};
}

void ApplyRoundedRegion(HWND hwnd, int width, int height)
{
    if (!hwnd)
        return;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, kPanelRadius, kPanelRadius);
    SetWindowRgn(hwnd, region, TRUE);
}

void UpdateTooltip()
{
    if (!g_tooltip || !g_status)
        return;
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_SUBCLASS;
    tool.hwnd = g_status;
    tool.uId = 1;
    static std::wstring text;
    text = TooltipText();
    tool.lpszText = text.data();
    GetClientRect(g_status, &tool.rect);
    SendMessageW(g_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, 320);
}

RECT WorkArea()
{
    RECT rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    return rc;
}

bool TaskbarRect(RECT& rc)
{
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &data))
    {
        rc = data.rc;
        return true;
    }
    HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
    return shell && GetWindowRect(shell, &rc);
}

bool RectIntersects(const RECT& a, const RECT& b)
{
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}

bool RectHasArea(const RECT& rc)
{
    return rc.right > rc.left && rc.bottom > rc.top;
}

bool RectNearTaskbar(const RECT& rc, const RECT& taskbar)
{
    RECT expanded = taskbar;
    expanded.top -= 80;
    expanded.bottom += 8;
    return RectHasArea(rc) && RectIntersects(rc, expanded);
}

struct OccupiedScan
{
    RECT taskbar{};
    std::vector<RECT> rects;
};

bool IsOccupiedTaskbarWindow(HWND hwnd)
{
    if (hwnd == g_status)
        return false;

    wchar_t cls[256]{};
    wchar_t title[256]{};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);
    std::wstring className(cls);
    std::wstring windowText(title);

    if (className == L"TrayNotifyWnd" || className == L"MSTaskSwWClass" || className == L"MSTaskListWClass" || className == L"ReBarWindow32")
        return true;
    if (className == L"#32770" && windowText == L"TrafficMonitorTaskbarWindow")
        return true;
    if (windowText == L"TrafficMonitor" || className.rfind(L"TrafficMonitor_", 0) == 0)
        return true;
    return false;
}

BOOL CALLBACK CollectOccupiedChild(HWND hwnd, LPARAM lParam)
{
    auto* scan = reinterpret_cast<OccupiedScan*>(lParam);
    RECT rc{};
    if (GetWindowRect(hwnd, &rc) && RectNearTaskbar(rc, scan->taskbar) && IsOccupiedTaskbarWindow(hwnd))
        scan->rects.push_back(rc);
    return TRUE;
}

BOOL CALLBACK CollectOccupiedTopLevel(HWND hwnd, LPARAM lParam)
{
    auto* scan = reinterpret_cast<OccupiedScan*>(lParam);
    RECT rc{};
    if (GetWindowRect(hwnd, &rc) && RectNearTaskbar(rc, scan->taskbar) && IsOccupiedTaskbarWindow(hwnd))
        scan->rects.push_back(rc);
    EnumChildWindows(hwnd, CollectOccupiedChild, lParam);
    return TRUE;
}

int FindFreePinnedX(const RECT& taskbar, int width, int height, int preferredRight)
{
    OccupiedScan scan;
    scan.taskbar = taskbar;
    EnumWindows(CollectOccupiedTopLevel, reinterpret_cast<LPARAM>(&scan));

    const int gap = 6;
    int minX = static_cast<int>(taskbar.left) + gap;
    int maxX = static_cast<int>(taskbar.right) - width - gap;
    int x = std::clamp(preferredRight - width, minX, std::max(minX, maxX));
    int y = static_cast<int>(taskbar.top) + std::max(0, (static_cast<int>(taskbar.bottom - taskbar.top) - height) / 2);

    for (int guard = 0; guard < 80; ++guard)
    {
        RECT candidate{x, y, x + width, y + height};
        bool moved = false;
        int nextX = x;
        for (const RECT& occupied : scan.rects)
        {
            if (RectIntersects(candidate, occupied))
            {
                nextX = std::min(nextX, static_cast<int>(occupied.left) - width - gap);
                moved = true;
            }
        }
        if (!moved)
            return x;
        if (nextX < minX)
            return minX;
        x = nextX;
    }
    return x;
}

void ApplyStatusPlacement()
{
    if (!g_status)
        return;

    SIZE measured = MeasureStatusSize();
    int width = measured.cx;
    int height = measured.cy;

    if (g_config.pinned && g_config.nativeEmbed && g_embed.Embed(g_status, width, height))
    {
        InvalidateRect(g_status, nullptr, TRUE);
        return;
    }

    g_embed.Restore(g_status);

    RECT area{};
    if (g_config.pinned && TaskbarRect(area))
    {
        int areaHeight = area.bottom - area.top;
        int x = FindFreePinnedX(area, width, height, static_cast<int>(area.right) - g_config.marginRight);
        int y = area.top + std::max(0, (areaHeight - height) / 2);
        x = std::clamp(x, static_cast<int>(area.left), std::max(static_cast<int>(area.left), static_cast<int>(area.right) - width));
        y = std::clamp(y, static_cast<int>(area.top), std::max(static_cast<int>(area.top), static_cast<int>(area.bottom) - height));

        LONG_PTR style = GetWindowLongPtrW(g_status, GWL_STYLE);
        style &= ~WS_CHILD;
        style |= WS_POPUP | WS_VISIBLE;
        SetWindowLongPtrW(g_status, GWL_STYLE, style);
        LONG_PTR exStyle = GetWindowLongPtrW(g_status, GWL_EXSTYLE);
        exStyle |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
        SetWindowLongPtrW(g_status, GWL_EXSTYLE, exStyle);
        SetLayeredWindowAttributes(g_status, 0, 255, LWA_ALPHA);
        SetParent(g_status, nullptr);
        ApplyRoundedRegion(g_status, width, height);
        SetWindowPos(g_status, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        ShowWindow(g_status, SW_SHOWNOACTIVATE);
        InvalidateRect(g_status, nullptr, TRUE);
        return;
    }

    RECT work = WorkArea();
    int x = work.right - width - g_config.marginRight;
    int y = work.bottom - height - g_config.marginBottom;

    LONG_PTR style = GetWindowLongPtrW(g_status, GWL_STYLE);
    style &= ~WS_CHILD;
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(g_status, GWL_STYLE, style);
    LONG_PTR exStyle = GetWindowLongPtrW(g_status, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
    SetWindowLongPtrW(g_status, GWL_EXSTYLE, exStyle);
    SetLayeredWindowAttributes(g_status, 0, 255, LWA_ALPHA);
    SetParent(g_status, nullptr);
    ApplyRoundedRegion(g_status, width, height);
    SetWindowPos(g_status, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    ShowWindow(g_status, SW_SHOWNOACTIVATE);
    InvalidateRect(g_status, nullptr, TRUE);
}
void StartRefresh()
{
    if (g_refreshing.exchange(true))
        return;
    std::thread([] {
        UsageState next = FetchUsage();
        auto* heapState = new UsageState(next);
        PostMessageW(g_owner, kRefreshDone, 0, reinterpret_cast<LPARAM>(heapState));
    }).detach();
}

void DrawStatus(HDC hdc, const RECT& rc)
{
    HBRUSH back = CreateSolidBrush(kPanelBack);
    HPEN border = CreatePen(PS_SOLID, 1, kPanelBorder);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, back));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, border));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, kPanelRadius, kPanelRadius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(border);
    DeleteObject(back);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPanelText);
    HFONT font = CreateUiFont(g_config.pinned ? 16 : 20, FW_BOLD);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));

    if (g_config.pinned)
    {
        RECT first = rc;
        RECT second = rc;
        first.left += kStatusPadX;
        first.right -= kStatusPadX;
        first.top += kStatusPadY;
        first.bottom = rc.top + (rc.bottom - rc.top) / 2;
        second.left += kStatusPadX;
        second.right -= kStatusPadX;
        second.top = first.bottom;
        second.bottom -= kStatusPadY;
        DrawTextW(hdc, (L"5h  " + PercentText(g_usage.fiveHour)).c_str(), -1, &first, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(hdc, (L"1w  " + PercentText(g_usage.weekly)).c_str(), -1, &second, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    else
    {
        RECT textRc = rc;
        textRc.left += kStatusPadX;
        textRc.right -= kStatusPadX;
        DrawTextW(hdc, StatusLineText().c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}


void DrawHoverTip(HDC hdc, const RECT& rc)
{
    HBRUSH back = CreateSolidBrush(kPanelBack);
    HPEN border = CreatePen(PS_SOLID, 1, kPanelBorder);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, back));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, border));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, kPanelRadius, kPanelRadius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(border);
    DeleteObject(back);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPanelText);
    HFONT font = CreateUiFont(16, FW_NORMAL);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
    RECT textRc = rc;
    textRc.left += kHoverPadX;
    textRc.top += kHoverPadY;
    textRc.right -= kHoverPadX;
    textRc.bottom -= kHoverPadY;
    std::wstring text = TooltipText();
    DrawTextW(hdc, text.c_str(), -1, &textRc, DT_LEFT | DT_TOP | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void HideHoverTip()
{
    if (g_hoverTip)
        ShowWindow(g_hoverTip, SW_HIDE);
    g_hoverTipVisible = false;
    if (g_status)
        KillTimer(g_owner, kHoverDelayTimer);
    g_trackingMouse = false;
}

void ShowHoverTip(HWND owner, LPARAM lParam)
{
    if (!g_hoverTip)
        return;
    if (g_hoverTipVisible && IsWindowVisible(g_hoverTip))
        return;

    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ClientToScreen(owner, &pt);
    SIZE measured = MeasureHoverTipSize();
    int width = measured.cx;
    int height = measured.cy;
    int x = pt.x + 14;
    int y = pt.y + 18;

    RECT work = WorkArea();
    if (x + width > work.right)
        x = pt.x - width - 14;
    if (y + height > work.bottom)
        y = pt.y - height - 14;
    x = std::max(static_cast<int>(work.left), x);
    y = std::max(static_cast<int>(work.top), y);

    LONG_PTR style = GetWindowLongPtrW(g_hoverTip, GWL_STYLE);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(g_hoverTip, GWL_STYLE, style);
    LONG_PTR exStyle = GetWindowLongPtrW(g_hoverTip, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_hoverTip, GWL_EXSTYLE, exStyle);
    ApplyRoundedRegion(g_hoverTip, width, height);
    SetWindowPos(g_hoverTip, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    ShowWindow(g_hoverTip, SW_SHOWNOACTIVATE);
    g_hoverTipVisible = true;
    UpdateWindow(g_hoverTip);
    InvalidateRect(g_hoverTip, nullptr, TRUE);

    if (!g_trackingMouse)
    {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = owner;
        if (TrackMouseEvent(&track))
            g_trackingMouse = true;
    }
}

LRESULT CALLBACK HoverTipProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        DrawHoverTip(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
LRESULT CALLBACK StatusProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        DrawStatus(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        UpdateTooltip();
        g_hoverPoint = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (!g_hoverTipVisible)
            SetTimer(g_owner, kHoverDelayTimer, 500, nullptr);
        if (!g_trackingMouse)
        {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            if (TrackMouseEvent(&track))
                g_trackingMouse = true;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_MOUSELEAVE:
        HideHoverTip();
        return 0;
    case WM_LBUTTONDOWN:
        if (!g_config.pinned)
        {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    case WM_RBUTTONUP:
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuRefresh, L"立即刷新");
        AppendMenuW(menu, MF_STRING, kMenuTogglePin, g_config.pinned ? L"取消固定到任务栏" : L"固定到任务栏");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd, &pt);
        g_contextMenuOpen = true;
        SetWindowPos(g_status, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (g_hoverTipVisible && g_hoverTip)
            SetWindowPos(g_hoverTip, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        g_contextMenuOpen = false;
        DestroyMenu(menu);
        if (cmd != kMenuExit)
            ApplyStatusPlacement();
        if (cmd != kMenuExit && g_hoverTipVisible && g_hoverTip)
            SetWindowPos(g_hoverTip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (cmd)
            SendMessageW(g_owner, WM_COMMAND, cmd, 0);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void CreateTooltip()
{
    return;
    g_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_status, nullptr, g_instance, nullptr);
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_SUBCLASS;
    tool.hwnd = g_status;
    tool.uId = 1;
    GetClientRect(g_status, &tool.rect);
    static std::wstring tip = TooltipText();
    tool.lpszText = tip.data();
    SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(g_tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 200);
    SendMessageW(g_tooltip, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    SendMessageW(g_tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 12000);
    SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, 320);
}

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_taskbarCreated)
    {
        g_embed.Restore(g_status);
        ApplyStatusPlacement();
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hwnd, kRefreshTimer, static_cast<UINT>(std::max(15, g_config.refreshSeconds) * 1000), nullptr);
        SetTimer(hwnd, kPlacementTimer, 1000, nullptr);
        StartRefresh();
        return 0;
    case WM_TIMER:
        if (wParam == kRefreshTimer)
            StartRefresh();
        else if (wParam == kPlacementTimer && g_config.pinned && !g_hoverTipVisible && !g_contextMenuOpen)
            ApplyStatusPlacement();
        else if (wParam == kHoverDelayTimer)
        {
            KillTimer(hwnd, kHoverDelayTimer);
            ShowHoverTip(g_status, MAKELPARAM(g_hoverPoint.x, g_hoverPoint.y));
        }
        return 0;
    case kShowExisting:
        ApplyStatusPlacement();
        SetWindowPos(g_status, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        return 0;
    case kRefreshDone:
    {
        auto* next = reinterpret_cast<UsageState*>(lParam);
        if (next)
        {
            g_usage = *next;
            delete next;
        }
        g_refreshing = false;
        UpdateTooltip();
        InvalidateRect(g_status, nullptr, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kMenuRefresh:
            StartRefresh();
            return 0;
        case kMenuTogglePin:
            g_config.pinned = !g_config.pinned;
            SaveConfig();
            ApplyStatusPlacement();
            return 0;
        case kMenuExit:
            DestroyWindow(hwnd);
            return 0;
        default:
            return 0;
        }
    case WM_DESTROY:
        g_embed.Restore(g_status);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool AcquireSingleInstance()
{
    g_singleInstance = CreateMutexW(nullptr, TRUE, L"Global\\CodexAssumptionNative.SingleInstance");
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

void WakeExistingInstance()
{
    HWND existing = FindWindowExW(HWND_MESSAGE, nullptr, L"CodexAssumptionNative.Owner", L"CodexAssumptionNative");
    if (existing)
        PostMessageW(existing, kShowExisting, 0, 0);
}
} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    g_instance = instance;
    if (!AcquireSingleInstance())
    {
        WakeExistingInstance();
        return 0;
    }

    g_appDir = ExeDir();
    LoadConfig();

    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW ownerClass{sizeof(WNDCLASSEXW)};
    ownerClass.lpfnWndProc = OwnerProc;
    ownerClass.hInstance = instance;
    ownerClass.lpszClassName = L"CodexAssumptionNative.Owner";
    RegisterClassExW(&ownerClass);

    WNDCLASSEXW statusClass{sizeof(WNDCLASSEXW)};
    statusClass.lpfnWndProc = StatusProc;
    statusClass.hInstance = instance;
    statusClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    statusClass.lpszClassName = L"CodexAssumptionNative.Status";
    RegisterClassExW(&statusClass);

    WNDCLASSEXW hoverClass{sizeof(WNDCLASSEXW)};
    hoverClass.lpfnWndProc = HoverTipProc;
    hoverClass.hInstance = instance;
    hoverClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    hoverClass.lpszClassName = L"CodexAssumptionNative.HoverTip";
    RegisterClassExW(&hoverClass);

    g_owner = CreateWindowExW(0, ownerClass.lpszClassName, L"CodexAssumptionNative", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    g_status = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, statusClass.lpszClassName, L"CodexAssumption", WS_POPUP, 0, 0, g_config.floatingWidth, g_config.floatingHeight, nullptr, nullptr, instance, nullptr);
    g_hoverTip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, hoverClass.lpszClassName, L"CodexAssumptionHoverTip", WS_POPUP, 0, 0, 230, 78, nullptr, nullptr, instance, nullptr);

    CreateTooltip();
    ApplyStatusPlacement();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}



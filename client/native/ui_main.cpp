#include "common.h"
#include "protocol.h"
#include "resource.h"

#include <bcrypt.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef ORBITLAN_SUPPORTER_EDITION
#define ORBITLAN_SUPPORTER_EDITION 0
#endif
#ifndef ORBITLAN_UI_PREVIEW
#define ORBITLAN_UI_PREVIEW 0
#endif

namespace fs = std::filesystem;

namespace orbitlan {
namespace {

constexpr UINT kTrayMessage = WM_APP + 10;
constexpr UINT kConnectComplete = WM_APP + 11;
constexpr UINT kDisconnectComplete = WM_APP + 12;
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kAnimationTimer = 2;
constexpr UINT kTaskbarCreatedFallback = WM_APP + 13;
constexpr int kWindowWidth = 440;
constexpr int kWindowHeight = 760;
constexpr bool kSupporterEdition = ORBITLAN_SUPPORTER_EDITION != 0;
constexpr bool kUiPreview = ORBITLAN_UI_PREVIEW != 0;
constexpr std::string_view kEditionWireName = kSupporterEdition ? "supporter" : "community";

enum ControlId : int {
    kNameEdit = 1001,
    kCodeEdit,
    kNewCode,
    kConnect,
    kNetworkCode,
    kVirtualIp,
    kSummary,
    kNodes,
    kDisconnect,
    kRelay,
    kCoordinator,
    kPriority,
    kSettings,
    kSettingsDone,
    kPerformance,
    kSupport,
};

struct Node {
    std::wstring id;
    std::wstring name;
    std::wstring ip;
    std::wstring state;
    std::wstring rtt;
};

struct AppState {
    HWND window = nullptr;
    HINSTANCE instance = nullptr;
    HICON icon = nullptr;
    HFONT normal_font = nullptr;
    HFONT small_font = nullptr;
    HFONT title_font = nullptr;
    HFONT heading_font = nullptr;
    HFONT code_font = nullptr;
    HFONT button_font = nullptr;
    HBRUSH field_brush = nullptr;
    HBRUSH nodes_brush = nullptr;
    std::unique_ptr<Gdiplus::Image> earth;
    std::unique_ptr<Gdiplus::Image> header_sheet;
    std::unique_ptr<Gdiplus::Image> moon;
    GUID earth_dimension{};
    std::vector<unsigned> earth_delays;
    unsigned earth_frame = 0;
    ULONGLONG next_earth_frame = 0;
    ULONGLONG animation_started = 0;
    NOTIFYICONDATAW tray{};
    UINT taskbar_created = kTaskbarCreatedFallback;
    bool connected = false;
    bool connecting = false;
    bool is_host = false;
    bool priority = false;
    bool performance_mode = false;
    int relay_mode = 0;
    ULONGLONG relay_suppress_until = 0;
    bool settings_open = false;
    bool install_offered = false;
    ULONGLONG planet_zoom_started = 0;
    double planet_zoom_from = 1.0;
    double planet_zoom_to = 1.0;
    bool node_scroll_dragging = false;
    int node_scroll_drag_offset = 0;
    int node_top_index = 0;
    std::wstring status = L"Offline";
    std::wstring network_summary = L"Waiting for nodes…";
    ULONGLONG copy_notice_until = 0;
    std::vector<Node> nodes;
    HWND name_edit = nullptr;
    HWND code_edit = nullptr;
    HWND new_button = nullptr;
    HWND connect_button = nullptr;
    HWND network_code = nullptr;
    HWND virtual_ip = nullptr;
    HWND summary = nullptr;
    HWND nodes_list = nullptr;
    HWND disconnect_button = nullptr;
    HWND relay_combo = nullptr;
    HWND coordinator_edit = nullptr;
    HWND priority_button = nullptr;
    HWND settings_button = nullptr;
    HWND settings_done = nullptr;
    HWND performance_button = nullptr;
    HWND support_button = nullptr;
};

AppState g;
ULONG_PTR g_gdiplus_token = 0;

COLORREF Color(unsigned red, unsigned green, unsigned blue) {
    return RGB(red, green, blue);
}

COLORREF ThemePanelColor() {
    return kSupporterEdition ? Color(20, 18, 48) : Color(24, 26, 75);
}

COLORREF ThemeDeepPanelColor() {
    return kSupporterEdition ? Color(12, 11, 32) : Color(19, 24, 76);
}

Gdiplus::Color ThemePanel(BYTE alpha = 255) {
    return kSupporterEdition ? Gdiplus::Color(alpha, 20, 18, 48)
                             : Gdiplus::Color(alpha, 24, 26, 75);
}

Gdiplus::Color ThemeDeepPanel(BYTE alpha = 255) {
    return kSupporterEdition ? Gdiplus::Color(alpha, 12, 11, 32)
                             : Gdiplus::Color(alpha, 19, 24, 76);
}

Gdiplus::Color ThemeBorder(BYTE alpha = 255) {
    return kSupporterEdition ? Gdiplus::Color(alpha, 61, 55, 95)
                             : Gdiplus::Color(alpha, 52, 52, 109);
}

void AddRoundedRectangle(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect,
                         Gdiplus::REAL radius) {
    const Gdiplus::REAL diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter,
                0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRounded(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect,
                 Gdiplus::REAL radius, const Gdiplus::Color& color) {
    Gdiplus::GraphicsPath path;
    AddRoundedRectangle(path, rect, radius);
    Gdiplus::SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

void StrokeRounded(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect,
                   Gdiplus::REAL radius, const Gdiplus::Color& color,
                   Gdiplus::REAL width = 1.0f) {
    Gdiplus::GraphicsPath path;
    AddRoundedRectangle(path, rect, radius);
    Gdiplus::Pen pen(color, width);
    graphics.DrawPath(&pen, &path);
}

void DrawSolidCard(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect,
                   Gdiplus::REAL radius) {
    FillRounded(graphics, rect, radius,
                kSupporterEdition ? Gdiplus::Color(255, 34, 31, 72)
                                  : Gdiplus::Color(255, 36, 40, 104));
}

std::wstring ReadWindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

fs::path ConfigPath() {
    fs::path directory = KnownFolder(FOLDERID_LocalAppData) / L"OrbitLan";
    std::error_code error;
    fs::create_directories(directory, error);
    return directory / L"native.ini";
}

std::wstring ReadSetting(const wchar_t* key, std::wstring_view fallback) {
    std::wstring result(4096, L'\0');
    GetPrivateProfileStringW(L"OrbitLan", key, std::wstring(fallback).c_str(), result.data(),
                             static_cast<DWORD>(result.size()), ConfigPath().c_str());
    result.resize(wcslen(result.c_str()));
    return result;
}

void WriteSetting(const wchar_t* key, std::wstring_view value) {
    WritePrivateProfileStringW(L"OrbitLan", key, std::wstring(value).c_str(), ConfigPath().c_str());
}

std::wstring DefaultUserName() {
    std::wstring value(256, L'\0');
    DWORD size = static_cast<DWORD>(value.size());
    if (!GetUserNameW(value.data(), &size) || size <= 1) return L"player";
    value.resize(size - 1);
    return value;
}

std::wstring RandomCode() {
    static constexpr wchar_t alphabet[] = L"abcdefghjkmnpqrstuvwxyz23456789";
    std::array<unsigned char, 6> bytes{};
    BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::wstring code;
    code.reserve(bytes.size());
    for (const unsigned char value : bytes) {
        code.push_back(alphabet[value % (std::size(alphabet) - 1)]);
    }
    return code;
}

HWND MakeControl(const wchar_t* type, const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | style, 0, 0, 10, 10, g.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.normal_font), TRUE);
    return control;
}

void Position(HWND control, int x, int y, int width, int height, bool show = true) {
    SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

void RoundControl(HWND control, int width, int height, int radius) {
    SetWindowRgn(control, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2), TRUE);
}

LRESULT CALLBACK InteractiveControlProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                        UINT_PTR subclass_id, DWORD_PTR) {
    if (message == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, InteractiveControlProc, subclass_id);
    return DefSubclassProc(window, message, wparam, lparam);
}

void UseHandCursor(HWND control) {
    SetWindowSubclass(control, InteractiveControlProc, 1, 0);
}

bool InRect(int x, int y, int left, int top, int right, int bottom) {
    return x >= left && x < right && y >= top && y < bottom;
}

double CurrentPlanetScale() {
    if (g.planet_zoom_started == 0 || g.planet_zoom_from == g.planet_zoom_to) {
        return g.planet_zoom_to;
    }
    const double elapsed = static_cast<double>(GetTickCount64() - g.planet_zoom_started);
    const double progress = std::clamp(elapsed / 680.0, 0.0, 1.0);
    const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
    return g.planet_zoom_from + (g.planet_zoom_to - g.planet_zoom_from) * eased;
}

void BeginPlanetZoom(bool connected) {
    g.planet_zoom_from = CurrentPlanetScale();
    g.planet_zoom_to = connected ? 1.50 : 1.0;
    g.planet_zoom_started = GetTickCount64();
}

bool CopyToClipboard(std::wstring_view text) {
    if (text.empty() || !OpenClipboard(g.window)) return false;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }
    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(destination)[text.size()] = L'\0';
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void ShowCopyNotice(const wchar_t* message) {
    SetWindowTextW(g.summary, message);
    g.copy_notice_until = GetTickCount64() + 1200;
    InvalidateRect(g.window, nullptr, FALSE);
}

struct NodeScrollGeometry {
    int count = 0;
    int visible = 0;
    int maximum_top = 0;
    int top_index = 0;
    int track_top = 3;
    int track_height = 0;
    int thumb_top = 0;
    int thumb_height = 0;
    bool shown = false;
};

NodeScrollGeometry GetNodeScrollGeometry(HWND list) {
    RECT area{};
    GetClientRect(list, &area);
    NodeScrollGeometry geometry;
    geometry.count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    geometry.visible = std::max(1, static_cast<int>((area.bottom - area.top) / 46));
    geometry.maximum_top = std::max(0, geometry.count - geometry.visible);
    geometry.top_index = std::clamp(
        static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0)), 0, geometry.maximum_top);
    geometry.track_height = std::max(0, static_cast<int>(area.bottom - 6));
    geometry.shown = geometry.maximum_top > 0 && geometry.track_height > 0;
    if (!geometry.shown) return geometry;
    geometry.thumb_height = std::max(28, geometry.track_height * geometry.visible /
                                             std::max(1, geometry.count));
    const int travel = std::max(1, geometry.track_height - geometry.thumb_height);
    geometry.thumb_top = geometry.track_top + travel * geometry.top_index /
                                              std::max(1, geometry.maximum_top);
    return geometry;
}

void DrawNodeScrollbar(HWND list) {
    const NodeScrollGeometry geometry = GetNodeScrollGeometry(list);
    if (!geometry.shown) return;
    RECT area{};
    GetClientRect(list, &area);
    HDC dc = GetDC(list);
    if (dc == nullptr) return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush clear(Gdiplus::Color(255, 45, 45, 53));
    graphics.FillRectangle(&clear, area.right - 10, 0, 10, area.bottom);
    FillRounded(graphics,
                Gdiplus::RectF(static_cast<Gdiplus::REAL>(area.right - 8),
                               static_cast<Gdiplus::REAL>(geometry.thumb_top), 6.0f,
                               static_cast<Gdiplus::REAL>(geometry.thumb_height)),
                3.0f, Gdiplus::Color(190, 207, 207, 218));
    ReleaseDC(list, dc);
}

void SetNodeTopIndex(HWND list, int index) {
    const NodeScrollGeometry geometry = GetNodeScrollGeometry(list);
    SendMessageW(list, LB_SETTOPINDEX,
                 static_cast<WPARAM>(std::clamp(index, 0, geometry.maximum_top)), 0);
    InvalidateRect(list, nullptr, TRUE);
}

int NodeIndexAtPoint(HWND list, POINT point) {
    RECT area{};
    GetClientRect(list, &area);
    if (point.x < 0 || point.y < 0 || point.x >= area.right - 12 ||
        point.y >= area.bottom) {
        return LB_ERR;
    }

    const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    if (count <= 0) return LB_ERR;

    const LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0,
                                     MAKELPARAM(point.x, point.y));
    const int index = LOWORD(hit);
    if (HIWORD(hit) != 0 || index < 0 || index >= count ||
        static_cast<size_t>(index) >= g.nodes.size()) {
        return LB_ERR;
    }

    RECT item{};
    if (SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&item)) == LB_ERR ||
        !PtInRect(&item, point)) {
        return LB_ERR;
    }
    return index;
}

int KickIndexAtPoint(HWND list, POINT point) {
    if (!g.is_host) return LB_ERR;
    const int index = NodeIndexAtPoint(list, point);
    if (index == LB_ERR) return LB_ERR;
    RECT item{};
    if (SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
        return LB_ERR;
    }
    const int visual_right = item.right - 12;
    return point.x >= visual_right - 66 && point.x < visual_right - 12 ? index : LB_ERR;
}

void RequestKickNode(size_t index) {
    if (!g.is_host || index >= g.nodes.size()) return;
    const Node node = g.nodes[index];
    const std::wstring prompt = L"Remove " + node.name + L" from this room?";
    if (MessageBoxW(g.window, prompt.c_str(), L"Remove node",
                    MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    const std::string command = "KICK\t" + Base64Encode(WideToUtf8(node.id));
    const PipeReply reply = CallService(command, 5000);
    if (!reply.command_ok) {
        MessageBoxW(g.window, Utf8ToWide(reply.error).c_str(),
                    L"Could not remove node", MB_ICONERROR | MB_OK);
        return;
    }
    g.nodes.erase(g.nodes.begin() + static_cast<std::ptrdiff_t>(index));
    SendMessageW(g.nodes_list, LB_DELETESTRING, static_cast<WPARAM>(index), 0);
    ShowCopyNotice(L"Node removed");
    InvalidateRect(g.window, nullptr, FALSE);
}

LRESULT CALLBACK NodeListProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                              UINT_PTR subclass_id, DWORD_PTR) {
    switch (message) {
        case WM_SETCURSOR: {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            if (NodeIndexAtPoint(window, point) != LB_ERR) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        case WM_MOUSEWHEEL: {
            const int direction = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
            const NodeScrollGeometry geometry = GetNodeScrollGeometry(window);
            SetNodeTopIndex(window, geometry.top_index + direction);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            RECT area{};
            GetClientRect(window, &area);
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            if (x < area.right - 12) {
                if (NodeIndexAtPoint(window, POINT{x, y}) != LB_ERR) break;
                return 0;
            }
            const NodeScrollGeometry geometry = GetNodeScrollGeometry(window);
            if (!geometry.shown) return 0;
            if (y >= geometry.thumb_top && y < geometry.thumb_top + geometry.thumb_height) {
                g.node_scroll_dragging = true;
                g.node_scroll_drag_offset = y - geometry.thumb_top;
                SetCapture(window);
            } else {
                SetNodeTopIndex(window, geometry.top_index +
                    (y < geometry.thumb_top ? -geometry.visible : geometry.visible));
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g.node_scroll_dragging && GetCapture() == window) {
                const NodeScrollGeometry geometry = GetNodeScrollGeometry(window);
                const int travel = std::max(1, geometry.track_height - geometry.thumb_height);
                const int thumb = std::clamp(GET_Y_LPARAM(lparam) - g.node_scroll_drag_offset -
                                                 geometry.track_top,
                                             0, travel);
                SetNodeTopIndex(window, (thumb * geometry.maximum_top + travel / 2) / travel);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (g.node_scroll_dragging) {
                g.node_scroll_dragging = false;
                if (GetCapture() == window) ReleaseCapture();
                return 0;
            }
            {
                RECT area{};
                GetClientRect(window, &area);
                const int x = GET_X_LPARAM(lparam);
                const int y = GET_Y_LPARAM(lparam);
                if (x < area.right - 12) {
                    const int index = NodeIndexAtPoint(window, POINT{x, y});
                    const int kick_index = KickIndexAtPoint(window, POINT{x, y});
                    if (kick_index != LB_ERR) {
                        RequestKickNode(static_cast<size_t>(kick_index));
                    } else if (index != LB_ERR &&
                        CopyToClipboard(g.nodes[static_cast<size_t>(index)].ip)) {
                        ShowCopyNotice(L"IP copied");
                    }
                }
            }
            break;
        case WM_CAPTURECHANGED:
            g.node_scroll_dragging = false;
            break;
        case WM_PAINT: {
            const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
            DrawNodeScrollbar(window);
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(window, NodeListProc, subclass_id);
            break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void Layout();

void SetUiState(bool connected, bool connecting) {
    if (g.connected != connected) BeginPlanetZoom(connected);
    g.connected = connected;
    g.connecting = connecting;
    if (!connected && !connecting) g.is_host = false;
    Layout();
    const bool main_screen = !g.settings_open;
    const bool show_setup = main_screen && !connected;
    ShowWindow(g.name_edit, show_setup ? SW_SHOW : SW_HIDE);
    ShowWindow(g.code_edit, show_setup ? SW_SHOW : SW_HIDE);
    ShowWindow(g.new_button, show_setup ? SW_SHOW : SW_HIDE);
    ShowWindow(g.connect_button, show_setup ? SW_SHOW : SW_HIDE);
    ShowWindow(g.network_code, SW_HIDE);
    ShowWindow(g.virtual_ip, SW_HIDE);
    ShowWindow(g.summary, SW_HIDE);
    ShowWindow(g.nodes_list, SW_HIDE);
    ShowWindow(g.disconnect_button, SW_HIDE);
    ShowWindow(g.relay_combo, g.settings_open ? SW_SHOW : SW_HIDE);
    ShowWindow(g.priority_button, g.settings_open ? SW_SHOW : SW_HIDE);
    ShowWindow(g.coordinator_edit, g.settings_open ? SW_SHOW : SW_HIDE);
    ShowWindow(g.settings_done, g.settings_open ? SW_SHOW : SW_HIDE);
    ShowWindow(g.performance_button, g.settings_open ? SW_SHOW : SW_HIDE);
    ShowWindow(g.support_button, g.settings_open ? SW_SHOW : SW_HIDE);
    EnableWindow(g.connect_button, !connecting);
    SetWindowTextW(g.connect_button, connecting ? L"Connecting…" : L"Connect");
    InvalidateRect(g.window, nullptr, TRUE);
}

void ReleaseVisuals() {
    KillTimer(g.window, kAnimationTimer);
    g.earth.reset();
    g.header_sheet.reset();
    g.moon.reset();
    g.earth_delays.clear();
    g.earth_frame = 0;
}

void TrimWorkingSet() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

fs::path VisualAsset(const wchar_t* name) {
    const fs::path direct = ExecutableDirectory() / L"assets" / name;
    if (FileExists(direct)) return direct;
    return ExecutableDirectory() / L"support" / L"assets" / name;
}

void LoadVisuals() {
    if (g.earth != nullptr) return;
    const fs::path image_path = VisualAsset(L"earth-clouds.gif");
    if (FileExists(image_path)) {
        auto image = std::make_unique<Gdiplus::Image>(image_path.c_str(), FALSE);
        if (image->GetLastStatus() == Gdiplus::Ok && image->GetFrameDimensionsCount() > 0) {
            image->GetFrameDimensionsList(&g.earth_dimension, 1);
            const unsigned frames = image->GetFrameCount(&g.earth_dimension);
            g.earth_delays.assign(frames, 100);
            const UINT bytes = image->GetPropertyItemSize(PropertyTagFrameDelay);
            if (bytes > 0) {
                std::vector<unsigned char> storage(bytes);
                auto* item = reinterpret_cast<Gdiplus::PropertyItem*>(storage.data());
                if (image->GetPropertyItem(PropertyTagFrameDelay, bytes, item) == Gdiplus::Ok &&
                    item->length >= frames * sizeof(UINT)) {
                    const auto* delays = static_cast<const UINT*>(item->value);
                    for (unsigned i = 0; i < frames; ++i) {
                        g.earth_delays[i] = std::clamp(delays[i] * 10u, 40u, 500u);
                    }
                }
            }
            g.earth = std::move(image);
            g.earth_frame = 0;
            g.earth->SelectActiveFrame(&g.earth_dimension, 0);
            g.next_earth_frame = GetTickCount64() + g.earth_delays.front();
        }
    }
    const fs::path header_path = VisualAsset(L"earth-header-sheet.png");
    if (FileExists(header_path)) {
        auto image = std::make_unique<Gdiplus::Image>(header_path.c_str(), FALSE);
        if (image->GetLastStatus() == Gdiplus::Ok) g.header_sheet = std::move(image);
    }
    if (kSupporterEdition) {
        const fs::path moon_path = VisualAsset(L"moon-supporter.png");
        if (FileExists(moon_path)) {
            auto image = std::make_unique<Gdiplus::Image>(moon_path.c_str(), FALSE);
            if (image->GetLastStatus() == Gdiplus::Ok) g.moon = std::move(image);
        }
    }
    g.animation_started = GetTickCount64();
    SetTimer(g.window, kAnimationTimer, 50, nullptr);
}

void AdvanceVisuals() {
    const ULONGLONG now = GetTickCount64();
    if (g.earth != nullptr && !g.earth_delays.empty() && now >= g.next_earth_frame) {
        g.earth_frame = (g.earth_frame + 1) % static_cast<unsigned>(g.earth_delays.size());
        g.earth->SelectActiveFrame(&g.earth_dimension, g.earth_frame);
        g.next_earth_frame = now + g.earth_delays[g.earth_frame];
    }
    InvalidateRect(g.window, nullptr, FALSE);
}

void Layout() {
    RECT area{};
    GetClientRect(g.window, &area);
    const int width = area.right;
    const int content = width - 52;
    Position(g.settings_button, 0, 0, 0, 0, false);
    if (g.settings_open) {
        Position(g.relay_combo, 58, 190, 324, 40);
        RoundControl(g.relay_combo, 324, 40, 10);
        Position(g.coordinator_edit, 72, 303, 296, 22);
        Position(g.performance_button, 58, 430, 324, 40);
        RoundControl(g.performance_button, 324, 40, 12);
        Position(g.priority_button, 58, 546, 324, 42);
        RoundControl(g.priority_button, 324, 42, 12);
        Position(g.support_button, 58, 610, 157, 42);
        RoundControl(g.support_button, 157, 42, 12);
        Position(g.settings_done, 225, 610, 157, 42);
        RoundControl(g.settings_done, 157, 42, 12);
    } else if (!g.connected) {
        Position(g.name_edit, 40, 466, content - 28, 22);
        Position(g.code_edit, 40, 551, content - 108, 22);
        Position(g.new_button, width - 96, 539, 70, 46);
        RoundControl(g.new_button, 70, 46, 12);
        Position(g.connect_button, 26, 612, content, 52);
        RoundControl(g.connect_button, content, 52, 14);
    } else {
        Position(g.network_code, 42, 347, content - 28, 32);
        Position(g.virtual_ip, 95, 383, 130, 22);
        Position(g.summary, 245, 383, 153, 22);
        Position(g.nodes_list, 32, 446, content - 12, 184);
        Position(g.disconnect_button, 26, 650, content, 44);
        RoundControl(g.disconnect_button, content, 44, 12);
    }
}

void AddTrayIcon() {
    g.tray = {};
    g.tray.cbSize = sizeof(g.tray);
    g.tray.hWnd = g.window;
    g.tray.uID = 1;
    g.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
    g.tray.uCallbackMessage = kTrayMessage;
    g.tray.hIcon = g.icon;
    g.tray.guidItem = {0x39494a64, 0x199b, 0x4c5c, {0xa6, 0x75, 0xed, 0xa9, 0xef, 0xf8, 0xe2, 0xc1}};
    wcscpy_s(g.tray.szTip, L"OrbitLan — Offline");
    Shell_NotifyIconW(NIM_ADD, &g.tray);
    g.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g.tray);
}

void UpdateTray() {
    std::wstring tip = L"OrbitLan — " + g.status;
    wcsncpy_s(g.tray.szTip, tip.c_str(), _TRUNCATE);
    g.tray.uFlags = NIF_TIP | NIF_GUID;
    Shell_NotifyIconW(NIM_MODIFY, &g.tray);
}

void ShowMainWindow() {
    LoadVisuals();
    ShowWindow(g.window, SW_RESTORE);
    SetForegroundWindow(g.window);
}

std::string JsonString(const std::string& json, std::string_view key, size_t from = 0) {
    const std::string marker = "\"" + std::string(key) + "\"";
    size_t position = json.find(marker, from);
    if (position == std::string::npos) return {};
    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return {};
    position = json.find('"', position + 1);
    if (position == std::string::npos) return {};
    ++position;
    std::string result;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char ch = json[position];
        if (escaped) {
            switch (ch) {
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: result.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            break;
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

std::string JsonNumber(const std::string& json, std::string_view key, size_t from) {
    const std::string marker = "\"" + std::string(key) + "\"";
    size_t position = json.find(marker, from);
    if (position == std::string::npos) return {};
    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return {};
    ++position;
    while (position < json.size() && json[position] == ' ') ++position;
    const size_t end = json.find_first_not_of("0123456789.-", position);
    return json.substr(position, end - position);
}

bool JsonBool(const std::string& json, std::string_view key, size_t from = 0) {
    const std::string marker = "\"" + std::string(key) + "\"";
    size_t position = json.find(marker, from);
    if (position == std::string::npos) return false;
    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return false;
    ++position;
    while (position < json.size() && json[position] == ' ') ++position;
    return json.compare(position, 4, "true") == 0;
}

void ApplyStatusJson(const std::string& json) {
    const std::wstring code = Utf8ToWide(JsonString(json, "code"));
    const std::wstring ip = Utf8ToWide(JsonString(json, "myIP"));
    const std::wstring summary = Utf8ToWide(JsonString(json, "summary"));
    SetWindowTextW(g.network_code, code.c_str());
    SetWindowTextW(g.virtual_ip, ip.c_str());
    g.network_summary = summary;
    g.is_host = JsonBool(json, "isHost");
    if (g.copy_notice_until == 0) SetWindowTextW(g.summary, summary.c_str());

    g.nodes.clear();
    const size_t peers = json.find("\"peers\"");
    const size_t array_start = peers == std::string::npos ? std::string::npos : json.find('[', peers);
    const size_t array_end = array_start == std::string::npos ? std::string::npos : json.find(']', array_start);
    size_t cursor = array_start;
    while (cursor != std::string::npos && array_end != std::string::npos && cursor < array_end) {
        const size_t object = json.find('{', cursor + 1);
        if (object == std::string::npos || object >= array_end) break;
        const size_t end = json.find('}', object);
        if (end == std::string::npos || end > array_end) break;
        Node node;
        node.id = Utf8ToWide(JsonString(json, "id", object));
        node.name = Utf8ToWide(JsonString(json, "name", object));
        node.ip = Utf8ToWide(JsonString(json, "ip", object));
        node.state = Utf8ToWide(JsonString(json, "state", object));
        const std::string rtt = JsonNumber(json, "rttMs", object);
        node.rtt = Utf8ToWide(rtt.empty() ? "—" : rtt + " ms");
        g.nodes.push_back(std::move(node));
        cursor = end;
    }
    SendMessageW(g.nodes_list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        SendMessageW(g.nodes_list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(g.nodes[i].name.c_str()));
    }
    SendMessageW(g.nodes_list, LB_SETTOPINDEX, 0, 0);
}

void SetStatus(std::wstring value) {
    g.status = std::move(value);
    UpdateTray();
    InvalidateRect(g.window, nullptr, FALSE);
}

void PollStatus() {
    if (g.connecting) return;
    const PipeReply reply = CallService("STATUS", 650);
    if (!reply.transport_ok) {
        SetStatus(L"Setup needed");
        SetUiState(false, false);
        return;
    }
    const auto fields = Split(reply.payload, '\t');
    if (!reply.command_ok || fields.empty() || fields[0] == "OFFLINE") {
        SetStatus(L"Offline");
        SetUiState(false, false);
        return;
    }
    if (fields[0] == "CONNECTING") {
        SetStatus(L"Connecting");
        return;
    }
    if (fields[0] == "CONNECTED" && fields.size() >= 2) {
        std::string json;
        if (Base64Decode(fields[1], json)) ApplyStatusJson(json);
        SetStatus(L"Connected");
        SetUiState(true, false);
    }
}

void SaveConfiguration() {
    WriteSetting(L"Name", ReadWindowText(g.name_edit));
    WriteSetting(L"Coordinator", ReadWindowText(g.coordinator_edit));
    WriteSetting(L"Relay", g.relay_mode == 1 ? L"off"
                              : kSupporterEdition && g.relay_mode == 2 ? L"on" : L"auto");
    WriteSetting(L"Priority", g.priority ? L"1" : L"0");
    WriteSetting(L"Performance", g.performance_mode ? L"1" : L"0");
}

std::string SelectedRelay() {
    return g.relay_mode == 1 ? "off"
             : kSupporterEdition && g.relay_mode == 2 ? "on" : "auto";
}

void ChooseRelayMode() {
    if (GetTickCount64() < g.relay_suppress_until) return;
    RECT anchor{};
    GetWindowRect(g.relay_combo, &anchor);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g.relay_mode == 0 ? MF_CHECKED : 0), 20, L"Auto — direct first");
    AppendMenuW(menu, MF_STRING | (g.relay_mode == 1 ? MF_CHECKED : 0), 21, L"Off — direct only");
    if (kSupporterEdition) {
        AppendMenuW(menu, MF_STRING | (g.relay_mode == 2 ? MF_CHECKED : 0), 22,
                    L"On — force relay");
    }
    SetForegroundWindow(g.window);
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN |
                                      TPM_TOPALIGN | TPM_RIGHTBUTTON,
                                       anchor.left, anchor.bottom, 0, g.window, nullptr);
    DestroyMenu(menu);
    PostMessageW(g.window, WM_NULL, 0, 0);
    const UINT last_choice = kSupporterEdition ? 22 : 21;
    if (choice < 20 || choice > last_choice) {
        g.relay_suppress_until = GetTickCount64() + 300;
        return;
    }
    g.relay_mode = static_cast<int>(choice - 20);
    SetWindowTextW(g.relay_combo, g.relay_mode == 0 ? L"Relay: Auto  ▾"
                                    : g.relay_mode == 1 ? L"Relay: Off  ▾"
                                                        : L"Relay: On  ▾");
    SaveConfiguration();
}

void ToggleSettings(bool open) {
    if (!open) SaveConfiguration();
    g.settings_open = open;
    Layout();
    SetUiState(g.connected, g.connecting);
}

void StartConnection() {
    if (g.connecting) return;
    const std::wstring name = ReadWindowText(g.name_edit);
    std::wstring code = ReadWindowText(g.code_edit);
    const std::wstring server = ReadWindowText(g.coordinator_edit);
    std::transform(code.begin(), code.end(), code.begin(), towlower);
    SetWindowTextW(g.code_edit, code.c_str());
    if (name.empty() || name.size() > 64 || code.size() < 3 || code.size() > 64) {
        MessageBoxW(g.window, L"Enter a name and a network code containing 3–64 characters.",
                    L"OrbitLan", MB_OK | MB_ICONWARNING);
        return;
    }
    SaveConfiguration();
    const std::string request = "CONNECT\t" + Base64Encode(WideToUtf8(name)) + "\t" +
                                Base64Encode(WideToUtf8(code)) + "\t" +
                                Base64Encode(WideToUtf8(server)) + "\t" + SelectedRelay() +
                                "\t" + std::string(kEditionWireName);
    SetStatus(L"Connecting");
    SetUiState(false, true);
    const HWND target = g.window;
    std::thread([target, request] {
        auto result = std::make_unique<PipeReply>(CallService(request, 45000));
        if (!PostMessageW(target, kConnectComplete, 0, reinterpret_cast<LPARAM>(result.get()))) return;
        result.release();
    }).detach();
}

void StopConnection() {
    if (g.connecting) return;
    g.connecting = true;
    SetStatus(L"Disconnecting");
    const HWND target = g.window;
    std::thread([target] {
        auto result = std::make_unique<PipeReply>(CallService("DISCONNECT", 8000));
        if (!PostMessageW(target, kDisconnectComplete, 0, reinterpret_cast<LPARAM>(result.get()))) return;
        result.release();
    }).detach();
}

void OfferServiceInstall() {
    if (g.install_offered || CallService("PING", 350).command_ok) return;
    g.install_offered = true;
    fs::path setup = ExecutableDirectory() / L"support" / L"OrbitLan.Setup.exe";
    if (!FileExists(setup)) setup = ExecutableDirectory() / L"OrbitLan.Setup.exe";
    if (!FileExists(setup)) setup = ExecutableDirectory() / L"OrbitLan.Uninstall.exe";
    if (!FileExists(setup)) {
        MessageBoxW(g.window,
                    L"OrbitLan's support files are missing. Extract the complete release before opening OrbitLan.",
                    L"OrbitLan service required", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const bool portable = setup.parent_path().filename() == L"support";
    const auto launched = reinterpret_cast<INT_PTR>(
        ShellExecuteW(g.window, L"runas", setup.c_str(),
                      portable ? L"--portable" : L"--install",
                      ExecutableDirectory().c_str(), SW_SHOWNORMAL));
    if (launched <= 32) {
        MessageBoxW(g.window,
                    L"OrbitLan setup was not started. Approve the Windows administrator prompt and try again.",
                    L"OrbitLan setup needed", MB_OK | MB_ICONWARNING);
    }
}

void DrawButton(const DRAWITEMSTRUCT* draw) {
    const bool hot = (draw->itemState & ODS_HOTLIGHT) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const COLORREF surrounding = (draw->CtlID == kRelay || draw->CtlID == kPriority ||
                                  draw->CtlID == kSettingsDone || draw->CtlID == kPerformance ||
                                  draw->CtlID == kSupport)
                                     ? ThemePanelColor()
                                     : (kSupporterEdition ? Color(30, 27, 58) : Color(55, 42, 99));
    HBRUSH surrounding_brush = CreateSolidBrush(surrounding);
    FillRect(draw->hDC, &draw->rcItem, surrounding_brush);
    DeleteObject(surrounding_brush);
    Gdiplus::Graphics graphics(draw->hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::RectF bounds(0.0f, 0.0f,
                                static_cast<Gdiplus::REAL>(draw->rcItem.right),
                                static_cast<Gdiplus::REAL>(draw->rcItem.bottom));
    Gdiplus::GraphicsPath path;
    AddRoundedRectangle(path, bounds, draw->CtlID == kConnect ? 14.0f : 12.0f);
    if (draw->CtlID == kConnect) {
        const BYTE alpha = disabled ? 130 : pressed ? 225 : hot ? 242 : 255;
        Gdiplus::LinearGradientBrush gradient(
            bounds,
            kSupporterEdition ? Gdiplus::Color(alpha, 67, 73, 145)
                              : Gdiplus::Color(alpha, 78, 127, 234),
            kSupporterEdition ? Gdiplus::Color(alpha, 99, 83, 142)
                              : Gdiplus::Color(alpha, 120, 104, 220),
            Gdiplus::LinearGradientModeHorizontal);
        graphics.FillPath(&gradient, &path);
    } else {
        const Gdiplus::Color background = kSupporterEdition
            ? Gdiplus::Color(255, pressed || hot ? 50 : 35,
                             pressed || hot ? 46 : 32,
                             pressed || hot ? 82 : 67)
            : Gdiplus::Color(255, pressed || hot ? 52 : 37,
                             pressed || hot ? 52 : 37,
                             pressed || hot ? 109 : 87);
        Gdiplus::SolidBrush brush(background);
        graphics.FillPath(&brush, &path);
    }
    if ((draw->itemState & ODS_FOCUS) != 0) {
        Gdiplus::Pen focus(Gdiplus::Color(190, 142, 131, 238), 1.0f);
        graphics.DrawPath(&focus, &path);
    }

    RECT text_rect = draw->rcItem;
    std::wstring text = ReadWindowText(draw->hwndItem);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, disabled ? Color(130, 133, 169) : Color(247, 246, 251));
    SelectObject(draw->hDC, draw->CtlID == kConnect ? g.button_font : g.normal_font);
    DrawTextW(draw->hDC, text.c_str(), -1, &text_rect,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

void DrawNodeRow(HDC dc, const RECT& row, size_t item_id) {
    if (item_id >= g.nodes.size()) return;
    const int saved_dc = SaveDC(dc);
    IntersectClipRect(dc, row.left, row.top, row.right, row.bottom);
    const Node& node = g.nodes[item_id];
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const int visual_right = row.right - 12;
    const float width = static_cast<float>(visual_right - row.left);
    const float height = static_cast<float>(row.bottom - row.top);
    const float left = static_cast<float>(row.left);
    const float top = static_cast<float>(row.top);
    const Gdiplus::RectF row_bounds(left, top, width, height);
    FillRounded(graphics, row_bounds, 10.0f,
                kSupporterEdition ? Gdiplus::Color(255, 43, 39, 85)
                                  : Gdiplus::Color(255, 46, 50, 117));
    const Gdiplus::Color node_color = node.state == L"direct"
        ? Gdiplus::Color(255, 76, 255, 159)
        : Gdiplus::Color(255, 245, 196, 74);
    Gdiplus::SolidBrush dot(node_color);
    graphics.FillEllipse(&dot, left + 12.0f, top + height / 2.0f - 5.0f, 10.0f, 10.0f);

    const int content_right = g.is_host ? visual_right - 166 : visual_right - 112;
    const int metrics_left = g.is_host ? visual_right - 162 : visual_right - 108;
    const int metrics_right = g.is_host ? visual_right - 70 : visual_right - 12;
    RECT name_rect{row.left + 34, row.top + 2, content_right, row.top + 22};
    RECT ip_rect{row.left + 34, row.top + 20, content_right, row.top + 40};
    RECT state_rect{metrics_left, row.top + 2, metrics_right, row.top + 22};
    RECT rtt_rect{metrics_left, row.top + 20, metrics_right, row.top + 40};
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.normal_font);
    SetTextColor(dc, Color(247, 246, 251));
    DrawTextW(dc, node.name.c_str(), -1, &name_rect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, g.small_font);
    SetTextColor(dc, Color(191, 195, 232));
    DrawTextW(dc, node.ip.c_str(), -1, &ip_rect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SetTextColor(dc, node.state == L"direct" ? Color(76, 255, 159) : Color(245, 196, 74));
    DrawTextW(dc, node.state.c_str(), -1, &state_rect,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    SetTextColor(dc, Color(191, 195, 232));
    DrawTextW(dc, node.rtt.c_str(), -1, &rtt_rect,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    if (g.is_host) {
        const Gdiplus::RectF kick_bounds(
            static_cast<Gdiplus::REAL>(visual_right - 64),
            static_cast<Gdiplus::REAL>(row.top + 8), 50.0f, 26.0f);
        FillRounded(graphics, kick_bounds, 13.0f,
                    kSupporterEdition ? Gdiplus::Color(255, 61, 45, 78)
                                      : Gdiplus::Color(255, 50, 45, 89));
        RECT kick_text{visual_right - 64, row.top + 8,
                       visual_right - 14, row.top + 34};
        SelectObject(dc, g.small_font);
        SetTextColor(dc, Color(246, 184, 201));
        DrawTextW(dc, L"Kick", -1, &kick_text,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    RestoreDC(dc, saved_dc);
}

void DrawNode(const DRAWITEMSTRUCT* draw) {
    DrawNodeRow(draw->hDC, draw->rcItem, draw->itemID);
}

void PaintWindow(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT area{};
    GetClientRect(window, &area);
    HDC buffer_dc = CreateCompatibleDC(dc);
    HBITMAP buffer_bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
    HGDIOBJ old_bitmap = SelectObject(buffer_dc, buffer_bitmap);
    HDC target = buffer_dc;
    TRIVERTEX vertices[2] = {
        {0, 0, 0x0900, 0x0e00, 0x4300, 0xff00},
        {area.right, area.bottom, 0x4000, 0x3100, 0x6b00, 0xff00},
    };
    if (kSupporterEdition) {
        vertices[0].Red = 0x0500;
        vertices[0].Green = 0x0600;
        vertices[0].Blue = 0x1900;
        vertices[1].Red = 0x1d00;
        vertices[1].Green = 0x1900;
        vertices[1].Blue = 0x3800;
    }
    GRADIENT_RECT gradient{0, 1};
    GradientFill(target, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);

    uint32_t seed = 0x4f524249;
    const double star_shift = static_cast<double>((GetTickCount64() - g.animation_started) % 18000) / 18000.0;
    HBRUSH star_dim = CreateSolidBrush(kSupporterEdition ? Color(110, 109, 144)
                                                         : Color(133, 140, 202));
    HBRUSH star_bright = CreateSolidBrush(kSupporterEdition ? Color(223, 216, 184)
                                                            : Color(201, 199, 255));
    for (int i = 0; i < 48; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const int x = static_cast<int>(seed % std::max(1L, area.right));
        seed = seed * 1664525u + 1013904223u;
        const int base_y = static_cast<int>(seed % std::max(1L, area.bottom - 16));
        const int speed = 18 + (i % 5) * 7;
        const int y = 8 + (base_y + static_cast<int>(star_shift * area.bottom * speed / 18.0)) %
                              std::max(1L, area.bottom - 16);
        const int size = i % 9 == 0 ? 2 : 1;
        RECT point{x, y, x + size, y + size};
        FillRect(target, &point, i % 5 == 0 ? star_bright : star_dim);
    }
    DeleteObject(star_dim);
    DeleteObject(star_bright);

    Gdiplus::Graphics graphics(target);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    const ULONGLONG animation_elapsed = GetTickCount64() - g.animation_started;
    if (kSupporterEdition && !g.performance_mode && animation_elapsed < 2400) {
        const double progress = static_cast<double>(animation_elapsed) / 2400.0;
        const Gdiplus::REAL trail_x = static_cast<Gdiplus::REAL>(370.0 - progress * 330.0);
        const Gdiplus::REAL trail_y = static_cast<Gdiplus::REAL>(92.0 + progress * 92.0);
        const BYTE alpha = static_cast<BYTE>((1.0 - progress) * 210.0);
        Gdiplus::Pen trail(Gdiplus::Color(alpha, 255, 220, 151), 2.3f);
        graphics.DrawLine(&trail, trail_x, trail_y, trail_x + 54.0f, trail_y - 15.0f);
        Gdiplus::SolidBrush head(Gdiplus::Color(alpha, 255, 244, 207));
        graphics.FillEllipse(&head, trail_x - 3.0f, trail_y - 3.0f, 6.0f, 6.0f);
    }
    if (g.earth != nullptr) {
        const double visual_scale = CurrentPlanetScale();
        const int earth_size = static_cast<int>(286.0 * visual_scale);
        const int earth_left = (area.right - earth_size) / 2;
        const double sway = g.performance_mode ? 0.0
            : std::sin(static_cast<double>(GetTickCount64() - g.animation_started) / 1080.0) * 5.0;
        const int zoom_drop = static_cast<int>(std::max(0.0, visual_scale - 1.0) * 56.0);
        const int earth_top = 194 - earth_size / 2 + zoom_drop + static_cast<int>(sway);
        if (kSupporterEdition && !g.performance_mode && animation_elapsed < 2600) {
            const double phase = static_cast<double>(animation_elapsed) / 2600.0;
            const int radius = earth_size / 2 + 8 + static_cast<int>(phase * 58.0);
            const BYTE alpha = static_cast<BYTE>((1.0 - phase) * 150.0);
            Gdiplus::Pen welcome(Gdiplus::Color(alpha, 255, 207, 112), 3.0f);
            graphics.DrawEllipse(&welcome, earth_left + earth_size / 2 - radius,
                                 earth_top + earth_size / 2 - radius, radius * 2, radius * 2);
        }
        if (g.connecting) {
            const double phase = static_cast<double>((GetTickCount64() - g.animation_started) % 1400) / 1400.0;
            const int radius = 94 + static_cast<int>(phase * 95);
            const BYTE alpha = static_cast<BYTE>((1.0 - phase) * 175);
            Gdiplus::Pen pulse(Gdiplus::Color(alpha, 126, 145, 255), 4.0f);
            graphics.DrawEllipse(&pulse, earth_left + earth_size / 2 - radius,
                                 earth_top + earth_size / 2 - radius, radius * 2, radius * 2);
        }
        struct MeshPoint {
            Gdiplus::REAL x;
            Gdiplus::REAL y;
            Gdiplus::REAL size;
            double depth;
        };
        std::array<MeshPoint, 8> mesh{};
        size_t mesh_count = 0;
        if (g.connected && !g.nodes.empty() && !g.performance_mode) {
            static constexpr std::array<double, 8> latitudes{
                -0.64, 0.08, 0.62, -0.22, 0.38, -0.48, 0.72, 0.18};
            const double time = static_cast<double>(GetTickCount64() - g.animation_started) / 1000.0;
            mesh_count = std::clamp<size_t>(g.nodes.size() + 2, 2, mesh.size());
            const double center_x = earth_left + earth_size / 2.0;
            const double center_y = earth_top + earth_size / 2.0;
            for (size_t i = 0; i < mesh_count; ++i) {
                const double longitude = i * (6.283185307 / mesh.size()) + (i % 2) * 0.34 +
                                         time * (0.19 + (i % 3) * 0.012);
                const double latitude = latitudes[i] + std::sin(time * 0.31 + i * 0.8) * 0.06;
                const double cos_latitude = std::cos(latitude);
                const double depth = std::cos(longitude) * cos_latitude;
                const double orbit_radius = (122.0 + (i % 3) * 4.0) * visual_scale;
                mesh[i] = {
                    static_cast<Gdiplus::REAL>(center_x + std::sin(longitude) * cos_latitude * orbit_radius),
                    static_cast<Gdiplus::REAL>(center_y - std::sin(latitude) * 103.0 * visual_scale +
                                               depth * 10.0 * visual_scale),
                    static_cast<Gdiplus::REAL>((8.5 + (depth + 1.0) * 2.4) * visual_scale),
                    depth,
                };
            }
        }
        static constexpr std::array<std::pair<size_t, size_t>, 14> links{{
            {0, 1}, {1, 2}, {2, 0}, {2, 3}, {3, 0}, {3, 4}, {4, 1},
            {4, 5}, {5, 2}, {5, 6}, {6, 3}, {6, 7}, {7, 4}, {7, 0},
        }};
        const auto draw_mesh_layer = [&](bool front) {
            for (size_t index = 0; index < links.size(); ++index) {
                const auto [a, b] = links[index];
                if (a >= mesh_count || b >= mesh_count) continue;
                const double depth = (mesh[a].depth + mesh[b].depth) / 2.0;
                if ((depth >= 0.0) != front) continue;
                const BYTE alpha = static_cast<BYTE>(front ? 145 + std::max(0.0, depth) * 70.0
                                                            : 44 + (depth + 1.0) * 24.0);
                Gdiplus::Pen line(Gdiplus::Color(alpha, 69, 242, 154), 1.7f);
                Gdiplus::REAL dash_pattern[]{2.2f, 2.8f};
                line.SetDashPattern(dash_pattern, 2);
                line.SetDashOffset(static_cast<Gdiplus::REAL>(-(GetTickCount64() / 145.0 + index * 0.7)));
                graphics.DrawLine(&line, mesh[a].x, mesh[a].y, mesh[b].x, mesh[b].y);
            }
            for (size_t i = 0; i < mesh_count; ++i) {
                if ((mesh[i].depth >= 0.0) != front) continue;
                const BYTE alpha = static_cast<BYTE>(front ? 205 + std::max(0.0, mesh[i].depth) * 45.0
                                                            : 82 + (mesh[i].depth + 1.0) * 34.0);
                Gdiplus::SolidBrush fill(Gdiplus::Color(alpha, 76, 255, 159));
                Gdiplus::Pen edge(Gdiplus::Color(alpha, 7, 92, 60), 1.6f);
                const Gdiplus::REAL half = mesh[i].size / 2.0f;
                graphics.FillEllipse(&fill, mesh[i].x - half, mesh[i].y - half,
                                     mesh[i].size, mesh[i].size);
                graphics.DrawEllipse(&edge, mesh[i].x - half, mesh[i].y - half,
                                     mesh[i].size, mesh[i].size);
            }
        };
        const double moon_time = static_cast<double>(animation_elapsed) / 1000.0;
        const double moon_angle = moon_time * 0.22 - 0.9;
        const double moon_depth = std::cos(moon_angle);
        const double moon_proximity = (moon_depth + 1.0) / 2.0;
        const Gdiplus::REAL moon_size = static_cast<Gdiplus::REAL>(
            (90.0 + moon_proximity * 110.0) * visual_scale);
        const Gdiplus::REAL moon_x = static_cast<Gdiplus::REAL>(
            earth_left + earth_size / 2.0 + std::sin(moon_angle) * 158.0 * visual_scale);
        const Gdiplus::REAL moon_y = static_cast<Gdiplus::REAL>(
            earth_top + earth_size / 2.0 - 24.0 * visual_scale +
            moon_depth * 42.0 * visual_scale);
        const auto draw_moon = [&](bool front) {
            if (g.moon == nullptr || g.performance_mode || ((moon_depth >= 0.0) != front)) return;
            if (g.connected && front) return;
            Gdiplus::ImageAttributes attributes;
            const float moon_alpha = g.connected
                ? static_cast<float>(std::clamp(-moon_depth * 0.72, 0.0, 0.62))
                : (front ? 1.0f : 0.48f);
            Gdiplus::ColorMatrix matrix = {{
                {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 0.0f, moon_alpha, 0.0f},
                {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
            }};
            attributes.SetColorMatrix(&matrix);
            const Gdiplus::RectF destination(moon_x - moon_size / 2.0f,
                                              moon_y - moon_size / 2.0f,
                                              moon_size, moon_size);
            graphics.DrawImage(g.moon.get(), destination, 0.0f, 0.0f,
                               static_cast<Gdiplus::REAL>(g.moon->GetWidth()),
                               static_cast<Gdiplus::REAL>(g.moon->GetHeight()),
                               Gdiplus::UnitPixel, &attributes);
        };
        draw_moon(false);
        draw_mesh_layer(false);
        graphics.DrawImage(g.earth.get(), earth_left, earth_top, earth_size, earth_size);
        draw_mesh_layer(true);
        draw_moon(true);
    }

    if (g.header_sheet != nullptr) {
        const int frame = static_cast<int>(g.earth_frame % 100);
        graphics.DrawImage(g.header_sheet.get(), Gdiplus::Rect(26, 18, 29, 29),
                           (frame % 10) * 48, (frame / 10) * 48, 48, 48,
                           Gdiplus::UnitPixel);
    } else {
        DrawIconEx(target, 26, 18, g.icon, 29, 29, 0, nullptr, DI_NORMAL);
    }
    SetBkMode(target, TRANSPARENT);
    SetTextColor(target, Color(247, 246, 251));
    SelectObject(target, g.title_font);
    RECT title{54, 17, 164, 48};
    DrawTextW(target, L"rbitLan", -1, &title, DT_SINGLELINE | DT_VCENTER);
    if (kSupporterEdition) {
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(255, 217, 139));
        RECT supporter{57, 43, 154, 58};
        DrawTextW(target, L"SUPPORTER", -1, &supporter, DT_SINGLELINE | DT_VCENTER);
    }

    const Gdiplus::RectF status_pill(214.0f, 18.0f, 110.0f, 29.0f);
    FillRounded(graphics, status_pill, 14.0f, ThemePanel());
    const Gdiplus::Color status_color = g.connected ? Gdiplus::Color(255, 76, 255, 159)
                                         : g.connecting ? Gdiplus::Color(255, 126, 145, 255)
                                                        : Gdiplus::Color(255, 191, 195, 232);
    Gdiplus::SolidBrush status_dot(status_color);
    graphics.FillEllipse(&status_dot, 224.0f, 29.0f, 7.0f, 7.0f);
    SelectObject(target, g.small_font);
    SetTextColor(target, Color(247, 246, 251));
    RECT status_rect{236, 18, 317, 47};
    DrawTextW(target, g.status.c_str(), -1, &status_rect,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    SetTextColor(target, Color(191, 195, 232));
    SelectObject(target, g.normal_font);
    RECT gear{330, 17, 354, 48};
    RECT minimize{363, 17, 387, 48};
    RECT close{396, 17, 420, 48};
    DrawTextW(target, L"⚙", -1, &gear, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    DrawTextW(target, L"—", -1, &minimize, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    DrawTextW(target, L"×", -1, &close, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    if (g.settings_open) {
        Gdiplus::SolidBrush dim(Gdiplus::Color(208, 5, 10, 24));
        graphics.FillRectangle(&dim, 0, 0, area.right, area.bottom);
        const Gdiplus::RectF card(36.0f, 75.0f, 368.0f, 610.0f);
        FillRounded(graphics, card, 20.0f, ThemePanel());
        StrokeRounded(graphics, card, 20.0f, ThemeBorder());
        SelectObject(target, g.heading_font);
        SetTextColor(target, Color(247, 246, 251));
        RECT heading{58, 94, 300, 124};
        DrawTextW(target, L"Settings", -1, &heading, DT_SINGLELINE | DT_VCENTER);
        SelectObject(target, g.normal_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT settings_close{364, 94, 384, 124};
        DrawTextW(target, L"×", -1, &settings_close, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        HBRUSH divider_brush = CreateSolidBrush(
            kSupporterEdition ? Color(61, 55, 95) : Color(52, 52, 109));
        RECT divider{58, 137, 382, 138};
        FillRect(target, &divider, divider_brush);
        DeleteObject(divider_brush);

        SelectObject(target, g.normal_font);
        SetTextColor(target, Color(247, 246, 251));
        RECT relay_label{58, 151, 250, 174};
        DrawTextW(target, L"Relay mode", -1, &relay_label, DT_SINGLELINE | DT_VCENTER);
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT relay_note{58, 172, 382, 190};
        DrawTextW(target, kSupporterEdition
                              ? L"Auto tries direct first. Force Relay is also available."
                              : L"Auto tries direct first, then a relay if needed.",
                  -1, &relay_note,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(target, g.normal_font);
        SetTextColor(target, Color(247, 246, 251));
        RECT coordinator_label{58, 246, 250, 269};
        DrawTextW(target, L"Coordinator", -1, &coordinator_label, DT_SINGLELINE | DT_VCENTER);
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT coordinator_note{58, 268, 382, 290};
        DrawTextW(target, L"Change this only for a self-hosted coordinator.", -1, &coordinator_note,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        FillRounded(graphics, Gdiplus::RectF(58.0f, 291.0f, 324.0f, 46.0f), 12.0f,
                    ThemePanel());
        StrokeRounded(graphics, Gdiplus::RectF(58.0f, 291.0f, 324.0f, 46.0f), 12.0f,
                      ThemeBorder());
        RECT divider_two{58, 355, 382, 356};
        divider_brush = CreateSolidBrush(
            kSupporterEdition ? Color(61, 55, 95) : Color(52, 52, 109));
        FillRect(target, &divider_two, divider_brush);
        DeleteObject(divider_brush);
        SelectObject(target, g.normal_font);
        SetTextColor(target, Color(247, 246, 251));
        RECT performance_label{58, 369, 382, 391};
        DrawTextW(target, L"Performance mode", -1, &performance_label, DT_SINGLELINE | DT_VCENTER);
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT performance_note{58, 393, 382, 426};
        DrawTextW(target, L"Turns off mesh depth and floating motion. Earth and stars remain animated.",
                  -1, &performance_note, DT_WORDBREAK | DT_LEFT);
        SelectObject(target, g.normal_font);
        SetTextColor(target, Color(247, 246, 251));
        RECT priority_label{58, 486, 382, 508};
        DrawTextW(target, L"Prioritize OrbitLan adapter", -1, &priority_label,
                  DT_SINGLELINE | DT_VCENTER);
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT priority_note{58, 510, 382, 541};
        DrawTextW(target, L"Helps older games discover LAN sessions over the virtual adapter.", -1,
                  &priority_note, DT_WORDBREAK | DT_LEFT);
        SetTextColor(target, Color(153, 157, 200));
        RECT version{58, 656, 382, 676};
        DrawTextW(target,
                  kSupporterEdition ? L"OrbitLan 1.0.0 · Supporter · larger hosted networks"
                                    : L"OrbitLan 1.0.0 · Community · hosts up to 4 nodes",
                  -1, &version,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    } else if (!g.connected) {
        SelectObject(target, g.heading_font);
        SetTextColor(target, Color(247, 246, 251));
        RECT heading{26, 362, area.right - 26, 391};
        DrawTextW(target, L"Join a network", -1, &heading, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT subheading{26, 392, area.right - 26, 417};
        DrawTextW(target, L"Everyone on the same code shares one LAN.", -1, &subheading,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        RECT name_label{28, 429, 220, 452};
        DrawTextW(target, L"Your name", -1, &name_label, DT_SINGLELINE | DT_VCENTER);
        FillRounded(graphics, Gdiplus::RectF(26.0f, 454.0f, 388.0f, 46.0f), 12.0f,
                    ThemePanel());
        StrokeRounded(graphics, Gdiplus::RectF(26.0f, 454.0f, 388.0f, 46.0f), 12.0f,
                      ThemeBorder());
        RECT code_label{28, 514, 220, 537};
        DrawTextW(target, L"Network code", -1, &code_label, DT_SINGLELINE | DT_VCENTER);
        FillRounded(graphics, Gdiplus::RectF(26.0f, 539.0f, 308.0f, 46.0f), 12.0f,
                    ThemePanel());
        StrokeRounded(graphics, Gdiplus::RectF(26.0f, 539.0f, 308.0f, 46.0f), 12.0f,
                      ThemeBorder());
    } else {
        const Gdiplus::RectF network_card(26.0f, 326.0f, 388.0f, 82.0f);
        DrawSolidCard(graphics, network_card, 18.0f);
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT code_label{42, 332, 220, 348};
        DrawTextW(target, L"NETWORK CODE", -1, &code_label, DT_SINGLELINE | DT_VCENTER);
        SelectObject(target, g.code_font);
        SetTextColor(target, Color(247, 246, 251));
        const std::wstring network_code = ReadWindowText(g.network_code);
        RECT code_value{42, 347, 398, 379};
        DrawTextW(target, network_code.c_str(), -1, &code_value,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT ip_label{42, 382, 94, 405};
        DrawTextW(target, L"Your IP", -1, &ip_label, DT_SINGLELINE | DT_VCENTER);
        SetTextColor(target, Color(247, 246, 251));
        const std::wstring virtual_ip = ReadWindowText(g.virtual_ip);
        RECT ip_value{95, 382, 225, 405};
        DrawTextW(target, virtual_ip.c_str(), -1, &ip_value,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        const std::wstring summary = ReadWindowText(g.summary);
        RECT summary_value{245, 382, 398, 405};
        DrawTextW(target, summary.c_str(), -1, &summary_value,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
        SetTextColor(target, Color(191, 195, 232));
        const Gdiplus::RectF nodes_card(26.0f, 418.0f, 388.0f, 222.0f);
        DrawSolidCard(graphics, nodes_card, 16.0f);
        RECT nodes{42, 421, 220, 443};
        DrawTextW(target, L"NODES", -1, &nodes, DT_SINGLELINE | DT_VCENTER);
        constexpr int visible_rows = 4;
        const int maximum_top = std::max(0, static_cast<int>(g.nodes.size()) - visible_rows);
        g.node_top_index = std::clamp(g.node_top_index, 0, maximum_top);
        for (int slot = 0; slot < visible_rows; ++slot) {
            const size_t node_index = static_cast<size_t>(g.node_top_index + slot);
            if (node_index >= g.nodes.size()) break;
            const RECT row{34, 450 + slot * 46, 406, 492 + slot * 46};
            DrawNodeRow(target, row, node_index);
        }
        if (maximum_top > 0) {
            constexpr int track_top = 452;
            constexpr int track_height = 176;
            const int thumb_height = std::max(30, track_height * visible_rows /
                                                   static_cast<int>(g.nodes.size()));
            const int thumb_top = track_top + (track_height - thumb_height) * g.node_top_index /
                                               maximum_top;
            FillRounded(graphics, Gdiplus::RectF(401.0f, static_cast<Gdiplus::REAL>(thumb_top),
                                                 5.0f, static_cast<Gdiplus::REAL>(thumb_height)),
                        2.5f, Gdiplus::Color(176, 218, 218, 228));
        }
        const Gdiplus::RectF disconnect_button(26.0f, 650.0f, 388.0f, 44.0f);
        FillRounded(graphics, disconnect_button, 12.0f,
                    kSupporterEdition ? Gdiplus::Color(255, 35, 32, 67)
                                      : Gdiplus::Color(255, 37, 37, 87));
        SelectObject(target, g.normal_font);
        SetTextColor(target, Color(247, 246, 251));
        RECT disconnect_text{26, 650, 414, 694};
        DrawTextW(target, L"Disconnect", -1, &disconnect_text,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    BitBlt(dc, 0, 0, area.right, area.bottom, target, 0, 0, SRCCOPY);
    SelectObject(buffer_dc, old_bitmap);
    DeleteObject(buffer_bitmap);
    DeleteDC(buffer_dc);
    EndPaint(window, &paint);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g.taskbar_created) {
        AddTrayIcon();
        return 0;
    }
    switch (message) {
        case WM_CREATE: {
            g.window = window;
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
            const DWORD corner_preference = 2;
            DwmSetWindowAttribute(window, 33, &corner_preference, sizeof(corner_preference));
            const DWORD border_color = 0xFFFFFFFE;
            DwmSetWindowAttribute(window, 34, &border_color, sizeof(border_color));
            SetWindowRgn(window, CreateRoundRectRgn(0, 0, kWindowWidth + 1, kWindowHeight + 1,
                                                    24, 24), TRUE);
            g.normal_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
            g.small_font = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Segoe UI");
            g.title_font = CreateFontW(-21, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Segoe UI Variable Display");
            g.heading_font = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         DEFAULT_PITCH, L"Segoe UI");
            g.code_font = CreateFontW(-23, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Segoe UI");
            g.button_font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Segoe UI");
            g.field_brush = CreateSolidBrush(ThemePanelColor());
            g.nodes_brush = CreateSolidBrush(Color(45, 45, 53));

            g.name_edit = MakeControl(L"EDIT", ReadSetting(L"Name", DefaultUserName()).c_str(),
                                      WS_TABSTOP | ES_AUTOHSCROLL, kNameEdit);
            g.code_edit = MakeControl(L"EDIT", RandomCode().c_str(), WS_TABSTOP | ES_AUTOHSCROLL, kCodeEdit);
            g.new_button = MakeControl(L"BUTTON", L"New", WS_TABSTOP | BS_OWNERDRAW, kNewCode);
            g.connect_button = MakeControl(L"BUTTON", L"Connect", WS_TABSTOP | BS_OWNERDRAW, kConnect);
            g.network_code = MakeControl(L"STATIC", L"—", SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
                                         kNetworkCode);
            SendMessageW(g.network_code, WM_SETFONT, reinterpret_cast<WPARAM>(g.code_font), TRUE);
            g.virtual_ip = MakeControl(L"STATIC", L"—", SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
                                      kVirtualIp);
            g.summary = MakeControl(L"STATIC", L"Waiting for nodes…", SS_RIGHT | SS_CENTERIMAGE, kSummary);
            SendMessageW(g.virtual_ip, WM_SETFONT, reinterpret_cast<WPARAM>(g.small_font), TRUE);
            SendMessageW(g.summary, WM_SETFONT, reinterpret_cast<WPARAM>(g.small_font), TRUE);
            g.nodes_list = MakeControl(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                                          LBS_NOINTEGRALHEIGHT,
                                       kNodes);
            SetWindowTheme(g.nodes_list, L"DarkMode_Explorer", nullptr);
            SetWindowSubclass(g.nodes_list, NodeListProc, 2, 0);
            g.disconnect_button = MakeControl(L"BUTTON", L"Disconnect", WS_TABSTOP | BS_OWNERDRAW, kDisconnect);
            g.relay_combo = MakeControl(L"BUTTON", L"Relay: Auto  ▾", WS_TABSTOP | BS_OWNERDRAW,
                                        kRelay);
            const std::wstring relay = ReadSetting(L"Relay", L"auto");
            g.relay_mode = relay == L"off" ? 1
                               : kSupporterEdition && relay == L"on" ? 2 : 0;
            SetWindowTextW(g.relay_combo, g.relay_mode == 0 ? L"Relay: Auto  ▾"
                                            : g.relay_mode == 1 ? L"Relay: Off  ▾"
                                                                : L"Relay: On  ▾");
            g.coordinator_edit = MakeControl(L"EDIT",
                ReadSetting(L"Coordinator", Utf8ToWide(kDefaultCoordinator)).c_str(),
                WS_TABSTOP | ES_AUTOHSCROLL, kCoordinator);
            g.priority = ReadSetting(L"Priority", L"0") == L"1";
            g.priority_button = MakeControl(L"BUTTON", g.priority ? L"Turn off" : L"Turn on",
                                            WS_TABSTOP | BS_OWNERDRAW,
                                            kPriority);
            g.settings_button = MakeControl(L"BUTTON", L"⚙", WS_TABSTOP | BS_OWNERDRAW, kSettings);
            g.settings_done = MakeControl(L"BUTTON", L"Save", WS_TABSTOP | BS_OWNERDRAW, kSettingsDone);
            g.performance_mode = ReadSetting(L"Performance", L"0") == L"1";
            g.performance_button = MakeControl(
                L"BUTTON", g.performance_mode ? L"Turn off" : L"Turn on",
                WS_TABSTOP | BS_OWNERDRAW, kPerformance);
            g.support_button = MakeControl(L"BUTTON",
                                           kSupporterEdition ? L"Supporter page" : L"Support",
                                           WS_TABSTOP | BS_OWNERDRAW, kSupport);
            for (HWND control : {g.new_button, g.connect_button, g.disconnect_button, g.relay_combo,
                                 g.priority_button, g.settings_done, g.performance_button,
                                 g.support_button, g.network_code, g.virtual_ip}) {
                UseHandCursor(control);
            }

            AddTrayIcon();
            LoadVisuals();
            Layout();
            SetUiState(false, false);
            SetTimer(window, kStatusTimer, 1000, nullptr);
            PostMessageW(window, WM_APP, 0, 0);
            return 0;
        }
        case WM_APP:
            if (!kUiPreview) {
                OfferServiceInstall();
                PollStatus();
            }
            return 0;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                ReleaseVisuals();
                ShowWindow(window, SW_HIDE);
                TrimWorkingSet();
            } else {
                Layout();
            }
            return 0;
        case WM_TIMER:
            if (wparam == kStatusTimer) {
                if (g.copy_notice_until != 0 && GetTickCount64() >= g.copy_notice_until) {
                    g.copy_notice_until = 0;
                    SetWindowTextW(g.summary, g.network_summary.c_str());
                }
                if (!kUiPreview) PollStatus();
            }
            else if (wparam == kAnimationTimer) AdvanceVisuals();
            return 0;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window, &point);
            if (point.y >= 8 && point.y < 55) {
                if ((point.x >= 326 && point.x < 425) || g.settings_open) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_SETCURSOR: {
            if (reinterpret_cast<HWND>(wparam) != window) break;
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            const bool header_action = InRect(point.x, point.y, 326, 8, 425, 55);
            const bool overlay_close = g.settings_open && InRect(point.x, point.y, 356, 88, 392, 132);
            const bool connected_copy = !g.settings_open && g.connected &&
                (InRect(point.x, point.y, 42, 347, 398, 379) ||
                 InRect(point.x, point.y, 42, 382, 225, 405));
            const int node_slot_y = point.y - 450;
            const bool connected_node = !g.settings_open && g.connected && node_slot_y >= 0 &&
                node_slot_y < 184 && node_slot_y % 46 < 42 && point.x >= 34 && point.x < 394;
            const bool connected_disconnect = !g.settings_open && g.connected &&
                InRect(point.x, point.y, 26, 650, 414, 694);
            if (header_action || overlay_close || connected_copy || connected_node ||
                connected_disconnect) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            const bool setup_field = !g.settings_open && !g.connected &&
                (InRect(point.x, point.y, 26, 454, 414, 500) ||
                 InRect(point.x, point.y, 26, 539, 334, 585));
            const bool settings_field = g.settings_open && InRect(point.x, point.y, 58, 291, 382, 337);
            if (setup_field || settings_field) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            if (!g.settings_open && g.connected && x >= 398 && x < 411 && y >= 449 && y < 632) {
                const int maximum_top = std::max(0, static_cast<int>(g.nodes.size()) - 4);
                if (maximum_top > 0) {
                    constexpr int track_top = 452;
                    constexpr int track_height = 176;
                    const int thumb_height = std::max(30, track_height * 4 /
                                                           static_cast<int>(g.nodes.size()));
                    const int thumb_top = track_top + (track_height - thumb_height) *
                                                       g.node_top_index / maximum_top;
                    if (y >= thumb_top && y < thumb_top + thumb_height) {
                        g.node_scroll_dragging = true;
                        g.node_scroll_drag_offset = y - thumb_top;
                        SetCapture(window);
                    } else {
                        g.node_top_index = std::clamp(g.node_top_index +
                            (y < thumb_top ? -4 : 4), 0, maximum_top);
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
                return 0;
            }
            if (!g.settings_open && !g.connected && InRect(x, y, 26, 454, 414, 500)) {
                SetFocus(g.name_edit);
                return 0;
            }
            if (!g.settings_open && !g.connected && InRect(x, y, 26, 539, 334, 585)) {
                SetFocus(g.code_edit);
                return 0;
            }
            if (g.settings_open && InRect(x, y, 58, 291, 382, 337)) {
                SetFocus(g.coordinator_edit);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (g.node_scroll_dragging && GetCapture() == window) {
                constexpr int track_top = 452;
                constexpr int track_height = 176;
                const int maximum_top = std::max(0, static_cast<int>(g.nodes.size()) - 4);
                if (maximum_top > 0) {
                    const int thumb_height = std::max(30, track_height * 4 /
                                                           static_cast<int>(g.nodes.size()));
                    const int travel = std::max(1, track_height - thumb_height);
                    const int thumb = std::clamp(GET_Y_LPARAM(lparam) -
                        g.node_scroll_drag_offset - track_top, 0, travel);
                    g.node_top_index = (thumb * maximum_top + travel / 2) / travel;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            break;
        case WM_LBUTTONUP: {
            if (g.node_scroll_dragging) {
                g.node_scroll_dragging = false;
                if (GetCapture() == window) ReleaseCapture();
                return 0;
            }
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            if (g.settings_open && x >= 356 && x <= 392 && y >= 88 && y <= 132) {
                ToggleSettings(false);
            } else if (!g.settings_open && g.connected &&
                       InRect(x, y, 26, 650, 414, 694)) {
                StopConnection();
            } else if (!g.settings_open && g.connected && y >= 450 && y < 634 &&
                       (y - 450) % 46 < 42 && x >= 34 && x < 394) {
                const size_t index = static_cast<size_t>(g.node_top_index + (y - 450) / 46);
                if (index < g.nodes.size()) {
                    if (g.is_host && x >= 330 && x < 380) {
                        RequestKickNode(index);
                    } else if (CopyToClipboard(g.nodes[index].ip)) {
                        ShowCopyNotice(L"IP copied");
                    }
                }
            } else if (!g.settings_open && g.connected && InRect(x, y, 42, 347, 398, 379)) {
                if (CopyToClipboard(ReadWindowText(g.network_code))) ShowCopyNotice(L"Code copied");
            } else if (!g.settings_open && g.connected && InRect(x, y, 42, 382, 225, 405)) {
                if (CopyToClipboard(ReadWindowText(g.virtual_ip))) ShowCopyNotice(L"IP copied");
            } else if (y >= 8 && y <= 55 && x >= 326 && x < 359) {
                ToggleSettings(!g.settings_open);
            } else if (y >= 8 && y <= 55 && x >= 359 && x < 392) {
                SendMessageW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            } else if (y >= 8 && y <= 55 && x >= 392 && x < 425) {
                SendMessageW(window, WM_CLOSE, 0, 0);
            } else if (g.settings_open && (x < 36 || x > 404 || y < 75 || y > 685)) {
                ToggleSettings(false);
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (!g.settings_open && g.connected) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(window, &point);
                if (InRect(point.x, point.y, 26, 416, 414, 640)) {
                    const int maximum_top = std::max(0, static_cast<int>(g.nodes.size()) - 4);
                    const int direction = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
                    g.node_top_index = std::clamp(g.node_top_index + direction, 0, maximum_top);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
            }
            break;
        case WM_CAPTURECHANGED:
            g.node_scroll_dragging = false;
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kNetworkCode:
                    if (g.connected && CopyToClipboard(ReadWindowText(g.network_code))) {
                        ShowCopyNotice(L"Code copied");
                    }
                    return 0;
                case kVirtualIp:
                    if (g.connected && CopyToClipboard(ReadWindowText(g.virtual_ip))) {
                        ShowCopyNotice(L"IP copied");
                    }
                    return 0;
                case kNewCode:
                    SetWindowTextW(g.code_edit, RandomCode().c_str());
                    return 0;
                case kConnect:
                    StartConnection();
                    return 0;
                case kDisconnect:
                    StopConnection();
                    return 0;
                case kRelay:
                    ChooseRelayMode();
                    return 0;
                case kPriority:
                    g.priority = !g.priority;
                    SetWindowTextW(g.priority_button, g.priority ? L"Turn off" : L"Turn on");
                    SaveConfiguration();
                    InvalidateRect(g.priority_button, nullptr, TRUE);
                    if (g.connected) CallService(g.priority ? "PRIORITY\t1" : "PRIORITY\t0", 3000);
                    return 0;
                case kPerformance:
                    g.performance_mode = !g.performance_mode;
                    SetWindowTextW(g.performance_button,
                                   g.performance_mode ? L"Turn off" : L"Turn on");
                    SaveConfiguration();
                    InvalidateRect(g.performance_button, nullptr, TRUE);
                    InvalidateRect(g.window, nullptr, FALSE);
                    return 0;
                case kSupport:
                    ShellExecuteW(window, L"open", L"https://orbitlan.site/#support",
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kSettings:
                    ToggleSettings(!g.settings_open);
                    return 0;
                case kSettingsDone:
                    ToggleSettings(false);
                    return 0;
            }
            break;
        case kConnectComplete: {
            std::unique_ptr<PipeReply> result(reinterpret_cast<PipeReply*>(lparam));
            g.connecting = false;
            if (!result->command_ok) {
                SetStatus(L"Offline");
                SetUiState(false, false);
                MessageBoxW(window, Utf8ToWide(result->error).c_str(), L"Could not connect",
                            MB_OK | MB_ICONERROR);
            } else {
                if (g.priority) CallService("PRIORITY\t1", 3000);
                PollStatus();
            }
            return 0;
        }
        case kDisconnectComplete: {
            std::unique_ptr<PipeReply> result(reinterpret_cast<PipeReply*>(lparam));
            g.connecting = false;
            SetStatus(L"Offline");
            SetUiState(false, false);
            if (!result->command_ok && !result->error.empty()) {
                MessageBoxW(window, Utf8ToWide(result->error).c_str(), L"Disconnect warning",
                            MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            measure->itemHeight = measure->CtlID == kNodes ? 46 : 32;
            return TRUE;
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draw->CtlType == ODT_BUTTON) DrawButton(draw);
            else if (draw->CtlID == kNodes) DrawNode(draw);
            return TRUE;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, Color(247, 246, 251));
            SetBkColor(dc, ThemePanelColor());
            return reinterpret_cast<LRESULT>(g.field_brush);
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, Color(247, 246, 251));
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, Color(45, 45, 53));
            return reinterpret_cast<LRESULT>(g.nodes_brush);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, Color(247, 246, 251));
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, ThemePanelColor());
            return reinterpret_cast<LRESULT>(g.field_brush);
        }
        case WM_PAINT:
            PaintWindow(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            ReleaseVisuals();
            ShowWindow(window, SW_HIDE);
            TrimWorkingSet();
            return 0;
        case kTrayMessage:
            if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
                ShowMainWindow();
            } else if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) {
                POINT point{};
                GetCursorPos(&point);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Open OrbitLan");
                AppendMenuW(menu, MF_STRING | (g.connected ? 0 : MF_GRAYED), 2, L"Disconnect");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 3, L"Exit");
                SetForegroundWindow(window);
                const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                   point.x, point.y, 0, window, nullptr);
                DestroyMenu(menu);
                if (choice == 1) ShowMainWindow();
                else if (choice == 2) StopConnection();
                else if (choice == 3) {
                    if (g.connected) CallService("DISCONNECT", 8000);
                    DestroyWindow(window);
                }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, kStatusTimer);
            ReleaseVisuals();
            g.tray.uFlags = NIF_GUID;
            Shell_NotifyIconW(NIM_DELETE, &g.tray);
            DeleteObject(g.normal_font);
            DeleteObject(g.small_font);
            DeleteObject(g.title_font);
            DeleteObject(g.heading_font);
            DeleteObject(g.code_font);
            DeleteObject(g.button_font);
            DeleteObject(g.field_brush);
            DeleteObject(g.nodes_brush);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace
}  // namespace orbitlan

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    using namespace orbitlan;
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\OrbitLan.Native.UI.v1");
    if (singleton != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kUiWindowClass, nullptr); existing != nullptr) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleton);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    Gdiplus::GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    g.instance = instance;
    g.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_ORBITLAN), IMAGE_ICON,
                                           0, 0, LR_DEFAULTSIZE));
    g.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hIcon = g.icon;
    window_class.hIconSm = g.icon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kUiWindowClass;
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&window_class);

    const int width = kWindowWidth;
    const int height = kWindowHeight;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kUiWindowClass, L"OrbitLan",
                                  WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                  x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (singleton != nullptr) CloseHandle(singleton);
    Gdiplus::GdiplusShutdown(g_gdiplus_token);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

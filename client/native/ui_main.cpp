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

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace orbitlan {
namespace {

constexpr UINT kTrayMessage = WM_APP + 10;
constexpr UINT kConnectComplete = WM_APP + 11;
constexpr UINT kDisconnectComplete = WM_APP + 12;
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kAnimationTimer = 2;
constexpr UINT kTaskbarCreatedFallback = WM_APP + 13;

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
};

struct Node {
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
    HBRUSH field_brush = nullptr;
    std::unique_ptr<Gdiplus::Image> earth;
    GUID earth_dimension{};
    std::vector<unsigned> earth_delays;
    unsigned earth_frame = 0;
    ULONGLONG next_earth_frame = 0;
    ULONGLONG animation_started = 0;
    NOTIFYICONDATAW tray{};
    UINT taskbar_created = kTaskbarCreatedFallback;
    bool connected = false;
    bool connecting = false;
    bool priority = false;
    int relay_mode = 0;
    bool settings_open = false;
    bool install_offered = false;
    std::wstring status = L"Offline";
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
};

AppState g;
ULONG_PTR g_gdiplus_token = 0;

COLORREF Color(unsigned red, unsigned green, unsigned blue) {
    return RGB(red, green, blue);
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
    std::array<unsigned char, 4> bytes{};
    BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    wchar_t code[32]{};
    swprintf_s(code, L"orbit-%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3]);
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

void SetUiState(bool connected, bool connecting) {
    g.connected = connected;
    g.connecting = connecting;
    const bool main_screen = !g.settings_open;
    const bool show_setup = main_screen && !connected;
    const bool show_connected = main_screen && connected;
    ShowWindow(g.name_edit, show_setup ? SW_SHOW : SW_HIDE);
    ShowWindow(g.code_edit, show_setup ? SW_SHOW : SW_HIDE);
    ShowWindow(g.new_button, show_setup ? SW_SHOW : SW_HIDE);
    ShowWindow(g.connect_button, show_setup ? SW_SHOW : SW_HIDE);
    ShowWindow(g.network_code, show_connected ? SW_SHOW : SW_HIDE);
    ShowWindow(g.virtual_ip, show_connected ? SW_SHOW : SW_HIDE);
    ShowWindow(g.summary, show_connected ? SW_SHOW : SW_HIDE);
    ShowWindow(g.nodes_list, show_connected ? SW_SHOW : SW_HIDE);
    ShowWindow(g.disconnect_button, show_connected ? SW_SHOW : SW_HIDE);
    ShowWindow(g.relay_combo, g.settings_open ? SW_SHOW : SW_HIDE);
    ShowWindow(g.priority_button, g.settings_open ? SW_SHOW : SW_HIDE);
    ShowWindow(g.coordinator_edit, g.settings_open ? SW_SHOW : SW_HIDE);
    ShowWindow(g.settings_done, g.settings_open ? SW_SHOW : SW_HIDE);
    EnableWindow(g.connect_button, !connecting);
    SetWindowTextW(g.connect_button, connecting ? L"Connecting…" : L"Connect");
    InvalidateRect(g.window, nullptr, TRUE);
}

void ReleaseVisuals() {
    KillTimer(g.window, kAnimationTimer);
    g.earth.reset();
    g.earth_delays.clear();
    g.earth_frame = 0;
}

void TrimWorkingSet() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void LoadVisuals() {
    if (g.earth != nullptr) return;
    const fs::path image_path = ExecutableDirectory() / L"assets" / L"earth-clouds.gif";
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
    const int content = width - 56;
    Position(g.settings_button, width - 184, 28, 36, 34);
    if (g.settings_open) {
        Position(g.relay_combo, 28, 168, 176, 40);
        Position(g.priority_button, 220, 168, content - 192, 40);
        Position(g.coordinator_edit, 28, 254, content, 42);
        Position(g.settings_done, 28, 324, content, 46);
    } else if (!g.connected) {
        Position(g.name_edit, 28, 150, content, 42);
        Position(g.code_edit, 28, 228, content - 92, 42);
        Position(g.new_button, width - 108, 228, 80, 42);
        Position(g.connect_button, 28, 292, content, 50);
    } else {
        Position(g.network_code, 28, 142, content, 38);
        Position(g.virtual_ip, 28, 188, content, 28);
        Position(g.summary, 28, 220, content, 24);
        Position(g.nodes_list, 28, 268, content, 192);
        Position(g.disconnect_button, 28, 476, content, 44);
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

void ApplyStatusJson(const std::string& json) {
    const std::wstring code = Utf8ToWide(JsonString(json, "code"));
    const std::wstring ip = Utf8ToWide(JsonString(json, "myIP"));
    const std::wstring summary = Utf8ToWide(JsonString(json, "summary"));
    SetWindowTextW(g.network_code, (L"Network  " + code).c_str());
    SetWindowTextW(g.virtual_ip, (L"Virtual IP  " + ip).c_str());
    SetWindowTextW(g.summary, summary.c_str());

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
        SendMessageW(g.nodes_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
    }
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
    WriteSetting(L"Relay", g.relay_mode == 1 ? L"off" : g.relay_mode == 2 ? L"on" : L"auto");
    WriteSetting(L"Priority", g.priority ? L"1" : L"0");
}

std::string SelectedRelay() {
    return g.relay_mode == 1 ? "off" : g.relay_mode == 2 ? "on" : "auto";
}

void ChooseRelayMode() {
    RECT anchor{};
    GetWindowRect(g.relay_combo, &anchor);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g.relay_mode == 0 ? MF_CHECKED : 0), 20, L"Auto — direct first");
    AppendMenuW(menu, MF_STRING | (g.relay_mode == 1 ? MF_CHECKED : 0), 21, L"Off — direct only");
    AppendMenuW(menu, MF_STRING | (g.relay_mode == 2 ? MF_CHECKED : 0), 22, L"On — force relay");
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, anchor.left, anchor.bottom,
                                       0, g.window, nullptr);
    DestroyMenu(menu);
    if (choice < 20 || choice > 22) return;
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
                                Base64Encode(WideToUtf8(server)) + "\t" + SelectedRelay();
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
    const fs::path setup = ExecutableDirectory() / L"OrbitLanSetup.exe";
    if (!FileExists(setup)) {
        MessageBoxW(g.window,
                    L"The OrbitLan network service is not installed. Run OrbitLanSetup.exe from the release package.",
                    L"OrbitLan service required", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(g.window,
                    L"OrbitLan needs its network service and TAP driver. Install them now?\n\n"
                    L"Windows will ask for administrator approval once. Normal OrbitLan launches will not ask again.",
                    L"Finish OrbitLan setup", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        ShellExecuteW(g.window, L"runas", setup.c_str(), L"--install", ExecutableDirectory().c_str(), SW_SHOWNORMAL);
    }
}

void DrawButton(const DRAWITEMSTRUCT* draw) {
    const bool hot = (draw->itemState & ODS_HOTLIGHT) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    COLORREF background = Color(37, 37, 87);
    if (draw->CtlID == kConnect) background = pressed ? Color(72, 91, 178) : Color(78, 127, 234);
    else if (pressed || hot) background = Color(52, 52, 109);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(draw->hDC, &draw->rcItem, brush);
    DeleteObject(brush);

    RECT text_rect = draw->rcItem;
    std::wstring text = ReadWindowText(draw->hwndItem);
    if (draw->CtlID == kPriority) {
        RECT box{text_rect.left + 12, text_rect.top + 11, text_rect.left + 28, text_rect.top + 27};
        HBRUSH box_brush = CreateSolidBrush(g.priority ? Color(76, 255, 159) : Color(24, 26, 75));
        FillRect(draw->hDC, &box, box_brush);
        DeleteObject(box_brush);
        FrameRect(draw->hDC, &box, GetSysColorBrush(COLOR_GRAYTEXT));
        text_rect.left += 38;
    }
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, disabled ? Color(130, 133, 169) : Color(247, 246, 251));
    SelectObject(draw->hDC, g.normal_font);
    DrawTextW(draw->hDC, text.c_str(), -1, &text_rect,
              DT_SINGLELINE | DT_VCENTER | (draw->CtlID == kPriority ? DT_LEFT : DT_CENTER));
}

void DrawNode(const DRAWITEMSTRUCT* draw) {
    if (draw->itemID >= g.nodes.size()) return;
    HBRUSH brush = CreateSolidBrush((draw->itemID % 2) == 0 ? Color(24, 26, 75) : Color(20, 23, 69));
    FillRect(draw->hDC, &draw->rcItem, brush);
    DeleteObject(brush);
    const Node& node = g.nodes[draw->itemID];
    RECT left = draw->rcItem;
    left.left += 12;
    left.right -= 120;
    RECT right = draw->rcItem;
    right.left = right.right - 112;
    right.right -= 12;
    SetBkMode(draw->hDC, TRANSPARENT);
    SelectObject(draw->hDC, g.normal_font);
    SetTextColor(draw->hDC, Color(247, 246, 251));
    const std::wstring primary = node.name + L"   " + node.ip;
    DrawTextW(draw->hDC, primary.c_str(), -1, &left, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SetTextColor(draw->hDC, node.state == L"direct" ? Color(76, 255, 159) : Color(245, 196, 74));
    const std::wstring state = node.state + L"  " + node.rtt;
    DrawTextW(draw->hDC, state.c_str(), -1, &right, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
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
    GRADIENT_RECT gradient{0, 1};
    GradientFill(target, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);

    uint32_t seed = 0x4f524249;
    const double star_shift = static_cast<double>((GetTickCount64() - g.animation_started) % 18000) / 18000.0;
    HBRUSH star_dim = CreateSolidBrush(Color(133, 140, 202));
    HBRUSH star_bright = CreateSolidBrush(Color(201, 199, 255));
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

    if (g.earth != nullptr) {
        Gdiplus::Graphics graphics(target);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        const int earth_size = 286;
        const int earth_left = (area.right - earth_size) / 2;
        const int earth_top = area.bottom - 282;
        if (g.connecting) {
            const double phase = static_cast<double>((GetTickCount64() - g.animation_started) % 1400) / 1400.0;
            const int radius = 94 + static_cast<int>(phase * 95);
            const BYTE alpha = static_cast<BYTE>((1.0 - phase) * 175);
            Gdiplus::Pen pulse(Gdiplus::Color(alpha, 126, 145, 255), 4.0f);
            graphics.DrawEllipse(&pulse, earth_left + earth_size / 2 - radius,
                                 earth_top + earth_size / 2 - radius, radius * 2, radius * 2);
        }
        graphics.DrawImage(g.earth.get(), earth_left, earth_top, earth_size, earth_size);
        if (g.connected && !g.nodes.empty()) {
            const double time = static_cast<double>(GetTickCount64() - g.animation_started) / 2400.0;
            std::vector<Gdiplus::PointF> points;
            const size_t count = std::clamp<size_t>(g.nodes.size() + 2, 3, 8);
            points.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                const double angle = time + 6.283185307 * static_cast<double>(i) / count;
                points.emplace_back(
                    static_cast<Gdiplus::REAL>(earth_left + earth_size / 2 + std::cos(angle) * 132.0),
                    static_cast<Gdiplus::REAL>(earth_top + earth_size / 2 + std::sin(angle) * 68.0));
            }
            Gdiplus::Pen link(Gdiplus::Color(115, 69, 242, 154), 1.5f);
            Gdiplus::SolidBrush node(Gdiplus::Color(235, 76, 255, 159));
            for (size_t i = 0; i < points.size(); ++i) {
                graphics.DrawLine(&link, points[i], points[(i + 1) % points.size()]);
                graphics.FillEllipse(&node, points[i].X - 4.0f, points[i].Y - 4.0f, 8.0f, 8.0f);
            }
        }
    }

    DrawIconEx(target, 26, 20, g.icon, 54, 54, 0, nullptr, DI_NORMAL);
    SetBkMode(target, TRANSPARENT);
    SetTextColor(target, Color(247, 246, 251));
    SelectObject(target, g.title_font);
    RECT title{84, 20, 300, 58};
    DrawTextW(target, L"OrbitLan", -1, &title, DT_SINGLELINE | DT_VCENTER);
    SelectObject(target, g.small_font);
    SetTextColor(target, Color(191, 195, 232));
    RECT subtitle{86, 52, 330, 74};
    DrawTextW(target, L"PRIVATE VIRTUAL LAN", -1, &subtitle, DT_SINGLELINE | DT_VCENTER);

    const COLORREF status_color = g.connected ? Color(76, 255, 159)
                                  : g.connecting ? Color(126, 145, 255)
                                                 : Color(191, 195, 232);
    HBRUSH dot = CreateSolidBrush(status_color);
    RECT dot_rect{area.right - 142, 40, area.right - 133, 49};
    FillRect(target, &dot_rect, dot);
    DeleteObject(dot);
    SetTextColor(target, Color(247, 246, 251));
    RECT status_rect{area.right - 126, 28, area.right - 22, 61};
    DrawTextW(target, g.status.c_str(), -1, &status_rect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);

    SelectObject(target, g.normal_font);
    SetTextColor(target, Color(201, 199, 255));
    if (g.settings_open) {
        RECT heading{28, 96, area.right - 28, 134};
        DrawTextW(target, L"Settings", -1, &heading, DT_SINGLELINE | DT_VCENTER);
        RECT relay_label{30, 140, 220, 166};
        DrawTextW(target, L"RELAY MODE", -1, &relay_label, DT_SINGLELINE | DT_VCENTER);
        RECT coordinator_label{30, 226, area.right - 28, 252};
        DrawTextW(target, L"COORDINATOR", -1, &coordinator_label, DT_SINGLELINE | DT_VCENTER);
        SelectObject(target, g.small_font);
        SetTextColor(target, Color(191, 195, 232));
        RECT note{28, 382, area.right - 28, 426};
        DrawTextW(target, L"Network changes are saved locally and applied on the next connection.",
                  -1, &note, DT_WORDBREAK | DT_LEFT);
    } else if (!g.connected) {
        RECT heading{28, 96, area.right - 28, 128};
        DrawTextW(target, L"Create or join a LAN", -1, &heading, DT_SINGLELINE | DT_VCENTER);
        RECT name_label{30, 126, 220, 148};
        DrawTextW(target, L"DISPLAY NAME", -1, &name_label, DT_SINGLELINE | DT_VCENTER);
        RECT code_label{30, 204, 220, 226};
        DrawTextW(target, L"NETWORK CODE", -1, &code_label, DT_SINGLELINE | DT_VCENTER);
    } else {
        RECT heading{28, 96, area.right - 28, 130};
        DrawTextW(target, L"Your LAN is live", -1, &heading, DT_SINGLELINE | DT_VCENTER);
        RECT nodes{30, 244, 220, 268};
        DrawTextW(target, L"NODES", -1, &nodes, DT_SINGLELINE | DT_VCENTER);
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
            g.normal_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
            g.small_font = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Segoe UI");
            g.title_font = CreateFontW(-28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Segoe UI");
            g.field_brush = CreateSolidBrush(Color(24, 26, 75));

            g.name_edit = MakeControl(L"EDIT", ReadSetting(L"Name", DefaultUserName()).c_str(),
                                      WS_TABSTOP | ES_AUTOHSCROLL, kNameEdit);
            g.code_edit = MakeControl(L"EDIT", RandomCode().c_str(), WS_TABSTOP | ES_AUTOHSCROLL, kCodeEdit);
            g.new_button = MakeControl(L"BUTTON", L"New", WS_TABSTOP | BS_OWNERDRAW, kNewCode);
            g.connect_button = MakeControl(L"BUTTON", L"Connect", WS_TABSTOP | BS_OWNERDRAW, kConnect);
            g.network_code = MakeControl(L"STATIC", L"Network  —", SS_LEFT | SS_CENTERIMAGE, kNetworkCode);
            g.virtual_ip = MakeControl(L"STATIC", L"Virtual IP  —", SS_LEFT | SS_CENTERIMAGE, kVirtualIp);
            g.summary = MakeControl(L"STATIC", L"Waiting for nodes…", SS_LEFT | SS_CENTERIMAGE, kSummary);
            g.nodes_list = MakeControl(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT |
                                                          WS_VSCROLL,
                                       kNodes);
            g.disconnect_button = MakeControl(L"BUTTON", L"Disconnect", WS_TABSTOP | BS_OWNERDRAW, kDisconnect);
            g.relay_combo = MakeControl(L"BUTTON", L"Relay: Auto  ▾", WS_TABSTOP | BS_OWNERDRAW,
                                        kRelay);
            const std::wstring relay = ReadSetting(L"Relay", L"auto");
            g.relay_mode = relay == L"off" ? 1 : relay == L"on" ? 2 : 0;
            SetWindowTextW(g.relay_combo, g.relay_mode == 0 ? L"Relay: Auto  ▾"
                                            : g.relay_mode == 1 ? L"Relay: Off  ▾"
                                                                : L"Relay: On  ▾");
            g.coordinator_edit = MakeControl(L"EDIT",
                ReadSetting(L"Coordinator", Utf8ToWide(kDefaultCoordinator)).c_str(),
                WS_TABSTOP | ES_AUTOHSCROLL, kCoordinator);
            g.priority = ReadSetting(L"Priority", L"0") == L"1";
            g.priority_button = MakeControl(L"BUTTON", L"Prioritize adapter", WS_TABSTOP | BS_OWNERDRAW,
                                            kPriority);
            g.settings_button = MakeControl(L"BUTTON", L"⚙", WS_TABSTOP | BS_OWNERDRAW, kSettings);
            g.settings_done = MakeControl(L"BUTTON", L"Done", WS_TABSTOP | BS_OWNERDRAW, kSettingsDone);

            AddTrayIcon();
            LoadVisuals();
            Layout();
            SetUiState(false, false);
            SetTimer(window, kStatusTimer, 1000, nullptr);
            PostMessageW(window, WM_APP, 0, 0);
            return 0;
        }
        case WM_APP:
            OfferServiceInstall();
            PollStatus();
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
            if (wparam == kStatusTimer) PollStatus();
            else if (wparam == kAnimationTimer) AdvanceVisuals();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
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
                    SaveConfiguration();
                    InvalidateRect(g.priority_button, nullptr, TRUE);
                    if (g.connected) CallService(g.priority ? "PRIORITY\t1" : "PRIORITY\t0", 3000);
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
            measure->itemHeight = measure->CtlID == kNodes ? 34 : 32;
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
            SetBkColor(dc, Color(24, 26, 75));
            return reinterpret_cast<LRESULT>(g.field_brush);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, Color(247, 246, 251));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        case WM_PAINT:
            PaintWindow(window);
            return 0;
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
            DeleteObject(g.field_brush);
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

    RECT desired{0, 0, 480, 720};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, 0);
    const int width = desired.right - desired.left;
    const int height = desired.bottom - desired.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(0, kUiWindowClass, L"OrbitLan", WS_OVERLAPPED | WS_CAPTION |
                                  WS_SYSMENU | WS_MINIMIZEBOX, x, y, width, height, nullptr,
                                  nullptr, instance, nullptr);
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

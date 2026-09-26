#include "common.h"
#include "protocol.h"

#include <bcrypt.h>
#include <sddl.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace orbitlan {
namespace {

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop_event = nullptr;
std::mutex g_engine_mutex;
HANDLE g_engine_process = nullptr;
HANDLE g_engine_job = nullptr;
std::string g_api_token;

struct LocalFreeDeleter {
    void operator()(void* value) const { if (value != nullptr) LocalFree(value); }
};
using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

struct WinHttpCloser {
    void operator()(void* value) const { if (value != nullptr) WinHttpCloseHandle(value); }
};
using HttpHandle = std::unique_ptr<void, WinHttpCloser>;

bool ProcessRunningLocked() {
    if (g_engine_process == nullptr) return false;
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(g_engine_process, &exit_code) || exit_code != STILL_ACTIVE) {
        CloseHandle(g_engine_process);
        g_engine_process = nullptr;
        if (g_engine_job != nullptr) {
            CloseHandle(g_engine_job);
            g_engine_job = nullptr;
        }
        g_api_token.clear();
        return false;
    }
    return true;
}

std::string RandomToken() {
    std::array<unsigned char, 32> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 15]);
    }
    return result;
}

bool HttpControl(std::wstring_view method, std::wstring_view path, std::string& body) {
    body.clear();
    HttpHandle session(WinHttpOpen(L"OrbitLanService/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;
    WinHttpSetTimeouts(session.get(), 1200, 1200, 1200, 1800);
    HttpHandle connection(WinHttpConnect(session.get(), L"127.0.0.1", kEngineApiPort, 0));
    if (!connection) return false;
    HttpHandle request(WinHttpOpenRequest(connection.get(), method.data(), path.data(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request) return false;
    const std::wstring header = L"X-OrbitLan-Token: " + Utf8ToWide(g_api_token) + L"\r\n";
    if (!WinHttpAddRequestHeaders(request.get(), header.c_str(), static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD)) {
        return false;
    }
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return false;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) return false;
        if (available == 0) break;
        if (body.size() + available > 256 * 1024) return false;
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), body.data() + offset, available, &read)) return false;
        body.resize(offset + read);
    }
    return status >= 200 && status < 300;
}

struct AdapterRecord {
    std::wstring guid;
    std::wstring connection_name;
};

std::wstring RegistryString(const std::wstring& subkey, const wchar_t* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buffer[512]{};
    DWORD type = 0;
    DWORD bytes = sizeof(buffer);
    const LONG result = RegQueryValueExW(key, value, nullptr, &type,
                                         reinterpret_cast<BYTE*>(buffer), &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ ? buffer : L"";
}

AdapterRecord FindOrbitLanAdapter() {
    constexpr wchar_t class_key[] =
        LR"(SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318})";
    const std::wstring owned_guid = RegistryString(LR"(SOFTWARE\OrbitLan)", L"AdapterGuid");
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_key, 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        return {};
    }
    AdapterRecord found;
    for (DWORD index = 0; found.guid.empty(); ++index) {
        wchar_t name[256]{};
        DWORD name_size = static_cast<DWORD>(std::size(name));
        const LONG enumerated = RegEnumKeyExW(root, index, name, &name_size, nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;
        HKEY child = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &child) != ERROR_SUCCESS) continue;
        wchar_t component[128]{};
        DWORD type = 0;
        DWORD bytes = sizeof(component);
        wchar_t guid[128]{};
        DWORD guid_type = 0;
        DWORD guid_bytes = sizeof(guid);
        const bool is_tap =
            RegQueryValueExW(child, L"ComponentId", nullptr, &type,
                             reinterpret_cast<BYTE*>(component), &bytes) == ERROR_SUCCESS &&
            type == REG_SZ && _wcsicmp(component, L"tap0901") == 0;
        const bool has_guid =
            RegQueryValueExW(child, L"NetCfgInstanceId", nullptr, &guid_type,
                             reinterpret_cast<BYTE*>(guid), &guid_bytes) == ERROR_SUCCESS &&
            guid_type == REG_SZ && guid[0] != 0;
        if (is_tap && has_guid) {
            const std::wstring connection =
                LR"(SYSTEM\CurrentControlSet\Control\Network\{4D36E972-E325-11CE-BFC1-08002BE10318}\)" +
                std::wstring(guid) + LR"(\Connection)";
            HKEY connection_key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, connection.c_str(), 0,
                              KEY_READ | KEY_WOW64_64KEY, &connection_key) == ERROR_SUCCESS) {
                wchar_t connection_name[128]{};
                DWORD name_type = 0;
                DWORD name_bytes = sizeof(connection_name);
                const bool has_name =
                    RegQueryValueExW(connection_key, L"Name", nullptr, &name_type,
                                     reinterpret_cast<BYTE*>(connection_name), &name_bytes) == ERROR_SUCCESS &&
                    name_type == REG_SZ;
                const bool owned = !owned_guid.empty() && _wcsicmp(guid, owned_guid.c_str()) == 0;
                const bool legacy_named = owned_guid.empty() && has_name &&
                                          _wcsicmp(connection_name, L"OrbitLan") == 0;
                if (owned || legacy_named) found = {guid, has_name ? connection_name : L""};
                RegCloseKey(connection_key);
            }
        }
        RegCloseKey(child);
    }
    RegCloseKey(root);
    return found;
}

bool EnsureAdapter(AdapterRecord& adapter, std::string& error) {
    adapter = FindOrbitLanAdapter();
    if (!adapter.guid.empty()) return true;
    error = "the OrbitLan TAP adapter is missing; reinstall OrbitLan to repair it";
    return false;
}

void StopEngineLocked() {
    if (!ProcessRunningLocked()) return;
    std::string ignored;
    HttpControl(L"POST", L"/shutdown", ignored);
    if (WaitForSingleObject(g_engine_process, 4000) != WAIT_OBJECT_0 && g_engine_job != nullptr) {
        TerminateJobObject(g_engine_job, 0);
        WaitForSingleObject(g_engine_process, 1500);
    }
    CloseHandle(g_engine_process);
    g_engine_process = nullptr;
    if (g_engine_job != nullptr) {
        CloseHandle(g_engine_job);
        g_engine_job = nullptr;
    }
    g_api_token.clear();
}

std::string EngineLogTail() {
    const fs::path path = KnownFolder(FOLDERID_ProgramData) / L"OrbitLan" / L"engine.log";
    std::ifstream input(path, std::ios::binary);
    if (!input) return "engine did not start; see the OrbitLan service log";
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    const std::streamoff start = size > 1000 ? static_cast<std::streamoff>(size) - 1000 : 0;
    input.seekg(start);
    std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    data.erase(std::remove(data.begin(), data.end(), '\r'), data.end());
    std::replace(data.begin(), data.end(), '\n', ' ');
    return data.empty() ? "engine exited before becoming ready" : data;
}

bool StartEngine(std::string_view name, std::string_view code, std::string_view server,
                 std::string_view relay, std::string_view edition, std::string& error) {
    std::scoped_lock lock(g_engine_mutex);
    if (ProcessRunningLocked()) {
        error = "OrbitLan is already connected";
        return false;
    }
    if (name.empty() || name.size() > 64 || code.size() < 3 || code.size() > 64) {
        error = "invalid player name or network code";
        return false;
    }
    if (!std::all_of(code.begin(), code.end(), [](unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
        })) {
        error = "network code may only contain letters, numbers, dashes, and underscores";
        return false;
    }
    if (!(server.starts_with("https://") || server.starts_with("http://"))) {
        error = "coordinator must be an http or https URL";
        return false;
    }
    if (relay != "off" && relay != "auto" && relay != "on") {
        error = "relay mode must be off, auto, or on";
        return false;
    }
    if (edition != "community" && edition != "supporter") {
        error = "edition must be community or supporter";
        return false;
    }
    AdapterRecord adapter;
    if (!EnsureAdapter(adapter, error)) return false;

    const fs::path directory = ExecutableDirectory();
    const fs::path engine = directory / L"OrbitLan.NetworkEngine.exe";
    if (!FileExists(engine)) {
        error = "OrbitLan's network engine is missing from the installation";
        return false;
    }
    g_api_token = RandomToken();
    if (g_api_token.empty()) {
        error = "could not create a local control token";
        return false;
    }

    std::wstring command = QuoteArgument(engine.wstring());
    const std::vector<std::wstring> args = {
        L"-code", Utf8ToWide(code), L"-name", Utf8ToWide(name), L"-server", Utf8ToWide(server),
        L"-relay", Utf8ToWide(relay), L"-edition", Utf8ToWide(edition), L"-datapath", L"tap", L"-api",
        L"127.0.0.1:" + std::to_wstring(kEngineApiPort),
        L"-control-token", Utf8ToWide(g_api_token), L"-adapter", adapter.guid,
    };
    for (const auto& arg : args) command += L" " + QuoteArgument(arg);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    const fs::path log_directory = KnownFolder(FOLDERID_ProgramData) / L"OrbitLan";
    std::error_code filesystem_error;
    fs::create_directories(log_directory, filesystem_error);
    HANDLE log = CreateFileW((log_directory / L"engine.log").c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) log = nullptr;
    if (log != nullptr) SetHandleInformation(log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(engine.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(),
                                        &startup, &process);
    if (log != nullptr) CloseHandle(log);
    if (!created) {
        error = "could not launch engine: " + WideToUtf8(WindowsError());
        g_api_token.clear();
        return false;
    }

    g_engine_job = CreateJobObjectW(nullptr, nullptr);
    if (g_engine_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_engine_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(g_engine_job, process.hProcess);
    }
    g_engine_process = process.hProcess;
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    const auto deadline = std::chrono::steady_clock::now() + 35s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!ProcessRunningLocked()) {
            error = EngineLogTail();
            return false;
        }
        std::string status;
        if (HttpControl(L"GET", L"/status", status)) {
            const size_t marker = status.find("\"myIP\":\"");
            if (marker != std::string::npos && marker + 8 < status.size() && status[marker + 8] != '\"') {
                return true;
            }
        }
        std::this_thread::sleep_for(250ms);
    }
    error = "connection timed out after 35 seconds";
    StopEngineLocked();
    return false;
}

std::string HandleCommand(std::string_view request) {
    const auto fields = Split(request, '\t');
    if (fields.empty()) return "ERR\tempty command";
    if (fields[0] == "PING") return "OK\tPONG";

    if (fields[0] == "CONNECT") {
        if (fields.size() != 5 && fields.size() != 6) return "ERR\tinvalid CONNECT request";
        std::string name, code, server;
        if (!Base64Decode(fields[1], name) || !Base64Decode(fields[2], code) ||
            !Base64Decode(fields[3], server)) {
            return "ERR\tinvalid request encoding";
        }
        const std::string_view edition = fields.size() == 6 ? fields[5] : "community";
        std::string error;
        return StartEngine(name, code, server, fields[4], edition, error) ? "OK\tCONNECTED"
                                                                          : "ERR\t" + error;
    }
    if (fields[0] == "DISCONNECT") {
        std::scoped_lock lock(g_engine_mutex);
        StopEngineLocked();
        return "OK\tOFFLINE";
    }
    if (fields[0] == "PRIORITY") {
        if (fields.size() != 2 || (fields[1] != "0" && fields[1] != "1")) {
            return "ERR\tinvalid PRIORITY request";
        }
        const AdapterRecord adapter = FindOrbitLanAdapter();
        if (adapter.connection_name.empty()) return "ERR\tOrbitLan adapter is unavailable";
        const std::wstring metric = fields[1] == "1" ? L"1" : L"automatic";
        DWORD code = 1;
        const bool ok = RunProcessAndWait(L"netsh.exe",
                                          {L"interface", L"ipv4", L"set", L"interface", adapter.connection_name,
                                           L"metric=" + metric},
                                          {}, 15000, &code);
        return ok ? "OK" : "ERR\tcould not update adapter priority";
    }
    if (fields[0] == "KICK") {
        if (fields.size() != 2) return "ERR\tinvalid KICK request";
        std::string peer_id;
        if (!Base64Decode(fields[1], peer_id) || peer_id.empty() || peer_id.size() > 128 ||
            !std::all_of(peer_id.begin(), peer_id.end(), [](unsigned char ch) {
                return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
            })) {
            return "ERR\tinvalid node";
        }
        std::scoped_lock lock(g_engine_mutex);
        if (!ProcessRunningLocked()) return "ERR\tOrbitLan is offline";
        std::string response;
        const std::wstring path = L"/kick?peerID=" + Utf8ToWide(peer_id);
        if (!HttpControl(L"POST", path, response)) {
            while (!response.empty() && (response.back() == '\r' || response.back() == '\n')) {
                response.pop_back();
            }
            return "ERR\t" + (response.empty() ? "could not remove node" : response);
        }
        return "OK\tREMOVED";
    }
    if (fields[0] == "STATUS") {
        std::scoped_lock lock(g_engine_mutex);
        if (!ProcessRunningLocked()) return "OK\tOFFLINE";
        std::string status;
        if (!HttpControl(L"GET", L"/status", status)) return "OK\tCONNECTING";
        return "OK\tCONNECTED\t" + Base64Encode(status);
    }
    return "ERR\tunknown command";
}

void ServePipe(HANDLE stop_event) {
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1,
        &raw_descriptor, nullptr);
    LocalMemory descriptor(raw_descriptor);
    SECURITY_ATTRIBUTES security{sizeof(security), raw_descriptor, FALSE};

    while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       4, 256 * 1024, 64 * 1024, 1000,
                                       raw_descriptor == nullptr ? nullptr : &security);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(500ms);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            std::string request;
            char buffer[4096];
            for (;;) {
                DWORD read = 0;
                const BOOL ok = ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr);
                if (read > 0) request.append(buffer, read);
                if (ok || GetLastError() != ERROR_MORE_DATA || request.size() > 64 * 1024) break;
            }
            while (!request.empty() && (request.back() == '\r' || request.back() == '\n')) request.pop_back();
            std::string response = HandleCommand(request);
            response.push_back('\n');
            DWORD written = 0;
            WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}

void ReportServiceStatus(DWORD state, DWORD error = NO_ERROR, DWORD wait_hint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = error;
    g_status.dwWaitHint = wait_hint;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING
                                      ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
                                      : 0;
    SetServiceStatus(g_status_handle, &g_status);
}

void WINAPI ServiceControl(DWORD control) {
    if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN) return;
    ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
    SetEvent(g_stop_event);
    // Wake the blocking ConnectNamedPipe call.
    HANDLE wake = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    g_status_handle = RegisterServiceCtrlHandlerW(kServiceName, ServiceControl);
    if (g_status_handle == nullptr) return;
    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        ReportServiceStatus(SERVICE_STOPPED, GetLastError());
        return;
    }
    ReportServiceStatus(SERVICE_RUNNING);
    ServePipe(g_stop_event);
    {
        std::scoped_lock lock(g_engine_mutex);
        StopEngineLocked();
    }
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    ReportServiceStatus(SERVICE_STOPPED);
}

BOOL WINAPI ConsoleControl(DWORD control) {
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT || control == CTRL_CLOSE_EVENT) {
        SetEvent(g_stop_event);
        HANDLE wake = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
        return TRUE;
    }
    return FALSE;
}

}  // namespace
}  // namespace orbitlan

int wmain(int argc, wchar_t** argv) {
    using namespace orbitlan;
    if (argc == 2 && _wcsicmp(argv[1], L"--console") == 0) {
        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(ConsoleControl, TRUE);
        ServePipe(g_stop_event);
        {
            std::scoped_lock lock(g_engine_mutex);
            StopEngineLocked();
        }
        CloseHandle(g_stop_event);
        return 0;
    }
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}

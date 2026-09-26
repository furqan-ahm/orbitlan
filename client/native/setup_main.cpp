#include "common.h"
#include "protocol.h"
#include "resource.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <netcon.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace orbitlan {
namespace {

fs::path InstallDirectory() {
    return KnownFolder(FOLDERID_ProgramFiles) / L"OrbitLan";
}

void SetupLog(const std::wstring& message) {
    const fs::path directory = KnownFolder(FOLDERID_ProgramData) / L"OrbitLan";
    std::error_code ec;
    fs::create_directories(directory, ec);
    std::ofstream output(directory / L"setup.log", std::ios::binary | std::ios::app);
    if (!output) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char timestamp[64]{};
    sprintf_s(timestamp, "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth,
              now.wDay, now.wHour, now.wMinute, now.wSecond);
    output << timestamp << WideToUtf8(message) << "\r\n";
}

struct EmbeddedItem {
    int resource_id;
    fs::path destination;
};

const std::array<EmbeddedItem, 12>& EmbeddedPayload() {
    static const std::array<EmbeddedItem, 12> payload = {{
        {IDR_PAYLOAD_UI, L"OrbitLan.exe"},
        {IDR_PAYLOAD_SERVICE, L"OrbitLan.NetworkService.exe"},
        {IDR_PAYLOAD_ENGINE, L"OrbitLan.NetworkEngine.exe"},
        {IDR_PAYLOAD_EARTH, fs::path(L"assets") / L"earth-clouds.gif"},
        {IDR_PAYLOAD_HEADER_EARTH, fs::path(L"assets") / L"earth-header-sheet.png"},
        {IDR_PAYLOAD_MOON, fs::path(L"assets") / L"moon-supporter.png"},
        {IDR_PAYLOAD_DEVCON, fs::path(L"driver") / L"devcon.exe"},
        {IDR_PAYLOAD_DRIVER_INF, fs::path(L"driver") / L"OemVista.inf"},
        {IDR_PAYLOAD_DRIVER_CAT, fs::path(L"driver") / L"tap0901.cat"},
        {IDR_PAYLOAD_DRIVER_SYS, fs::path(L"driver") / L"tap0901.sys"},
        {IDR_PAYLOAD_LICENSE_GPL, fs::path(L"driver") / L"LICENSE-GPL-2.0.txt"},
        {IDR_PAYLOAD_LICENSE_MS, fs::path(L"driver") / L"LICENSE-MS-PL.txt"},
    }};
    return payload;
}

struct ResourceView {
    const unsigned char* data = nullptr;
    DWORD size = 0;
};

fs::path CurrentExecutable() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
}

ResourceView EmbeddedResource(int resource_id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (resource == nullptr) return {};
    HGLOBAL loaded = LoadResource(module, resource);
    if (loaded == nullptr) return {};
    return {static_cast<const unsigned char*>(LockResource(loaded)), SizeofResource(module, resource)};
}

bool IsWindowsAmd64Image(const unsigned char* data, size_t size) {
    if (data == nullptr || size < 64 || data[0] != 'M' || data[1] != 'Z') return false;
    const unsigned long pe_offset = static_cast<unsigned long>(data[0x3c]) |
                                    (static_cast<unsigned long>(data[0x3d]) << 8) |
                                    (static_cast<unsigned long>(data[0x3e]) << 16) |
                                    (static_cast<unsigned long>(data[0x3f]) << 24);
    return pe_offset <= size - 6 && data[pe_offset] == 'P' && data[pe_offset + 1] == 'E' &&
           data[pe_offset + 2] == 0 && data[pe_offset + 3] == 0 &&
           data[pe_offset + 4] == 0x64 && data[pe_offset + 5] == 0x86;
}

bool ValidateEmbeddedPayload(std::wstring& error) {
    for (const auto& item : EmbeddedPayload()) {
        const ResourceView resource = EmbeddedResource(item.resource_id);
        if (resource.data == nullptr || resource.size == 0) {
            error = L"The OrbitLan installer is damaged. Missing embedded component: " +
                    item.destination.wstring();
            return false;
        }
    }
    const ResourceView engine = EmbeddedResource(IDR_PAYLOAD_ENGINE);
    if (!IsWindowsAmd64Image(engine.data, engine.size)) {
        error = L"The embedded OrbitLan network engine is not a Windows x64 executable.";
        return false;
    }
    return true;
}

bool ExtractEmbeddedPayload(const fs::path& destination, std::wstring& error) {
    std::error_code ec;
    for (const auto& item : EmbeddedPayload()) {
        const ResourceView resource = EmbeddedResource(item.resource_id);
        const fs::path target = destination / item.destination;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            error = L"Could not create the installation folder: " + Utf8ToWide(ec.message());
            return false;
        }
        fs::path temporary = target;
        temporary += L".installing";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output.write(reinterpret_cast<const char*>(resource.data), resource.size)) {
                error = L"Could not extract " + item.destination.wstring() + L".";
                return false;
            }
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = L"Could not install " + item.destination.wstring() + L": " + WindowsError();
            fs::remove(temporary, ec);
            return false;
        }
    }
    const fs::path uninstall = destination / L"OrbitLan.Uninstall.exe";
    const fs::path current = CurrentExecutable();
    if (!(fs::equivalent(current, uninstall, ec) && !ec)) {
        ec.clear();
        fs::copy_file(current, uninstall, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"Could not install the OrbitLan uninstaller: " + Utf8ToWide(ec.message());
            return false;
        }
    }
    return true;
}

bool HasEmbeddedPayload() {
    const ResourceView ui = EmbeddedResource(IDR_PAYLOAD_UI);
    return ui.data != nullptr && ui.size != 0;
}

fs::path LoosePayloadRoot() {
    const fs::path executable = ExecutableDirectory();
    if (FileExists(executable / L"OrbitLan.exe")) return executable;
    if (FileExists(executable.parent_path() / L"OrbitLan.exe")) return executable.parent_path();
    return executable;
}

fs::path LoosePayloadFile(const fs::path& root, const fs::path& relative) {
    const fs::path direct = root / relative;
    return FileExists(direct) ? direct : root / L"support" / relative;
}

fs::path LooseSetupFile(const fs::path& root) {
    const std::array candidates = {
        root / L"support" / L"OrbitLan.Setup.exe",
        root / L"OrbitLan.Setup.exe",
        root / L"OrbitLan.Uninstall.exe",
    };
    const auto found = std::find_if(candidates.begin(), candidates.end(), [](const fs::path& path) {
        return FileExists(path);
    });
    return found == candidates.end() ? fs::path{} : *found;
}

bool IsWindowsAmd64Executable(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streamsize size = input.tellg();
    if (size < 64 || size > 128 * 1024 * 1024) return false;
    std::vector<unsigned char> data(static_cast<size_t>(size));
    input.seekg(0);
    return input.read(reinterpret_cast<char*>(data.data()), size) &&
           IsWindowsAmd64Image(data.data(), data.size());
}

bool ValidateLoosePayload(const fs::path& root, std::wstring& error) {
    for (const auto& item : EmbeddedPayload()) {
        if (!FileExists(LoosePayloadFile(root, item.destination))) {
            error = L"The portable OrbitLan package is incomplete. Missing: " +
                    item.destination.wstring();
            return false;
        }
    }
    if (!FileExists(LooseSetupFile(root))) {
        error = L"The portable OrbitLan package is incomplete. Missing its setup helper.";
        return false;
    }
    if (!IsWindowsAmd64Executable(LoosePayloadFile(root, L"OrbitLan.NetworkEngine.exe"))) {
        error = L"The portable package contains an invalid Windows network engine.";
        return false;
    }
    return true;
}

bool CopyLoosePayload(const fs::path& root, const fs::path& destination,
                      bool runtime_only, std::wstring& error) {
    std::error_code ec;
    for (const auto& item : EmbeddedPayload()) {
        if (runtime_only && (item.resource_id == IDR_PAYLOAD_UI ||
                             item.resource_id == IDR_PAYLOAD_EARTH ||
                             item.resource_id == IDR_PAYLOAD_HEADER_EARTH ||
                             item.resource_id == IDR_PAYLOAD_MOON)) {
            continue;
        }
        const fs::path source = LoosePayloadFile(root, item.destination);
        const fs::path target = destination / item.destination;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            error = L"Could not create the installation folder: " + Utf8ToWide(ec.message());
            return false;
        }
        if (fs::equivalent(source, target, ec) && !ec) continue;
        ec.clear();
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"Could not install " + item.destination.wstring() + L": " +
                    Utf8ToWide(ec.message());
            return false;
        }
    }
    const fs::path setup = LooseSetupFile(root);
    const fs::path uninstall = destination / L"OrbitLan.Uninstall.exe";
    if (!(fs::equivalent(setup, uninstall, ec) && !ec)) {
        ec.clear();
        fs::copy_file(setup, uninstall, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"Could not install the OrbitLan uninstaller: " + Utf8ToWide(ec.message());
            return false;
        }
    }
    return true;
}

struct TapAdapter {
    std::wstring driver_key;
    std::wstring guid;
    std::wstring connection_name;
};

std::wstring RegistryString(HKEY parent, const std::wstring& subkey, const wchar_t* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(parent, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
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

std::vector<TapAdapter> TapAdapters() {
    constexpr wchar_t class_key[] =
        LR"(SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318})";
    constexpr wchar_t connection_key[] =
        LR"(SYSTEM\CurrentControlSet\Control\Network\{4D36E972-E325-11CE-BFC1-08002BE10318})";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_key, 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        return {};
    }
    std::vector<TapAdapter> adapters;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD name_size = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(root, index, name, &name_size, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) continue;
        HKEY child = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &child) != ERROR_SUCCESS) continue;
        wchar_t component[128]{};
        wchar_t guid[128]{};
        DWORD type = 0;
        DWORD component_bytes = sizeof(component);
        DWORD guid_bytes = sizeof(guid);
        const bool is_tap =
            RegQueryValueExW(child, L"ComponentId", nullptr, &type,
                             reinterpret_cast<BYTE*>(component), &component_bytes) == ERROR_SUCCESS &&
            type == REG_SZ && _wcsicmp(component, L"tap0901") == 0;
        type = 0;
        const bool has_guid =
            RegQueryValueExW(child, L"NetCfgInstanceId", nullptr, &type,
                             reinterpret_cast<BYTE*>(guid), &guid_bytes) == ERROR_SUCCESS &&
            type == REG_SZ && guid[0] != 0;
        RegCloseKey(child);
        if (!is_tap || !has_guid) continue;
        const std::wstring connection = std::wstring(connection_key) + L"\\" + guid + L"\\Connection";
        adapters.push_back({name, guid, RegistryString(HKEY_LOCAL_MACHINE, connection, L"Name")});
    }
    RegCloseKey(root);
    return adapters;
}

const TapAdapter* OrbitLanAdapter(const std::vector<TapAdapter>& adapters) {
    const auto found = std::find_if(adapters.begin(), adapters.end(), [](const TapAdapter& adapter) {
        return _wcsicmp(adapter.connection_name.c_str(), L"OrbitLan") == 0;
    });
    return found == adapters.end() ? nullptr : &*found;
}

std::wstring DeviceInstanceId(const TapAdapter& adapter) {
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return {};
    std::wstring instance;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) break;
        wchar_t driver[512]{};
        DWORD type = 0;
        DWORD bytes = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_DRIVER, &type,
                                               reinterpret_cast<BYTE*>(driver), sizeof(driver), &bytes) ||
            type != REG_SZ) {
            continue;
        }
        const std::wstring driver_path = driver;
        const size_t slash = driver_path.find_last_of(L'\\');
        if (slash == std::wstring::npos ||
            _wcsicmp(driver_path.c_str() + slash + 1, adapter.driver_key.c_str()) != 0) {
            continue;
        }
        wchar_t id[512]{};
        if (SetupDiGetDeviceInstanceIdW(devices, &device, id,
                                        static_cast<DWORD>(std::size(id)), nullptr)) {
            instance = id;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return instance;
}

void WriteAdapterOwnership(const TapAdapter& adapter) {
    const std::wstring instance_id = DeviceInstanceId(adapter);
    if (instance_id.empty()) return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\OrbitLan)", 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, L"AdapterInstanceId", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(instance_id.c_str()),
                   static_cast<DWORD>((instance_id.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"AdapterGuid", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(adapter.guid.c_str()),
                   static_cast<DWORD>((adapter.guid.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

const TapAdapter* OwnedAdapter(const std::vector<TapAdapter>& adapters) {
    const std::wstring owned_instance = RegistryString(
        HKEY_LOCAL_MACHINE, LR"(SOFTWARE\OrbitLan)", L"AdapterInstanceId");
    if (owned_instance.empty()) return nullptr;
    const auto found = std::find_if(adapters.begin(), adapters.end(), [&](const TapAdapter& adapter) {
        return _wcsicmp(DeviceInstanceId(adapter).c_str(), owned_instance.c_str()) == 0;
    });
    return found == adapters.end() ? nullptr : &*found;
}

void FreeConnectionProperties(NETCON_PROPERTIES* properties) {
    if (properties == nullptr) return;
    CoTaskMemFree(properties->pszwName);
    CoTaskMemFree(properties->pszwDeviceName);
    CoTaskMemFree(properties);
}

bool RenameAdapter(const std::wstring& guid_string, std::wstring& error) {
    GUID target{};
    if (FAILED(CLSIDFromString(guid_string.c_str(), &target))) {
        error = L"Windows returned an invalid identifier for the OrbitLan adapter.";
        return false;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        error = L"Windows could not initialize network setup.";
        return false;
    }

    bool renamed = false;
    HRESULT last_error = E_FAIL;
    for (int attempt = 0; attempt < 4 && !renamed; ++attempt) {
        INetConnectionManager* manager = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ConnectionManager, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&manager));
        IEnumNetConnection* connections = nullptr;
        if (SUCCEEDED(hr)) hr = manager->EnumConnections(NCME_DEFAULT, &connections);
        if (SUCCEEDED(hr)) {
            INetConnection* connection = nullptr;
            ULONG fetched = 0;
            while (connections->Next(1, &connection, &fetched) == S_OK) {
                NETCON_PROPERTIES* properties = nullptr;
                hr = connection->GetProperties(&properties);
                const bool match = SUCCEEDED(hr) && properties != nullptr &&
                                   IsEqualGUID(properties->guidId, target);
                FreeConnectionProperties(properties);
                if (match) {
                    hr = connection->Rename(L"OrbitLan");
                    connection->Release();
                    renamed = SUCCEEDED(hr);
                    last_error = hr;
                    break;
                }
                connection->Release();
                connection = nullptr;
            }
        }
        if (connections != nullptr) connections->Release();
        if (manager != nullptr) manager->Release();
        if (!renamed) Sleep(250);
    }
    if (uninitialize) CoUninitialize();
    if (!renamed) {
        wchar_t code[16]{};
        swprintf_s(code, L"0x%08X", static_cast<unsigned>(last_error));
        error = L"Windows created the TAP adapter but could not name it OrbitLan (" +
                std::wstring(code) + L").";
    }
    return renamed;
}

bool FinishAdapterSetup(const TapAdapter& adapter, std::wstring& error) {
    const std::wstring instance = DeviceInstanceId(adapter);
    if (instance.empty()) {
        error = L"Windows created the TAP adapter but its device identity is not ready.";
        return false;
    }
    WriteAdapterOwnership(adapter);
    if (_wcsicmp(adapter.connection_name.c_str(), L"OrbitLan") != 0 &&
        !RenameAdapter(adapter.guid, error)) {
        SetupLog(L"WARNING: " + error + L" Continuing with the adapter's Windows name: " +
                 adapter.connection_name);
        error.clear();
    }
    const auto verified = TapAdapters();
    if (OwnedAdapter(verified) == nullptr) {
        error = L"The OrbitLan TAP adapter was created but its ownership could not be verified.";
        return false;
    }
    return true;
}

bool InstallAdapter(const fs::path& directory, std::wstring& error) {
    auto before = TapAdapters();
    if (const TapAdapter* existing = OrbitLanAdapter(before); existing != nullptr) {
        WriteAdapterOwnership(*existing);
        return true;
    }
    if (const TapAdapter* interrupted = OwnedAdapter(before); interrupted != nullptr) {
        return FinishAdapterSetup(*interrupted, error);
    }
    const fs::path driver = directory / L"driver";
    DWORD exit_code = 1;
    if (!RunProcessAndWait(driver / L"devcon.exe",
                           {L"install", (driver / L"OemVista.inf").wstring(), L"tap0901"},
                           driver, 60000, &exit_code)) {
        error = L"The signed OrbitLan network adapter could not be installed (devcon exit " +
                std::to_wstring(exit_code) + L").";
        return false;
    }
    auto after = TapAdapters();
    TapAdapter* created = nullptr;
    for (auto& candidate : after) {
        const bool existed = std::any_of(before.begin(), before.end(), [&](const TapAdapter& old) {
            return _wcsicmp(old.guid.c_str(), candidate.guid.c_str()) == 0;
        });
        if (!existed) {
            created = &candidate;
            break;
        }
    }
    if (created == nullptr || created->connection_name.empty()) {
        error = L"Windows installed the TAP driver, but OrbitLan could not identify its new adapter.";
        return false;
    }
    return FinishAdapterSetup(*created, error);
}

bool StopAndDeleteService(bool remove, std::wstring& error) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        error = L"Could not open the Windows service manager: " + WindowsError();
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, kServiceName,
                                     SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == nullptr) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        if (code == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        error = L"Could not open the OrbitLan service: " + WindowsError(code);
        return false;
    }

    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    for (int attempt = 0; attempt < 50; ++attempt) {
        SERVICE_STATUS_PROCESS current{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&current), sizeof(current), &bytes) ||
            current.dwCurrentState == SERVICE_STOPPED) {
            break;
        }
        Sleep(100);
    }
    bool ok = true;
    if (remove && !DeleteService(service) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
        error = L"Could not remove the OrbitLan service: " + WindowsError();
        ok = false;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

bool CreateOrbitLanService(const fs::path& directory, std::wstring& error) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        error = L"Could not open the Windows service manager: " + WindowsError();
        return false;
    }
    const std::wstring binary = L"\"" + (directory / L"OrbitLan.NetworkService.exe").wstring() + L"\"";
    SC_HANDLE service = CreateServiceW(
        manager, kServiceName, kServiceDisplayName,
        SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        binary.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr);
    if (service == nullptr && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(manager, kServiceName,
                               SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
        if (service != nullptr) {
            ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                 SERVICE_NO_CHANGE, binary.c_str(), nullptr, nullptr, nullptr,
                                 nullptr, nullptr, kServiceDisplayName);
        }
    }
    if (service == nullptr) {
        error = L"Could not create the OrbitLan service: " + WindowsError();
        CloseServiceHandle(manager);
        return false;
    }
    SERVICE_DESCRIPTIONW description{
        const_cast<LPWSTR>(L"Runs OrbitLan's encrypted virtual network without elevating the user interface.")};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
    SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
    if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        error = L"OrbitLan installed, but its service could not start: " + WindowsError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return false;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return true;
}

bool CreateStartMenuShortcut(const fs::path& directory, std::wstring& error) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) {
        link->SetPath((directory / L"OrbitLan.exe").c_str());
        link->SetWorkingDirectory(directory.c_str());
        link->SetDescription(L"Open OrbitLan");
        link->SetIconLocation((directory / L"OrbitLan.exe").c_str(), 0);
        IPersistFile* persist = nullptr;
        hr = link->QueryInterface(IID_PPV_ARGS(&persist));
        if (SUCCEEDED(hr)) {
            const fs::path shortcut = KnownFolder(FOLDERID_CommonPrograms) / L"OrbitLan.lnk";
            hr = persist->Save(shortcut.c_str(), TRUE);
            persist->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (FAILED(hr)) {
        error = L"Could not create the Start Menu shortcut.";
        return false;
    }
    return true;
}

void WriteUninstallRegistration(const fs::path& directory, bool portable) {
    constexpr wchar_t key_name[] =
        LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OrbitLan)";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_name, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const std::wstring uninstall = L"\"" + (directory / L"OrbitLan.Uninstall.exe").wstring() +
                                   L"\" --uninstall";
    const std::wstring display_icon =
        (directory / (portable ? L"OrbitLan.Uninstall.exe" : L"OrbitLan.exe")).wstring();
    const DWORD one = 1;
    const auto set_string = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    set_string(L"DisplayName", portable ? L"OrbitLan Network Components" : L"OrbitLan");
    set_string(L"DisplayVersion", L"1.0.0");
    set_string(L"Publisher", L"Furqan Ahmad");
    set_string(L"InstallLocation", directory.wstring());
    set_string(L"DisplayIcon", display_icon);
    set_string(L"UninstallString", uninstall);
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
}

void RemoveLegacyPayload(const fs::path& directory) {
    const std::array legacy = {
        directory / L"OrbitLanService.exe",
        directory / L"orbitlan-engine.exe",
        directory / L"OrbitLanSetup.exe",
        directory / L"OrbitLanUninstall.exe",
    };
    for (const auto& file : legacy) {
        std::error_code ec;
        if (!fs::remove(file, ec) && FileExists(file)) {
            MoveFileExW(file.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
}

void AdoptLegacyAdapterIfUnambiguous(const fs::path& directory) {
    const bool legacy_install = FileExists(directory / L"OrbitLanService.exe") ||
                                FileExists(directory / L"orbitlan-engine.exe") ||
                                FileExists(directory / L"OrbitLanSetup.exe") ||
                                FileExists(directory / L"OrbitLanUninstall.exe");
    if (!legacy_install || !RegistryString(
            HKEY_LOCAL_MACHINE, LR"(SOFTWARE\OrbitLan)", L"AdapterInstanceId").empty()) {
        return;
    }
    const auto adapters = TapAdapters();
    if (adapters.size() == 1) WriteAdapterOwnership(adapters.front());
}

bool Install(bool portable, std::wstring& error) {
    const fs::path destination = InstallDirectory();
    const bool embedded = HasEmbeddedPayload();
    const fs::path portable_root = embedded ? fs::path{} : LoosePayloadRoot();
    SetupLog(embedded ? L"Validating the embedded Windows payload"
                      : L"Validating the portable Windows payload");
    if (embedded) {
        if (!ValidateEmbeddedPayload(error)) return false;
    } else if (!ValidateLoosePayload(portable_root, error)) {
        return false;
    }
    SetupLog(L"Stopping the previous OrbitLan service");
    if (!StopAndDeleteService(true, error)) return false;
    SetupLog(L"Extracting application components");
    if (embedded) {
        if (!ExtractEmbeddedPayload(destination, error)) return false;
    } else if (!CopyLoosePayload(portable_root, destination, portable, error)) {
        return false;
    }
    AdoptLegacyAdapterIfUnambiguous(destination);
    RemoveLegacyPayload(destination);
    SetupLog(L"Installing and naming the OrbitLan TAP adapter");
    if (!InstallAdapter(destination, error)) return false;
    SetupLog(L"Creating the OrbitLan service");
    if (!CreateOrbitLanService(destination, error)) return false;
    SetupLog(portable ? L"Registering portable network components"
                      : L"Creating shortcuts and uninstall registration");
    if (!portable && !CreateStartMenuShortcut(destination, error)) return false;
    WriteUninstallRegistration(destination, portable);
    SetupLog(L"Installation completed successfully");
    return true;
}

void ScheduleRemoval(const fs::path& path) {
    if (FileExists(path)) MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

bool Uninstall(std::wstring& error) {
    const fs::path directory = InstallDirectory();
    if (!StopAndDeleteService(true, error)) return false;
    const fs::path devcon = directory / L"driver" / L"devcon.exe";
    if (FileExists(devcon)) {
        std::wstring instance = RegistryString(
            HKEY_LOCAL_MACHINE, LR"(SOFTWARE\OrbitLan)", L"AdapterInstanceId");
        if (instance.empty()) {
            const auto adapters = TapAdapters();
            if (const TapAdapter* owned = OrbitLanAdapter(adapters); owned != nullptr) {
                instance = DeviceInstanceId(*owned);
            }
        }
        DWORD ignored = 0;
        if (!instance.empty()) {
            RunProcessAndWait(devcon, {L"remove", L"@" + instance},
                              devcon.parent_path(), 30000, &ignored);
        }
    }
    DWORD ignored = 0;
    RunProcessAndWait(L"netsh.exe", {L"advfirewall", L"firewall", L"delete", L"rule",
                                     L"name=OrbitLan"}, {}, 15000, &ignored);
    std::error_code ec;
    fs::remove(KnownFolder(FOLDERID_CommonPrograms) / L"OrbitLan.lnk", ec);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                   LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OrbitLan)");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\OrbitLan)");

    const std::vector<fs::path> files = {
        directory / L"OrbitLan.exe", directory / L"OrbitLan.NetworkService.exe",
        directory / L"OrbitLan.NetworkEngine.exe", directory / L"assets" / L"earth-clouds.gif",
        directory / L"assets" / L"earth-header-sheet.png",
        directory / L"assets" / L"moon-supporter.png",
        directory / L"driver" / L"devcon.exe",
        directory / L"driver" / L"OemVista.inf", directory / L"driver" / L"tap0901.cat",
        directory / L"driver" / L"tap0901.sys", directory / L"driver" / L"LICENSE-GPL-2.0.txt",
        directory / L"driver" / L"LICENSE-MS-PL.txt", directory / L"OrbitLan.Setup.exe",
        directory / L"OrbitLan.Uninstall.exe",
        directory / L"OrbitLanService.exe", directory / L"orbitlan-engine.exe",
        directory / L"OrbitLanSetup.exe", directory / L"OrbitLanUninstall.exe",
    };
    for (const auto& file : files) {
        if (!fs::remove(file, ec)) ScheduleRemoval(file);
        ec.clear();
    }
    fs::remove(directory / L"driver", ec);
    fs::remove(directory / L"assets", ec);
    fs::remove(directory, ec);
    if (fs::exists(directory, ec)) MoveFileExW(directory.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}

}  // namespace
}  // namespace orbitlan

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    using namespace orbitlan;
    const bool uninstall = command_line != nullptr && wcsstr(command_line, L"--uninstall") != nullptr;
    const bool portable = command_line != nullptr && wcsstr(command_line, L"--portable") != nullptr;
    if (uninstall && MessageBoxW(nullptr, L"Disconnect and remove OrbitLan from this computer?",
                                 L"Uninstall OrbitLan", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return 0;
    }
    std::wstring error;
    const bool ok = uninstall ? Uninstall(error) : Install(portable, error);
    if (!ok) {
        SetupLog(L"ERROR: " + error);
        MessageBoxW(nullptr, error.c_str(), uninstall ? L"OrbitLan uninstall failed" : L"OrbitLan setup failed",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!uninstall && !portable) {
        const fs::path app = InstallDirectory() / L"OrbitLan.exe";
        const std::wstring argument = QuoteArgument(app.wstring());
        ShellExecuteW(nullptr, L"open", L"explorer.exe", argument.c_str(),
                      InstallDirectory().c_str(), SW_SHOWNORMAL);
        return 0;
    }
    if (portable) return 0;
    MessageBoxW(nullptr, L"OrbitLan was removed. Files in use will be cleared after the next restart.",
                L"OrbitLan removed", MB_OK | MB_ICONINFORMATION);
    return 0;
}

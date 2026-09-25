#include "common.h"
#include "protocol.h"
#include "resource.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace orbitlan {
namespace {

fs::path InstallDirectory() {
    return KnownFolder(FOLDERID_ProgramFiles) / L"OrbitLan";
}

bool AdapterPresent() {
    constexpr wchar_t class_key[] =
        LR"(SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318})";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_key, 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        return false;
    }
    bool present = false;
    for (DWORD index = 0; !present; ++index) {
        wchar_t name[256]{};
        DWORD name_size = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(root, index, name, &name_size, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) continue;
        HKEY child = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &child) != ERROR_SUCCESS) continue;
        wchar_t component[128]{};
        DWORD type = 0;
        DWORD bytes = sizeof(component);
        if (RegQueryValueExW(child, L"ComponentId", nullptr, &type,
                             reinterpret_cast<BYTE*>(component), &bytes) == ERROR_SUCCESS &&
            type == REG_SZ && _wcsicmp(component, L"tap0901") == 0) {
            present = true;
        }
        RegCloseKey(child);
    }
    RegCloseKey(root);
    return present;
}

bool InstallAdapter(const fs::path& directory, std::wstring& error) {
    if (AdapterPresent()) return true;
    const fs::path driver = directory / L"driver";
    DWORD exit_code = 1;
    if (!RunProcessAndWait(driver / L"devcon.exe",
                           {L"install", (driver / L"OemVista.inf").wstring(), L"tap0901"},
                           driver, 60000, &exit_code) || !AdapterPresent()) {
        error = L"The signed OrbitLan network adapter could not be installed (devcon exit " +
                std::to_wstring(exit_code) + L").";
        return false;
    }
    return true;
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

bool CopyPayload(const fs::path& source, const fs::path& destination, std::wstring& error) {
    std::error_code ec;
    fs::create_directories(destination / L"driver", ec);
    if (ec) {
        error = L"Could not create the driver installation folder: " + Utf8ToWide(ec.message());
        return false;
    }
    fs::create_directories(destination / L"assets", ec);
    if (ec) {
        error = L"Could not create the installation folder: " + Utf8ToWide(ec.message());
        return false;
    }
    const std::vector<fs::path> files = {
        L"OrbitLan.exe",
        L"OrbitLanService.exe",
        L"OrbitLanSetup.exe",
        L"orbitlan-engine.exe",
        fs::path(L"assets") / L"earth-clouds.gif",
        fs::path(L"driver") / L"devcon.exe",
        fs::path(L"driver") / L"OemVista.inf",
        fs::path(L"driver") / L"tap0901.cat",
        fs::path(L"driver") / L"tap0901.sys",
        fs::path(L"driver") / L"LICENSE-GPL-2.0.txt",
        fs::path(L"driver") / L"LICENSE-MS-PL.txt",
    };
    for (const auto& relative : files) {
        const fs::path from = source / relative;
        const fs::path to = destination / relative;
        if (!FileExists(from)) {
            error = L"The release package is incomplete. Missing: " + relative.wstring();
            return false;
        }
        if (fs::equivalent(from, to, ec) && !ec) continue;
        ec.clear();
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"Could not install " + relative.wstring() + L": " + Utf8ToWide(ec.message());
            return false;
        }
    }
    return true;
}

bool CreateOrbitLanService(const fs::path& directory, std::wstring& error) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        error = L"Could not open the Windows service manager: " + WindowsError();
        return false;
    }
    const std::wstring binary = L"\"" + (directory / L"OrbitLanService.exe").wstring() + L"\"";
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

void WriteUninstallRegistration(const fs::path& directory) {
    constexpr wchar_t key_name[] =
        LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OrbitLan)";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_name, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const std::wstring uninstall = L"\"" + (directory / L"OrbitLanSetup.exe").wstring() +
                                   L"\" --uninstall";
    const std::wstring display_icon = (directory / L"OrbitLan.exe").wstring();
    const DWORD one = 1;
    const auto set_string = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    set_string(L"DisplayName", L"OrbitLan");
    set_string(L"DisplayVersion", L"2.0.0");
    set_string(L"Publisher", L"Furqan Ahmad");
    set_string(L"InstallLocation", directory.wstring());
    set_string(L"DisplayIcon", display_icon);
    set_string(L"UninstallString", uninstall);
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
}

bool Install(std::wstring& error) {
    const fs::path source = ExecutableDirectory();
    const fs::path destination = InstallDirectory();
    if (!StopAndDeleteService(true, error)) return false;
    if (!CopyPayload(source, destination, error)) return false;
    if (!InstallAdapter(destination, error)) return false;
    if (!CreateOrbitLanService(destination, error)) return false;
    if (!CreateStartMenuShortcut(destination, error)) return false;
    WriteUninstallRegistration(destination);
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
        DWORD ignored = 0;
        RunProcessAndWait(devcon, {L"remove", L"tap0901"}, devcon.parent_path(), 30000, &ignored);
    }
    DWORD ignored = 0;
    RunProcessAndWait(L"netsh.exe", {L"advfirewall", L"firewall", L"delete", L"rule",
                                     L"name=OrbitLan"}, {}, 15000, &ignored);
    std::error_code ec;
    fs::remove(KnownFolder(FOLDERID_CommonPrograms) / L"OrbitLan.lnk", ec);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                   LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OrbitLan)");

    const std::vector<fs::path> files = {
        directory / L"OrbitLan.exe", directory / L"OrbitLanService.exe",
        directory / L"orbitlan-engine.exe", directory / L"assets" / L"earth-clouds.gif",
        directory / L"driver" / L"devcon.exe",
        directory / L"driver" / L"OemVista.inf", directory / L"driver" / L"tap0901.cat",
        directory / L"driver" / L"tap0901.sys", directory / L"driver" / L"LICENSE-GPL-2.0.txt",
        directory / L"driver" / L"LICENSE-MS-PL.txt", directory / L"OrbitLanSetup.exe",
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
    if (uninstall && MessageBoxW(nullptr, L"Disconnect and remove OrbitLan from this computer?",
                                 L"Uninstall OrbitLan", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return 0;
    }
    std::wstring error;
    const bool ok = uninstall ? Uninstall(error) : Install(error);
    if (!ok) {
        MessageBoxW(nullptr, error.c_str(), uninstall ? L"OrbitLan uninstall failed" : L"OrbitLan setup failed",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    MessageBoxW(nullptr,
                uninstall ? L"OrbitLan was removed. Files in use will be cleared after the next restart."
                          : L"OrbitLan is installed. Open it from the Start menu. Future launches will not request administrator approval.",
                uninstall ? L"OrbitLan removed" : L"OrbitLan is ready", MB_OK | MB_ICONINFORMATION);
    return 0;
}

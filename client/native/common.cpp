#include "common.h"

#include "protocol.h"

#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <memory>
#include <sstream>

namespace orbitlan {
namespace {

struct HandleCloser {
    void operator()(void* value) const {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(value));
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

}  // namespace

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string Base64Encode(std::string_view value) {
    if (value.empty()) return {};
    DWORD size = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(value.data()),
                              static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size)) {
        return {};
    }
    std::string result(size, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(value.data()),
                              static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &size)) {
        return {};
    }
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

bool Base64Decode(std::string_view value, std::string& output) {
    output.clear();
    if (value.empty()) return true;
    DWORD size = 0;
    if (!CryptStringToBinaryA(value.data(), static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) {
        return false;
    }
    output.resize(size);
    return CryptStringToBinaryA(value.data(), static_cast<DWORD>(value.size()),
                                CRYPT_STRING_BASE64,
                                reinterpret_cast<BYTE*>(output.data()), &size, nullptr, nullptr) != FALSE;
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
    std::vector<std::string> pieces;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(delimiter, start);
        pieces.emplace_back(value.substr(start, end == std::string_view::npos ? value.size() - start
                                                                              : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return pieces;
}

std::wstring QuoteArgument(std::wstring_view value) {
    if (value.find_first_of(L" \t\"") == std::wstring_view::npos) return std::wstring(value);
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            slashes = 0;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(size);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::wstring WindowsError(DWORD error) {
    wchar_t* message = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = size == 0 ? L"Windows error " + std::to_wstring(error)
                                    : std::wstring(message, size);
    if (message != nullptr) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

bool FileExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

PipeReply CallService(std::string_view request, DWORD timeout_ms) {
    PipeReply reply;
    if (!WaitNamedPipeW(kPipeName, timeout_ms)) {
        reply.error = WideToUtf8(WindowsError());
        return reply;
    }
    UniqueHandle pipe(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE) {
        pipe.release();
        reply.error = WideToUtf8(WindowsError());
        return reply;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(static_cast<HANDLE>(pipe.get()), &mode, nullptr, nullptr);
    std::string wire(request);
    wire.push_back('\n');
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(pipe.get()), wire.data(), static_cast<DWORD>(wire.size()),
                   &written, nullptr)) {
        reply.error = WideToUtf8(WindowsError());
        return reply;
    }

    std::string response;
    char block[4096];
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(static_cast<HANDLE>(pipe.get()), block, sizeof(block), &read, nullptr);
        if (read > 0) response.append(block, read);
        if (ok || GetLastError() != ERROR_MORE_DATA) break;
        if (response.size() > 256 * 1024) {
            reply.error = "service response too large";
            return reply;
        }
    }
    reply.transport_ok = true;
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) response.pop_back();
    if (response == "OK") {
        reply.command_ok = true;
    } else if (response.starts_with("OK\t")) {
        reply.command_ok = true;
        reply.payload = response.substr(3);
    } else if (response.starts_with("ERR\t")) {
        reply.error = response.substr(4);
    } else {
        reply.error = "invalid service response";
    }
    return reply;
}

bool RunProcessAndWait(const std::filesystem::path& executable,
                       const std::vector<std::wstring>& arguments,
                       const std::filesystem::path& working_directory,
                       DWORD timeout_ms,
                       DWORD* exit_code,
                       bool hidden) {
    std::wstring command = QuoteArgument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += QuoteArgument(argument);
    }
    STARTUPINFOW startup{sizeof(startup)};
    if (hidden) {
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL created = CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                                        hidden ? CREATE_NO_WINDOW : 0, nullptr,
                                        working_directory.empty() ? nullptr : working_directory.c_str(),
                                        &startup, &process);
    if (!created) return false;
    UniqueHandle thread(process.hThread);
    UniqueHandle handle(process.hProcess);
    if (WaitForSingleObject(process.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 1000);
        if (exit_code != nullptr) *exit_code = ERROR_TIMEOUT;
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    if (exit_code != nullptr) *exit_code = code;
    return code == 0;
}

}  // namespace orbitlan

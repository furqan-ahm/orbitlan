#pragma once

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orbitlan {

std::wstring Utf8ToWide(std::string_view value);
std::string WideToUtf8(std::wstring_view value);
std::string Base64Encode(std::string_view value);
bool Base64Decode(std::string_view value, std::string& output);
std::vector<std::string> Split(std::string_view value, char delimiter);
std::wstring QuoteArgument(std::wstring_view value);
std::filesystem::path ExecutableDirectory();
std::filesystem::path KnownFolder(REFKNOWNFOLDERID id);
std::wstring WindowsError(DWORD error = GetLastError());
bool FileExists(const std::filesystem::path& path);

struct PipeReply {
    bool transport_ok = false;
    bool command_ok = false;
    std::string payload;
    std::string error;
};

PipeReply CallService(std::string_view request, DWORD timeout_ms = 2500);

bool RunProcessAndWait(const std::filesystem::path& executable,
                       const std::vector<std::wstring>& arguments,
                       const std::filesystem::path& working_directory,
                       DWORD timeout_ms,
                       DWORD* exit_code = nullptr,
                       bool hidden = true);

}  // namespace orbitlan

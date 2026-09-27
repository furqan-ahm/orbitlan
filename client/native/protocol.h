#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace orbitlan {

inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\OrbitLan.Control.v1)";
inline constexpr wchar_t kServiceName[] = L"OrbitLanService";
inline constexpr wchar_t kServiceDisplayName[] = L"OrbitLan Network Service";
inline constexpr wchar_t kUiWindowClass[] = L"OrbitLan.Native.Window.v1";
inline constexpr wchar_t kProductName[] = L"OrbitLan";
inline constexpr wchar_t kVersion[] = L"1.0.1-native";
inline constexpr unsigned short kEngineApiPort = 9101;

inline constexpr std::string_view kDefaultCoordinator =
    "https://orbitlan.furqan-ahm.workers.dev";

inline std::string EditionFromConnectFields(const std::vector<std::string>& fields) {
    return fields.size() == 6 ? fields[5] : "community";
}

}  // namespace orbitlan

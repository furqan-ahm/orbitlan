#pragma once

#include <string_view>

namespace orbitlan {

inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\OrbitLan.Control.v1)";
inline constexpr wchar_t kServiceName[] = L"OrbitLanService";
inline constexpr wchar_t kServiceDisplayName[] = L"OrbitLan Network Service";
inline constexpr wchar_t kUiWindowClass[] = L"OrbitLan.Native.Window.v1";
inline constexpr wchar_t kProductName[] = L"OrbitLan";
inline constexpr wchar_t kVersion[] = L"2.0.0-native";
inline constexpr unsigned short kEngineApiPort = 9101;

inline constexpr std::string_view kDefaultCoordinator =
    "https://orbitlan.furqan-ahm.workers.dev";

}  // namespace orbitlan

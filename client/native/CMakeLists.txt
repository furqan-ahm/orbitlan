cmake_minimum_required(VERSION 3.24)
project(OrbitLanNative VERSION 1.0.0 LANGUAGES CXX RC)

include(CTest)

option(ORBITLAN_SUPPORTER_EDITION "Build the OrbitLan Supporter edition" OFF)
option(ORBITLAN_UI_PREVIEW "Build a UI-only preview that does not request setup" OFF)

if(NOT WIN32)
  message(FATAL_ERROR "OrbitLan native client is Windows-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(orbitlan_common STATIC common.cpp)
target_include_directories(orbitlan_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(orbitlan_common PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(orbitlan_common PUBLIC crypt32 advapi32 shell32)

add_executable(OrbitLan WIN32 ui_main.cpp ui.rc)
target_link_libraries(OrbitLan PRIVATE orbitlan_common comctl32 msimg32 uxtheme bcrypt dwmapi gdiplus)
target_link_options(OrbitLan PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
if(ORBITLAN_SUPPORTER_EDITION)
  target_compile_definitions(OrbitLan PRIVATE ORBITLAN_SUPPORTER_EDITION=1)
else()
  target_compile_definitions(OrbitLan PRIVATE ORBITLAN_SUPPORTER_EDITION=0)
endif()
if(ORBITLAN_UI_PREVIEW)
  target_compile_definitions(OrbitLan PRIVATE ORBITLAN_UI_PREVIEW=1)
else()
  target_compile_definitions(OrbitLan PRIVATE ORBITLAN_UI_PREVIEW=0)
endif()
add_custom_command(TARGET OrbitLan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OrbitLan>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../ui/assets/earth-clouds.gif"
          "$<TARGET_FILE_DIR:OrbitLan>/assets/earth-clouds.gif"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../ui/assets/earth-header-sheet.png"
          "$<TARGET_FILE_DIR:OrbitLan>/assets/earth-header-sheet.png"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../ui/assets/moon-supporter.png"
          "$<TARGET_FILE_DIR:OrbitLan>/assets/moon-supporter.png")

add_executable(OrbitLanService service_main.cpp service.rc)
target_link_libraries(OrbitLanService PRIVATE orbitlan_common winhttp bcrypt userenv)
target_link_options(OrbitLanService PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")

set(ORBITLAN_ENGINE_PAYLOAD
    "${CMAKE_CURRENT_SOURCE_DIR}/../ui/payload/orbitlan-engine.exe"
    CACHE FILEPATH "Windows x64 OrbitLan engine embedded in the installer")
if(NOT EXISTS "${ORBITLAN_ENGINE_PAYLOAD}")
  message(FATAL_ERROR "Missing Windows engine payload: ${ORBITLAN_ENGINE_PAYLOAD}")
endif()

set(ORBITLAN_UI_PAYLOAD "${CMAKE_BINARY_DIR}/bin/OrbitLan.exe")
set(ORBITLAN_SERVICE_PAYLOAD "${CMAKE_BINARY_DIR}/bin/OrbitLan.NetworkService.exe")
set(ORBITLAN_EARTH_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/assets/earth-clouds.gif")
set(ORBITLAN_HEADER_EARTH_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/assets/earth-header-sheet.png")
set(ORBITLAN_MOON_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/assets/moon-supporter.png")
set(ORBITLAN_DEVCON_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/payload/driver/devcon.exe")
set(ORBITLAN_DRIVER_INF_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/payload/driver/OemVista.inf")
set(ORBITLAN_DRIVER_CAT_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/payload/driver/tap0901.cat")
set(ORBITLAN_DRIVER_SYS_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/payload/driver/tap0901.sys")
set(ORBITLAN_LICENSE_GPL_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/payload/driver/LICENSE-GPL-2.0.txt")
set(ORBITLAN_LICENSE_MS_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/../ui/payload/driver/LICENSE-MS-PL.txt")
configure_file(setup_payload.rc.in "${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc" PROPERTIES
  OBJECT_DEPENDS
    "${ORBITLAN_UI_PAYLOAD};${ORBITLAN_SERVICE_PAYLOAD};${ORBITLAN_ENGINE_PAYLOAD};${ORBITLAN_EARTH_PAYLOAD};${ORBITLAN_HEADER_EARTH_PAYLOAD};${ORBITLAN_MOON_PAYLOAD};${ORBITLAN_DEVCON_PAYLOAD};${ORBITLAN_DRIVER_INF_PAYLOAD};${ORBITLAN_DRIVER_CAT_PAYLOAD};${ORBITLAN_DRIVER_SYS_PAYLOAD};${ORBITLAN_LICENSE_GPL_PAYLOAD};${ORBITLAN_LICENSE_MS_PAYLOAD}")

add_executable(OrbitLanPortableSetup WIN32 setup_main.cpp setup.rc)
target_link_libraries(OrbitLanPortableSetup PRIVATE orbitlan_common ole32 shlwapi setupapi)
target_link_options(OrbitLanPortableSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")

add_executable(OrbitLanInstaller WIN32 setup_main.cpp setup.rc "${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc")
add_dependencies(OrbitLanInstaller OrbitLan OrbitLanService)
target_link_libraries(OrbitLanInstaller PRIVATE orbitlan_common ole32 shlwapi setupapi)
target_link_options(OrbitLanInstaller PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")

if(MSVC)
  foreach(target orbitlan_common OrbitLan OrbitLanService OrbitLanPortableSetup OrbitLanInstaller)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endforeach()
endif()

set_target_properties(OrbitLan OrbitLanService OrbitLanPortableSetup OrbitLanInstaller PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
set_target_properties(OrbitLanService PROPERTIES OUTPUT_NAME "OrbitLan.NetworkService")
set_target_properties(OrbitLanPortableSetup PROPERTIES OUTPUT_NAME "OrbitLan.Setup")
set_target_properties(OrbitLanInstaller PROPERTIES OUTPUT_NAME "OrbitLan-Installer")

if(BUILD_TESTING)
  add_executable(OrbitLanNativeTests native_tests.cpp)
  target_link_libraries(OrbitLanNativeTests PRIVATE orbitlan_common)
  if(MSVC)
    target_compile_options(OrbitLanNativeTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_property(TARGET OrbitLanNativeTests PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME OrbitLanNativeTests COMMAND OrbitLanNativeTests)
endif()

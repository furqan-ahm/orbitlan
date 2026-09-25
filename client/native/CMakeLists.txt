cmake_minimum_required(VERSION 3.24)
project(OrbitLanNative VERSION 2.0.0 LANGUAGES CXX RC)

include(CTest)

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
add_custom_command(TARGET OrbitLan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OrbitLan>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../ui/assets/earth-clouds.gif"
          "$<TARGET_FILE_DIR:OrbitLan>/assets/earth-clouds.gif")

add_executable(OrbitLanService service_main.cpp service.rc)
target_link_libraries(OrbitLanService PRIVATE orbitlan_common winhttp bcrypt userenv)
target_link_options(OrbitLanService PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")

add_executable(OrbitLanSetup WIN32 setup_main.cpp setup.rc)
target_link_libraries(OrbitLanSetup PRIVATE orbitlan_common ole32 shlwapi)
target_link_options(OrbitLanSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")

if(MSVC)
  foreach(target orbitlan_common OrbitLan OrbitLanService OrbitLanSetup)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endforeach()
endif()

set_target_properties(OrbitLan OrbitLanService OrbitLanSetup PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

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

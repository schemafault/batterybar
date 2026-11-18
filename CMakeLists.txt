cmake_minimum_required(VERSION 3.16)
project(BatteryBar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Platform-independent protocol tests: build everywhere.
enable_testing()
add_executable(hidpp_tests tests/hidpp_tests.cpp src/hidpp_proto.cpp)
add_executable(roster_tests tests/roster_tests.cpp src/roster.cpp)
add_executable(appearance_tests tests/appearance_tests.cpp src/appearance.cpp)
foreach(t hidpp_tests roster_tests appearance_tests)
  target_include_directories(${t} PRIVATE src)
  if(MINGW)
    target_link_options(${t} PRIVATE -static)
  endif()
  if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME ${t} COMMAND ${t})
  endif()
endforeach()

# The tray utility needs Windows (native or a MinGW cross toolchain).
if(WIN32)
  enable_language(RC)

  add_executable(BatteryBar WIN32
    src/main.cpp
    src/roster.cpp
    src/appearance.cpp
    src/hidpp_proto.cpp
    src/hidpp.cpp
    src/btbattery.cpp
    src/xinput_battery.cpp
    res/app.rc)
  target_compile_definitions(BatteryBar PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(BatteryBar PRIVATE setupapi hid cfgmgr32 shell32 user32 gdi32 advapi32 ole32 uuid)

  if(MSVC)
    # CMake merges the manifest itself; keep it out of the .rc to avoid a duplicate resource.
    target_sources(BatteryBar PRIVATE res/app.manifest)
    set_source_files_properties(res/app.rc PROPERTIES COMPILE_DEFINITIONS BATTERYBAR_EXTERNAL_MANIFEST)
  else()
    set_source_files_properties(res/app.rc PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/res")
    target_compile_options(BatteryBar PRIVATE -Os)
    target_link_options(BatteryBar PRIVATE -static -municode -s)
  endif()
endif()

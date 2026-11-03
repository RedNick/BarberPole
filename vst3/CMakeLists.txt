# Barber Pole as a VST3 plugin (for DAWs such as FL Studio and Reaper).
#
# A separate CMake project from the VirtualDJ plugin, so it builds without the VirtualDJ SDK:
#
#   cmake -S vst3 -B build-vst3                       (add -A x64 on Windows)
#   cmake --build build-vst3 --config Release         -> build-vst3/VST3/Release/BarberPole.vst3
#
# The Steinberg VST3 SDK (MIT licence) is downloaded at configure time. To use a copy you
# already have, pass -DVST3_SDK_DIR=<path to vst3sdk>. Steinberg's validator runs on the
# plugin after every build and fails the build if the plugin breaks the VST3 rules.
cmake_minimum_required(VERSION 3.25)

# Before project(), as the SDK sets its own default otherwise
if(NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Oldest macOS version to support")
endif()

# Keep the version in step with src/BarberPole.cpp (see "CI and releases" in CLAUDE.md)
project(BarberPoleVST3 VERSION 2.5.0 LANGUAGES C CXX)

# --- The VST3 SDK -----------------------------------------------------------------------------
set(VST3_SDK_DIR "" CACHE PATH "An existing vst3sdk checkout (leave empty to download it)")
set(VST3_SDK_TAG "v3.8.1_build_84" CACHE STRING "VST3 SDK release to download")

if(NOT VST3_SDK_DIR)
  include(FetchContent)
  FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG        ${VST3_SDK_TAG}
    GIT_SHALLOW    TRUE
    # only the parts needed for a plugin without a custom editor (skips VSTGUI, docs, tutorials)
    GIT_SUBMODULES base cmake pluginterfaces public.sdk
    # Download only: the SDK is added below with add_subdirectory, once its options are set.
    # (MakeAvailable adds SOURCE_SUBDIR, which doesn't exist, so it adds nothing itself.)
    SOURCE_SUBDIR  download-only)
  FetchContent_MakeAvailable(vst3sdk)
  set(VST3_SDK_DIR "${vst3sdk_SOURCE_DIR}")
endif()

# SDK options: no example plugins and no VSTGUI (hosts draw their own knob panel). The hosting
# examples stay on because they include the validator.
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT       OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_AUV2_BUILDS          OFF CACHE BOOL "" FORCE)
# Don't try to link the plugin into the system VST3 folder (needs admin rights on Windows)
set(SMTG_CREATE_PLUGIN_LINK          OFF CACHE BOOL "" FORCE)
# Static runtime: no VC++ redistributable needed (same as the VirtualDJ plugin)
set(SMTG_USE_STATIC_CRT              ON  CACHE BOOL "" FORCE)

add_subdirectory("${VST3_SDK_DIR}" "${PROJECT_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
smtg_enable_vst3_sdk()

# --- The plugin -------------------------------------------------------------------------------
set(BARBERPOLE_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

smtg_add_vst3plugin(BarberPole
  BarberPoleIds.h
  BarberPoleState.h
  BarberPoleProcessor.h
  BarberPoleProcessor.cpp
  BarberPoleController.h
  BarberPoleController.cpp
  BarberPoleEntry.cpp
  "${BARBERPOLE_DSP_DIR}/barberpole_dsp.h")

# The same DSP header as the VirtualDJ plugin, so both sound identical
target_include_directories(BarberPole PRIVATE "${BARBERPOLE_DSP_DIR}")
target_compile_features(BarberPole PRIVATE cxx_std_17)
target_link_libraries(BarberPole PRIVATE sdk)
target_compile_definitions(BarberPole PRIVATE BARBERPOLE_VST3_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_definitions(BarberPole PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

if(SMTG_MAC)
  smtg_target_set_bundle(BarberPole
    BUNDLE_IDENTIFIER "io.github.rednick.barberpole.vst3"
    COMPANY_NAME "Barber Pole project")
endif()

# --- Test: the VST3 processor must sound exactly like the DSP core ----------------------------
# Run it with: cmake --build build-vst3 --config Release --target compare_with_dsp
# then build-vst3/bin/Release/compare_with_dsp (Windows: ...\compare_with_dsp.exe)
add_executable(compare_with_dsp
  test/compare_with_dsp.cpp
  BarberPoleProcessor.cpp
  "${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp")
target_include_directories(compare_with_dsp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${BARBERPOLE_DSP_DIR}")
target_compile_features(compare_with_dsp PRIVATE cxx_std_17)
target_link_libraries(compare_with_dsp PRIVATE sdk sdk_hosting)
target_compile_definitions(compare_with_dsp PRIVATE BARBERPOLE_VST3_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_definitions(compare_with_dsp PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
if(APPLE)
  # the SDK's string and UID code uses CoreFoundation on macOS
  target_link_libraries(compare_with_dsp PRIVATE "-framework CoreFoundation")
endif()

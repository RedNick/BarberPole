cmake_minimum_required(VERSION 3.15)
project(BarberPole CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Put vdjPlugin8.h and vdjDsp8.h from the official VirtualDJ SDK into ./SDK
# (download: https://virtualdj.com/developers/VirtualDJ8_SDK_20211003.zip)
set(VDJ_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDK" CACHE PATH "Folder containing vdjPlugin8.h and vdjDsp8.h")
if(NOT EXISTS "${VDJ_SDK_DIR}/vdjPlugin8.h" OR NOT EXISTS "${VDJ_SDK_DIR}/vdjDsp8.h")
  message(FATAL_ERROR "VirtualDJ SDK headers not found in ${VDJ_SDK_DIR}. "
                      "Copy vdjPlugin8.h and vdjDsp8.h there (see README.md).")
endif()

set(SOURCES src/BarberPole.cpp src/BarberPole.h src/barberpole_dsp.h)

if(APPLE)
  add_library(BarberPole MODULE ${SOURCES})
  set_target_properties(BarberPole PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "bundle"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.barberpole.vdjplugin"
    MACOSX_BUNDLE_BUNDLE_NAME "BarberPole"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    CXX_VISIBILITY_PRESET hidden)
  target_link_libraries(BarberPole PRIVATE "-framework CoreFoundation")
elseif(WIN32)
  # the .def file exports DllGetClassObject undecorated (needed for 32-bit builds)
  add_library(BarberPole SHARED ${SOURCES} src/BarberPole.def)
  if(MSVC)
    # static runtime: no VC++ redistributable needed on the DJ laptop
    set_property(TARGET BarberPole PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(BarberPole PRIVATE /O2 /W3)
    target_compile_definitions(BarberPole PRIVATE NOMINMAX)
  endif()
else()
  add_library(BarberPole SHARED ${SOURCES})   # Linux: compile check only
endif()

target_include_directories(BarberPole PRIVATE "${VDJ_SDK_DIR}" src)
if(NOT MSVC)
  target_compile_options(BarberPole PRIVATE -O2)
endif()

# MIDI knob sequencer for the Traktor / VirtualDJ comparison test (Windows only)
if(WIN32)
  add_executable(knob_sequencer tools/knob_sequencer.cpp)
  target_link_libraries(knob_sequencer PRIVATE winmm)
  # compares two recordings of the knob test (decodes WAV/FLAC through Media Foundation)
  add_executable(compare_recordings tools/compare_recordings.cpp)
  target_link_libraries(compare_recordings PRIVATE mfplat mfreadwrite mfuuid ole32)
  if(MSVC)
    set_property(TARGET knob_sequencer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET compare_recordings PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(compare_recordings PRIVATE /O2 /W3)
  endif()
endif()

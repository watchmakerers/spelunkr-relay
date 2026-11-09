# SPELUNKR desktop app (Windows, macOS, Linux), built on SDL2.
#   cmake -S desktop -B build/desktop && cmake --build build/desktop
# -DSPELUNKR_VENDOR_SDL=ON downloads SDL2 and links it statically (self-contained binaries;
# the release builds use this).
cmake_minimum_required(VERSION 3.16)
project(spelunkr C)
set(CMAKE_C_STANDARD 11)
# MSVC: link the C runtime statically too, so spelunkr.exe runs on its own.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SPELUNKR_VENDOR_SDL "Build SDL2 from source and link it statically" OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/client.cmake)

if(SPELUNKR_VENDOR_SDL)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-2.30.9 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL2)
  set(SDL_LIBS SDL2::SDL2main SDL2::SDL2-static)
else()
  find_package(SDL2 REQUIRED)
  set(SDL_LIBS SDL2::SDL2)
  if(TARGET SDL2::SDL2main)
    list(PREPEND SDL_LIBS SDL2::SDL2main)
  endif()
endif()

add_executable(spelunkr WIN32 MACOSX_BUNDLE ${SPELUNKR_SOURCES})
target_include_directories(spelunkr PRIVATE ${SPELUNKR_INCLUDES})
target_link_libraries(spelunkr PRIVATE ${SDL_LIBS})
if(WIN32)
  target_link_libraries(spelunkr PRIVATE ws2_32)
  if(MINGW)
    target_link_options(spelunkr PRIVATE -static -static-libgcc)
  endif()
elseif(NOT APPLE)
  target_link_libraries(spelunkr PRIVATE m)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(spelunkr PRIVATE -Wall -Wno-unused-function)
elseif(MSVC)
  target_compile_definitions(spelunkr PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
set_target_properties(spelunkr PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME "SPELUNKR"
  MACOSX_BUNDLE_GUI_IDENTIFIER "tv.spelunkr.client"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")

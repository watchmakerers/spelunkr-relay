# SPELUNKR for PS Vita (VitaSDK + its SDL2 port). Produces spelunkr.vpk.
#   cmake -S vita -B build/vita && cmake --build build/vita     (with $VITASDK set)
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()
project(spelunkr C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "SPELUNKR")
set(VITA_TITLEID  "SPLK00001")
set(VITA_VERSION  "01.00")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wno-unused-function")

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/client.cmake)
find_package(SDL2 REQUIRED)

add_executable(spelunkr ${SPELUNKR_SOURCES})
target_include_directories(spelunkr PRIVATE ${SPELUNKR_INCLUDES})
# SDL2's Vita renderer sits on vitaGL (C++), which needs these too.
target_link_libraries(spelunkr
  SDL2::SDL2-static
  vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub
  SceCtrl_stub SceNet_stub SceNetCtl_stub SceSysmodule_stub ScePower_stub SceAppMgr_stub
  stdc++ m)
set_target_properties(spelunkr PROPERTIES LINKER_LANGUAGE CXX)

vita_create_self(eboot.bin spelunkr)
vita_create_vpk(spelunkr.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)

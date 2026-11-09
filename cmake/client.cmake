# Shared by desktop/ and vita/: the client sources and the embedded font.
set(SPELUNKR_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
file(GLOB SPELUNKR_CORE_SOURCES ${SPELUNKR_ROOT}/core/*.c)
set(SPELUNKR_SOURCES ${SPELUNKR_CORE_SOURCES} ${SPELUNKR_ROOT}/sdl/platform_sdl.c)
set(SPELUNKR_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(SPELUNKR_FONT ${SPELUNKR_ROOT}/core/font/spacemono_bold.ttf)
add_custom_command(
  OUTPUT ${SPELUNKR_GEN}/spacemono_bold_ttf.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SPELUNKR_GEN}
  COMMAND ${CMAKE_COMMAND} -DIN=${SPELUNKR_FONT} -DOUT=${SPELUNKR_GEN}/spacemono_bold_ttf.h -DNAME=spacemono_bold_ttf
          -P ${SPELUNKR_ROOT}/cmake/embed.cmake
  DEPENDS ${SPELUNKR_FONT} ${SPELUNKR_ROOT}/cmake/embed.cmake)
list(APPEND SPELUNKR_SOURCES ${SPELUNKR_GEN}/spacemono_bold_ttf.h)
set(SPELUNKR_INCLUDES ${SPELUNKR_ROOT}/core ${SPELUNKR_GEN})

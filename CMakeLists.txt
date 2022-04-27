cmake_minimum_required(VERSION 3.10)

project(cutty)

#
# project settings
#

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)

#
# compiler flags
#

include(CheckCXXCompilerFlag)
include(CheckSymbolExists)

if(MSVC)
  add_definitions(/MP)
  add_definitions(/D_SECURE_SCL=0 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS)
endif()

check_cxx_compiler_flag("-pg" has_gprof "int main() { return 0; }")
if (CMAKE_PROFILE AND has_gprof)
  add_compile_options(-pg)
endif()

check_cxx_compiler_flag("-fno-omit-frame-pointer" has_no_omit_fp "int main() { return 0; }")
if (has_no_omit_fp)
  add_compile_options(-fno-omit-frame-pointer)
endif()

#
# system libraries
#

find_package(Threads REQUIRED)
list(APPEND GLYB_LIBS Threads::Threads)

#
# bundled libraries
#

## glad
add_subdirectory(third_party/glad)
include_directories(${CMAKE_BINARY_DIR}/third_party/glad/include)

## glfw
add_subdirectory(third_party/glfw)
include_directories(third_party/glfw/include)

# brotli
set(BROTLI_BUNDLED_MODE OFF CACHE BOOL "")
add_subdirectory(third_party/brotli)
include_directories(third_party/brotli/c/include)
set_property(TARGET brotlidec-static PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET brotlicommon-static PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
install(TARGETS brotlidec-static brotlicommon-static EXPORT brotli-targets DESTINATION "${INSTALL_BIN_DIR}")
install(EXPORT brotli-targets DESTINATION lib)

## bzip2
set(ENABLE_LIB_ONLY ON CACHE BOOL "")
set(ENABLE_SHARED_LIB OFF CACHE BOOL "")
set(ENABLE_STATIC_LIB ON CACHE BOOL "")
add_subdirectory(third_party/bzip2)
include_directories(third_party/bzip2)

## zlib
add_subdirectory(third_party/zlib)
include_directories(third_party/zlib)
include_directories(${CMAKE_BINARY_DIR}/third_party/zlib)
install(TARGETS zlib zlibstatic EXPORT zlib-targets DESTINATION "${INSTALL_BIN_DIR}")
install(EXPORT zlib-targets DESTINATION lib)

## png
add_subdirectory(third_party/libpng)
include_directories(third_party/libpng)
include_directories(${CMAKE_BINARY_DIR}/third_party/libpng)

## freetype2
include_directories(third_party/harfbuzz/src)
add_subdirectory(third_party/freetype2)
add_definitions( -DFT_CONFIG_CONFIG_H=\"ftconfig.h\" )
add_definitions( -DFT_CONFIG_MODULES_H=\"ftmodule.h\" )
include_directories(third_party/freetype2/include)
include_directories(third_party/freetype2/include/freetype/config)

## harfbuzz
set(HB_HAVE_FREETYPE ON CACHE BOOL "Enable FreeType Integration" FORCE)
add_subdirectory(third_party/harfbuzz)

## imgui
set(IMGUI_SOURCES
  third_party/imgui/backends/imgui_impl_glfw.cpp
  third_party/imgui/backends/imgui_impl_opengl3.cpp
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_demo.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp)
include_directories(third_party/imgui)
include_directories(third_party/imgui/backends)
add_definitions(-DIMGUI_IMPL_OPENGL_LOADER_GLAD)
add_library(imgui STATIC ${IMGUI_SOURCES})
add_dependencies(imgui glad-generate-files)

## msdfgen
add_subdirectory(third_party/msdfgen)
include_directories(third_party/msdfgen)

## glm
include_directories(third_party/glm)

#
# libglyb
#

include_directories(lib)
include_directories(examples)
file(GLOB GLYB_SOURCES lib/*.cc lib/*.h)
add_library(glyb STATIC ${GLYB_SOURCES})
add_dependencies(glyb glad-generate-files)
set_target_properties(glyb PROPERTIES CXX_STANDARD 17)
list(APPEND GLYB_LIBS
  ${PNG_LIBRARY}
  ${BROTLIDEC_LIBRARY}
  ${BZIP_LIBRARY}
  ${ZLIB_LIBRARY}
  ${HARFBUZZ_LIBRARY}
  ${FREETYPE_LIBRARY}
  glyb
  lib_msdfgen)

#
# Test programs
#

file(GLOB TEST_ALL_SOURCES RELATIVE ${CMAKE_SOURCE_DIR} "tests/*.cc")
foreach(TEST_SOURCE ${TEST_ALL_SOURCES})
  string(REGEX REPLACE "^.*/([^/]*)\\.[^.]*$" "\\1" TEST_NAME ${TEST_SOURCE})
  add_executable(${TEST_NAME} ${TEST_SOURCE})
  target_link_libraries(${TEST_NAME} ${GLYB_LIBS} ${CMAKE_DL_LIBS})
endforeach()

#
# CLI programs
#

foreach(prog IN ITEMS genatlas)
  add_executable(${prog} util/${prog}.cc)
  target_link_libraries(${prog} ${GLYB_LIBS} ${CMAKE_DL_LIBS})
endforeach(prog)

#
# Terminal
#

list(APPEND CMAKE_REQUIRED_LIBRARIES util)
check_symbol_exists(forkpty libutil.h have_forkpty_libutil_h)
check_symbol_exists(forkpty pty.h have_forkpty_pty_h)
if (have_forkpty_libutil_h OR have_forkpty_pty_h)
  list(APPEND UTIL_LIBS util)
endif()

set(cutty_sources
    app/cellgrid.cc
    app/colors.cc
    app/process.cc
    app/render.cc
    app/terminal.cc
    app/typeface.cc)

include_directories(app)
add_executable(terminal app/app.cc ${cutty_sources})
target_link_libraries(terminal ${GLYB_LIBS} ${CMAKE_DL_LIBS} ${GLAD_LIBRARIES} ${UTIL_LIBS} glfw)
target_compile_definitions(terminal PRIVATE -DHAVE_GLAD)
if(WIN32)
  set_property(TARGET terminal PROPERTY WIN32_EXECUTABLE TRUE)
endif()

#
# screen capture and OCR tests using osmesa and tesseract OCR
#

# FindOSMesa does not find the library on systems that do not use pkg-config.
# OSMesa can be built using this script: https://github.com/devernay/osmesa-install

set (OSMESA_DIR /opt/osmesa)
set (LLVM_DIR /opt/llvm)

# try to find OSMesa using pkg-config
find_package(PkgConfig)
pkg_check_modules(OSMESA osmesa)

# otherwise try to find OSMesa headers and library ourselves
if (NOT OSMESA_FOUND)
  find_path(OSMESA_INCLUDE GL/osmesa.h PATHS ${OSMESA_DIR}/include NO_CMAKE_SYSTEM_PATH NO_CACHE)
  find_library(OSMESA_LIBRARY OSMesa32 PATHS ${OSMESA_DIR}/lib NO_CMAKE_SYSTEM_PATH NO_CACHE)
  set(OSMESA_LDFLAGS -L${OSMESA_DIR}/lib)

  # find LLVM library using llvm-config, as osmesa-install mesa depends on LLVM
  find_program(LLVM_CONFIG NAMES llvm-config REQUIRED PATHS /usr/bin /opt/llvm/bin)
  exec_program(${LLVM_CONFIG} ARGS --ldflags OUTPUT_VARIABLE LLVM_LDFLAGS)
  exec_program(${LLVM_CONFIG} ARGS --libs OUTPUT_VARIABLE LLVM_LIBS)
  separate_arguments(LLVM_LDFLAGS UNIX_COMMAND "${LLVM_LDFLAGS}")
  separate_arguments(LLVM_LIBS UNIX_COMMAND "${LLVM_LIBS}")

  # put the results together
  if (OSMESA_LIBRARY AND LLVM_LIBS)
    set(OSMESA_LDFLAGS ${OSMESA_LDFLAGS} ${LLVM_LDFLAGS})
    set(OSMESA_LIBS ${OSMESA_LIBRARY} ${LLVM_LIBS} ncurses)
  endif()
endif()


# find tesseract
find_program(TESSERACT_BINARY tesseract)

# build capture test program
if (OSMESA_LDFLAGS AND TESSERACT_BINARY)
add_executable(capture util/capture.cc ${cutty_sources})
target_include_directories(capture PUBLIC ${OSMESA_INCLUDE})
target_compile_definitions(capture PUBLIC USE_OSMESA=1)
target_link_libraries(capture ${GLYB_LIBS} ${CMAKE_DL_LIBS} ${UTIL_LIBS} ${OSMESA_LDFLAGS} ${OSMESA_LIBS})
endif()

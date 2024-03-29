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

option(TTY_ENABLE_ASAN "Enable ASAN" OFF)
option(TTY_ENABLE_MSAN "Enable MSAN" OFF)
option(TTY_ENABLE_TSAN "Enable TSAN" OFF)
option(TTY_ENABLE_UBSAN "Enable UBSAN" OFF)

macro(add_compiler_flag)
   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ARGN}")
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ARGN}")
endmacro(add_compiler_flag)

macro(add_linker_flag)
  set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} ${ARGN}")
endmacro(add_linker_flag)

if (TTY_ENABLE_ASAN)
  add_compiler_flag(-fsanitize=address)
  add_linker_flag(-fsanitize=address)
endif()

if(TTY_ENABLE_MSAN)
  add_compiler_flag(-fsanitize=memory)
  add_linker_flag(-fsanitize=memory)
endif()

if(TTY_ENABLE_TSAN)
  add_compiler_flag("-fPIE -fsanitize=thread")
  add_linker_flag("-fPIE -fsanitize=thread")
endif()

if(TTY_ENABLE_UBSAN)
  add_compiler_flag(-fsanitize=undefined)
  add_linker_flag(-fsanitize=undefined)
endif()

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
    app/teletype.cc
    app/timestamp.cc
    app/translate.cc
    app/typeface.cc)

include_directories(app)
if(APPLE)
  file(GLOB_RECURSE font_bundle
    Resources/fonts/NotoColorEmoji.ttf
    Resources/fonts/NotoSansMono-Regular.ttf
    Resources/fonts/NotoSansMono-Bold.ttf
    Resources/fonts/NotoSansMono_ExtraCondensed-Regular.ttf
    Resources/fonts/NotoSansMono_ExtraCondensed-Bold.ttf)
  file(GLOB_RECURSE shader_bundle
    Resources/shaders/*.fsh
    Resources/shaders/*.vsh
    Resources/shaders/*.properties)
  file(GLOB_RECURSE settings_bundle
    Resources/settings/*.map)
  set(cutty_bundle ${shader_bundle} ${settings_bundle})
  foreach(font_file ${font_bundle})
    list(APPEND cutty_bundle ${font_file})
  endforeach()
  foreach(rsrc_file ${cutty_bundle})
        get_filename_component(rsrc_file_path ${rsrc_file} PATH)
        file(RELATIVE_PATH rsrc_file_dir ${CMAKE_CURRENT_SOURCE_DIR} ${rsrc_file_path})
        set_source_files_properties(${rsrc_file} PROPERTIES MACOSX_PACKAGE_LOCATION ${rsrc_file_dir})
  endforeach(rsrc_file)
  set(cutty_icon Resources/images/cutty.icns)
  set(MACOSX_BUNDLE_ICON_FILE images/cutty.icns)
  set_source_files_properties(${cutty_icon} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/images)
  add_executable(cutty MACOSX_BUNDLE app/app.cc ${cutty_sources} ${cutty_bundle} ${cutty_icon})

  set_target_properties(cutty PROPERTIES
    BUNDLE True
    RESOURCE "${cutty_resources}"
    MACOSX_BUNDLE_GUI_IDENTIFIER org.cutty
    MACOSX_BUNDLE_BUNDLE_NAME cutty
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    MACOSX_BUNDLE_COPYRIGHT "Copyright © 2022 Cutty contributors. All rights reserved."
  )
else()
  add_executable(cutty app/app.cc ${cutty_sources})
endif()
target_link_libraries(cutty ${GLYB_LIBS} ${CMAKE_DL_LIBS} ${GLAD_LIBRARIES} ${UTIL_LIBS} glfw)
target_compile_definitions(cutty PRIVATE -DHAVE_GLAD)
if(WIN32)
  set_property(TARGET cutty PROPERTY WIN32_EXECUTABLE TRUE)
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
add_executable(capture app/capture.cc ${cutty_sources})
target_include_directories(capture PUBLIC ${OSMESA_INCLUDE})
target_compile_definitions(capture PUBLIC USE_OSMESA=1)
target_link_libraries(capture ${GLYB_LIBS} ${CMAKE_DL_LIBS} ${UTIL_LIBS} ${OSMESA_LDFLAGS} ${OSMESA_LIBS})
endif()

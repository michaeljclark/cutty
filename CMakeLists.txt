cmake_minimum_required(VERSION 3.10)

project(cuterm)

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

set(GLAD_LIBRARIES ${GLAD_LIBRARIES} PARENT_SCOPE)
set(GLYB_LIBS ${GLYB_LIBS} PARENT_SCOPE)

#
# Test programs
#

foreach(test IN ITEMS t0 t1 t2 t3 t4 t5 t6 t7 t8)
  add_executable(${test} tests/${test}.cc)
  target_link_libraries(${test} ${GLYB_LIBS} ${CMAKE_DL_LIBS})
endforeach(test)

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

set(cuterm_sources
    app/cellgrid.cc
    app/colors.cc
    app/process.cc
    app/render.cc
    app/terminal.cc
    app/typeface.cc)

include_directories(app)
add_executable(cuterm app/app.cc ${cuterm_sources})
target_link_libraries(cuterm ${GLYB_LIBS} ${CMAKE_DL_LIBS} ${GLAD_LIBRARIES} ${UTIL_LIBS} glfw)
target_compile_definitions(cuterm PRIVATE -DHAVE_GLAD)
if(WIN32)
  set_property(TARGET cuterm PROPERTY WIN32_EXECUTABLE TRUE)
endif()

#
# screen capture and OCR tests using osmesa and tesseract OCR
#

# FindOSMesa does not find the library on systems that do not use pkg-config.
# OSMesa can be built using this script: https://github.com/devernay/osmesa-install

set (OSMESA_DIR /opt/osmesa)
set (LLVM_DIR /opt/llvm)

find_path(OSMESA_INCLUDE GL/osmesa.h PATHS ${OSMESA_DIR}/include NO_CMAKE_SYSTEM_PATH NO_CACHE)
find_library(OSMESA_LIBRARY OSMesa32 PATHS ${OSMESA_DIR}/lib NO_CMAKE_SYSTEM_PATH NO_CACHE)
find_path(LLVM_INCLUDE llvm/Pass.h PATHS ${LLVM_DIR}/include NO_CMAKE_SYSTEM_PATH NO_CACHE)
find_library(LLVM_LIBRARY LLVM PATHS ${LLVM_DIR}/lib NO_CMAKE_SYSTEM_PATH NO_CACHE)
find_program(TESSERACT_BINARY tesseract)

if (OSMESA_LIBRARY AND LLVM_LIBRARY AND TESSERACT_BINARY)
add_executable(capture util/capture.cc ${cuterm_sources})
target_include_directories(capture PUBLIC ${OSMESA_DIR}/include)
target_compile_definitions(capture PUBLIC USE_OSMESA=1)
target_link_options(capture PUBLIC -L${OSMESA_DIR}/lib -L${LLVM_DIR}/lib)
target_link_libraries(capture ${GLYB_LIBS} ${CMAKE_DL_LIBS} ${UTIL_LIBS} OSMesa32 LLVM)
endif()

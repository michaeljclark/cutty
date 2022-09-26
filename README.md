# cutty

cutty is an OpenGL terminal emulator using FreeType and HarfBuzz.

![cutty](/images/cutty-screenshot.png)

## Introduction

The cutty terminal emulator is a simple, maintainable, cross-platform
and standards compliant terminal emulator written in portable C++17.
The cutty terminal emulator is a work in progress and is not feature
complete nevertheless it provides a fast and competent basis to explore
evolution of the DEC/VT/ANSI/xterm protocols.

The cutty terminal emulator currently supports the following features:

- [X] Anti-aliased truetype rendering for UTF-8 text with emoji.
- [X] Xterm 16, 256, and 16M colors, bold and underlined styled text.
- [X] Compressed history with scrollbars and wheel mouse support.
- [X] Flexible input translation layer for adding key bindings.
- [X] Additional columns for line numbers and time stamps.

The terminal emulator supports standalone builds on macOS and Linux.
All dependencies required to build cutty besides OpenGL are included.
Included dependencies are statically linked and the GLAD OpenGL and
GLFW window system loaders are used to minimize runtime dependencies.

The cutty terminal includes these submodules:
[FreeType](https://github.com/freetype/freetype.git)
[†](https://www.freetype.org/),
[HarfBuzz](https://github.com/harfbuzz/harfbuzz)
[†](https://harfbuzz.github.io/),
[msdfgen](https://github.com/Chlumsky/msdfgen)
[†](https://github.com/Chlumsky/msdfgen/files/3050967/thesis.pdf),
[zlib](https://github.com/madler/zlib.git)
[†](http://zlib.net/),
[brotli](https://github.com/google/brotli.git)
[†](https://en.wikipedia.org/wiki/Brotli),
[bzip2](https://gitlab.com/federicomenaquintero/bzip2.git)
[†](https://www.sourceware.org/bzip2/),
[libpng](https://github.com/glennrp/libpng)
[†](http://www.libpng.org/pub/png/libpng.html).

## Building

The cutty terminal emulator uses cmake to build. All runtime dependencies
besides OpenGL are included as submodules. tesseract OCR and osmesa are
required to run the tests. It is recommended to build cutty using Ninja:

```
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -- --verbose
```

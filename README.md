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

## Internals

This section describes the internal representation of the virtual line
buffer used to store the terminal history.

### Source organisation

The terminal source code is organized as follows:

- `app` - _window creation and event handling_
- `capture` - _capture harness for running offscreen tests_
- `cellgrid` - _layout of the virtual buffer to a physical buffer_
- `process` - _creation of shell process attached to pseudo typewritter_
- `render` - _generating batches and issues rendering commands_
- `teletype` - _implementation of terminal protocol on a virtual buffer_
- `translate` - _translating keyboard mappings to terminal protocol_
- `typeface` - _font loading, measurement and metrics_

### Buffer coordinates

The terminal internally uses three different types of coordinates:

- _logical {line,offset}-addressing_
  - (0,0) is the first character in the logical history
- _physical {row,column}-addressing_
  - (0,0) is the first character in the physical layout
- _protocol {row,column}-addressing_
  - (1,1) is the top left cell of the visble portion

The following diagram shows the relationship between logical, physical,
and protocol coordinates.

![coordinates](/images/cutty-coordinates.png)

The virtual line buffer uses _logical {line,offset}-addressing_ internally
to record text selection ranges and to store the position of the cursor.
Logical addresses are translated to _physical {row,column}-addressing_
to map the buffer to line wrapped physical coordinates to support precise
scrolling of the buffer. Protocol addresses are the screen coordinates used
in escape codes to address the visible portion of the terminal viewport.

Forward and reverse indexes are maintained that map from logical buffer
addresses to physical buffer addresses. Index are incrementally recomputed
when the viewport size changes. If the buffer is edited, care must be taken
to recompute the cursor position based on the current window dimensions and
the location of line feeds.

#### list of internal functions returning buffer coordinates

| function          | basis      | description                          |
|:------------------|:-----------|:-------------------------------------|
| `cursor_line()`   | _logical_  | line the cursor is at currently      |
| `cursor_offset()` | _logical_  | offset the cursor is at currently    |
| `total_rows()`    | _physical_ | total number of rows after wrap      |
| `total_cols()`    | _physical_ | total number of columns after wrap   |
| `top_row()`       | _physical_ | row number of the topmost line       |
| `cursor_row()`    | _physical_ | row number of the cursor line        |
| `visible_rows()`  | _protocol_ | window size in rows (`ws_rows`)      |
| `visible_cols()`  | _protocol_ | window size in cols (`ws_cols`)      |
| `scroll_top()`    | _protocol_ | scroll region top row                |
| `scroll_bottom()` | _protocol_ | scroll region bottom row             |

# cuterm

cuterm is an OpenGL terminal emulator using FreeType and HarfBuzz.

## Building

The cuterm terminal emulator uses cmake to build. All runtime dependencies
besides OpenGL are included as submodules.

```
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -- --verbose
```

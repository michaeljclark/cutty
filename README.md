# cutty

cutty is an OpenGL terminal emulator using FreeType and HarfBuzz.

![cutty](/images/cutty-screenshot.png)

## Building

The cutty terminal emulator uses cmake to build. All runtime dependencies
besides OpenGL are included as submodules. tesseract OCR and osmesa are
required to run the tests. It is recommended to build cutty using Ninja:

```
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -- --verbose
```

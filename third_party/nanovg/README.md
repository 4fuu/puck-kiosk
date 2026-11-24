# NanoVG

Vendored from https://github.com/memononen/nanovg at commit `ce3bf74`.

NanoVG is a small antialiased 2D vector drawing library on top of OpenGL. The
Linux and DRM builds use `nanovg_gles3.c`, which selects the upstream
`NANOVG_GLES3_IMPLEMENTATION` so it runs on the same OpenGL ES 3.0 context as
the Puck renderer. The Windows build uses `nanovg_gl3.c`, which selects the
upstream desktop `NANOVG_GL3_IMPLEMENTATION` on an OpenGL 3.3 core context.

`nanovg_gl.h` and `nanovg.c` are unmodified upstream sources. The selection
files only define the implementation macro:

- `nanovg_gles3.c` selects the GLES3 implementation.
- `nanovg_gl3.c` selects the GL3 implementation.
- `nanovg_gl3_impl.h` maps the OpenGL 3.3 entry points that Windows'
  `opengl32.dll` does not export onto the loader table, then includes
  `nanovg_gl.h`.
- `nanovg_gl3_functions.h` declares that loader table.
- `nanovg_gl3_api.h` is the consumer header; it defines `NANOVG_GL3` so
  `nvgCreateGL3` is declared without compiling the implementation.

The loader table is implemented by `src/platform/win32/gl_loader.cpp`, which
resolves the missing functions with `wglGetProcAddress`.

License: MIT (see `LICENSE`).

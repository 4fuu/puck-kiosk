# NanoVG

Vendored from https://github.com/memononen/nanovg at commit `ce3bf74`.

NanoVG is a small antialiased 2D vector drawing library on top of OpenGL. It is
used here as the M0 2D canvas backend, with `NANOVG_GLES3_IMPLEMENTATION` so it
runs on the same OpenGL ES 3.0 context as the Puck renderer.

Files are unmodified upstream sources except for `nanovg_gles3.c`, which only
selects the GLES3 implementation.

License: MIT (see `LICENSE`).

// Desktop OpenGL 3.3 implementation of NanoVG, used by the Windows backend.
// The upstream implementation is nanovg_gl.h; this file only selects it and
// provides the Windows entry-point mapping.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "nanovg_gl3_impl.h"

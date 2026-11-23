#pragma once

// Consumer-side declarations for the GLES3 backend implemented in
// nanovg_gles3.c. Upstream only exposes nvgCreateGLES3 when the implementation
// macro is defined, so we select the declarations without the implementation.
#include <GLES3/gl3.h>

#include "nanovg.h"

#define NANOVG_GLES3 1
#include "nanovg_gl.h"

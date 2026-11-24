#pragma once

// Consumer-side declarations for the desktop GL3 backend implemented in
// nanovg_gl3.c. Upstream only exposes nvgCreateGL3 when NANOVG_GL3 is defined,
// so we select the declarations without the implementation.
#include <GL/gl.h>

#include "nanovg.h"

#define NANOVG_GL3 1
#include "nanovg_gl.h"

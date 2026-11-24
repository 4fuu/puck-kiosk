#pragma once

// OpenGL 3.3 entry points that Windows' opengl32.dll does not export.
//
// opengl32.dll only exports OpenGL 1.1. NanoVG's GL3 backend needs buffers,
// vertex arrays, shader objects, and uniform blocks, so the Windows platform
// loads those through wglGetProcAddress and the GL3 implementation calls them
// through this table. The names are stable because nanovg_gl3_impl.h maps the
// plain gl* identifiers onto the table members.
#include <GL/gl.h>
#include <GL/glext.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NVGgl3Functions {
    PFNGLACTIVETEXTUREPROC ActiveTexture;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBINDBUFFERRANGEPROC BindBufferRange;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLBLENDFUNCSEPARATEPROC BlendFuncSeparate;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLGENERATEMIPMAPPROC GenerateMipmap;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETUNIFORMBLOCKINDEXPROC GetUniformBlockIndex;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLSTENCILOPSEPARATEPROC StencilOpSeparate;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUNIFORM2FVPROC Uniform2fv;
    PFNGLUNIFORM4FVPROC Uniform4fv;
    PFNGLUNIFORMBLOCKBINDINGPROC UniformBlockBinding;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
} NVGgl3Functions;

extern NVGgl3Functions nvg_gl3;

// Resolves every entry point against the current WGL context. Returns 0 and
// names the first missing function in `missing` on failure.
int nvg_gl3_load_functions(const char** missing);

#ifdef __cplusplus
}
#endif

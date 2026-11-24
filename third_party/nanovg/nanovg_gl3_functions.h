#pragma once

// OpenGL 3.3 entry points that Windows' opengl32.dll does not export.
//
// opengl32.dll only exports OpenGL 1.1. NanoVG's GL3 backend and Puck3D need
// buffers, vertex arrays, shader objects, framebuffer objects, and uniforms,
// so the Windows platform loads those through wglGetProcAddress and both
// callers reach them through this table. The names are stable because
// nanovg_gl3_impl.h and gl3_api.h map the plain gl* identifiers onto the table
// members.
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
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLBLENDFUNCSEPARATEPROC BlendFuncSeparate;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLGENERATEMIPMAPPROC GenerateMipmap;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETUNIFORMBLOCKINDEXPROC GetUniformBlockIndex;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLSTENCILOPSEPARATEPROC StencilOpSeparate;
    PFNGLUNIFORM1FPROC Uniform1f;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUNIFORM2FPROC Uniform2f;
    PFNGLUNIFORM2FVPROC Uniform2fv;
    PFNGLUNIFORM3FPROC Uniform3f;
    PFNGLUNIFORM4FPROC Uniform4f;
    PFNGLUNIFORM4FVPROC Uniform4fv;
    PFNGLUNIFORMBLOCKBINDINGPROC UniformBlockBinding;
    PFNGLUNIFORMMATRIX3FVPROC UniformMatrix3fv;
    PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv;
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

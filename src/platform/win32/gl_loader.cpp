#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nanovg_gl3_functions.h"

NVGgl3Functions nvg_gl3 = {};

namespace {

void* resolve(const char* name) {
    PROC address = wglGetProcAddress(name);
    if (address != nullptr && address != reinterpret_cast<PROC>(1) &&
        address != reinterpret_cast<PROC>(2) && address != reinterpret_cast<PROC>(3) &&
        address != reinterpret_cast<PROC>(-1)) {
        return reinterpret_cast<void*>(address);
    }
    HMODULE module = GetModuleHandleA("opengl32.dll");
    if (!module) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}

}  // namespace

extern "C" int nvg_gl3_load_functions(const char** missing) {
#define PK_LOAD_GL3(field, type, symbol)                                  \
    do {                                                                  \
        nvg_gl3.field = reinterpret_cast<type>(resolve(symbol));          \
        if (!nvg_gl3.field) {                                             \
            if (missing) {                                                \
                *missing = symbol;                                        \
            }                                                             \
            return 0;                                                     \
        }                                                                 \
    } while (0)

    PK_LOAD_GL3(ActiveTexture, PFNGLACTIVETEXTUREPROC, "glActiveTexture");
    PK_LOAD_GL3(AttachShader, PFNGLATTACHSHADERPROC, "glAttachShader");
    PK_LOAD_GL3(BindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC, "glBindAttribLocation");
    PK_LOAD_GL3(BindBuffer, PFNGLBINDBUFFERPROC, "glBindBuffer");
    PK_LOAD_GL3(BindBufferRange, PFNGLBINDBUFFERRANGEPROC, "glBindBufferRange");
    PK_LOAD_GL3(BindVertexArray, PFNGLBINDVERTEXARRAYPROC, "glBindVertexArray");
    PK_LOAD_GL3(BlendFuncSeparate, PFNGLBLENDFUNCSEPARATEPROC, "glBlendFuncSeparate");
    PK_LOAD_GL3(BufferData, PFNGLBUFFERDATAPROC, "glBufferData");
    PK_LOAD_GL3(CompileShader, PFNGLCOMPILESHADERPROC, "glCompileShader");
    PK_LOAD_GL3(CreateProgram, PFNGLCREATEPROGRAMPROC, "glCreateProgram");
    PK_LOAD_GL3(CreateShader, PFNGLCREATESHADERPROC, "glCreateShader");
    PK_LOAD_GL3(DeleteBuffers, PFNGLDELETEBUFFERSPROC, "glDeleteBuffers");
    PK_LOAD_GL3(DeleteProgram, PFNGLDELETEPROGRAMPROC, "glDeleteProgram");
    PK_LOAD_GL3(DeleteShader, PFNGLDELETESHADERPROC, "glDeleteShader");
    PK_LOAD_GL3(DeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC, "glDeleteVertexArrays");
    PK_LOAD_GL3(DisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC,
                "glDisableVertexAttribArray");
    PK_LOAD_GL3(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC,
                "glEnableVertexAttribArray");
    PK_LOAD_GL3(GenBuffers, PFNGLGENBUFFERSPROC, "glGenBuffers");
    PK_LOAD_GL3(GenVertexArrays, PFNGLGENVERTEXARRAYSPROC, "glGenVertexArrays");
    PK_LOAD_GL3(GenerateMipmap, PFNGLGENERATEMIPMAPPROC, "glGenerateMipmap");
    PK_LOAD_GL3(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC, "glGetProgramInfoLog");
    PK_LOAD_GL3(GetProgramiv, PFNGLGETPROGRAMIVPROC, "glGetProgramiv");
    PK_LOAD_GL3(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC, "glGetShaderInfoLog");
    PK_LOAD_GL3(GetShaderiv, PFNGLGETSHADERIVPROC, "glGetShaderiv");
    PK_LOAD_GL3(GetUniformBlockIndex, PFNGLGETUNIFORMBLOCKINDEXPROC, "glGetUniformBlockIndex");
    PK_LOAD_GL3(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC, "glGetUniformLocation");
    PK_LOAD_GL3(LinkProgram, PFNGLLINKPROGRAMPROC, "glLinkProgram");
    PK_LOAD_GL3(ShaderSource, PFNGLSHADERSOURCEPROC, "glShaderSource");
    PK_LOAD_GL3(StencilOpSeparate, PFNGLSTENCILOPSEPARATEPROC, "glStencilOpSeparate");
    PK_LOAD_GL3(Uniform1i, PFNGLUNIFORM1IPROC, "glUniform1i");
    PK_LOAD_GL3(Uniform2fv, PFNGLUNIFORM2FVPROC, "glUniform2fv");
    PK_LOAD_GL3(Uniform4fv, PFNGLUNIFORM4FVPROC, "glUniform4fv");
    PK_LOAD_GL3(UniformBlockBinding, PFNGLUNIFORMBLOCKBINDINGPROC, "glUniformBlockBinding");
    PK_LOAD_GL3(UseProgram, PFNGLUSEPROGRAMPROC, "glUseProgram");
    PK_LOAD_GL3(VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC, "glVertexAttribPointer");

#undef PK_LOAD_GL3
    return 1;
}

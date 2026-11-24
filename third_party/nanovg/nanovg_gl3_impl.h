#pragma once

// Implementation-only header for nanovg_gl3.c. It maps the OpenGL 3.3 calls
// NanoVG makes onto the Windows loader table before including the upstream
// implementation, so the rest of the vendored code stays unmodified.
#include <GL/gl.h>

#include "nanovg.h"
#include "nanovg_gl3_functions.h"

#define glActiveTexture nvg_gl3.ActiveTexture
#define glAttachShader nvg_gl3.AttachShader
#define glBindAttribLocation nvg_gl3.BindAttribLocation
#define glBindBuffer nvg_gl3.BindBuffer
#define glBindBufferRange nvg_gl3.BindBufferRange
#define glBindVertexArray nvg_gl3.BindVertexArray
#define glBlendFuncSeparate nvg_gl3.BlendFuncSeparate
#define glBufferData nvg_gl3.BufferData
#define glCompileShader nvg_gl3.CompileShader
#define glCreateProgram nvg_gl3.CreateProgram
#define glCreateShader nvg_gl3.CreateShader
#define glDeleteBuffers nvg_gl3.DeleteBuffers
#define glDeleteProgram nvg_gl3.DeleteProgram
#define glDeleteShader nvg_gl3.DeleteShader
#define glDeleteVertexArrays nvg_gl3.DeleteVertexArrays
#define glDisableVertexAttribArray nvg_gl3.DisableVertexAttribArray
#define glEnableVertexAttribArray nvg_gl3.EnableVertexAttribArray
#define glGenBuffers nvg_gl3.GenBuffers
#define glGenVertexArrays nvg_gl3.GenVertexArrays
#define glGenerateMipmap nvg_gl3.GenerateMipmap
#define glGetProgramInfoLog nvg_gl3.GetProgramInfoLog
#define glGetProgramiv nvg_gl3.GetProgramiv
#define glGetShaderInfoLog nvg_gl3.GetShaderInfoLog
#define glGetShaderiv nvg_gl3.GetShaderiv
#define glGetUniformBlockIndex nvg_gl3.GetUniformBlockIndex
#define glGetUniformLocation nvg_gl3.GetUniformLocation
#define glLinkProgram nvg_gl3.LinkProgram
#define glShaderSource nvg_gl3.ShaderSource
#define glStencilOpSeparate nvg_gl3.StencilOpSeparate
#define glUniform1i nvg_gl3.Uniform1i
#define glUniform2fv nvg_gl3.Uniform2fv
#define glUniform4fv nvg_gl3.Uniform4fv
#define glUniformBlockBinding nvg_gl3.UniformBlockBinding
#define glUseProgram nvg_gl3.UseProgram
#define glVertexAttribPointer nvg_gl3.VertexAttribPointer

#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg_gl.h"

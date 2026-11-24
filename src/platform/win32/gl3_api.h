#pragma once

// Desktop OpenGL 3.3 API for portable GL code on the Windows backend.
//
// Puck3D is written against the GLES 3.0 entry points. A WGL 3.3 core context
// provides the same operations, but opengl32.dll only exports OpenGL 1.1, so
// gl_loader.cpp resolves the newer entry points into the table NanoVG also
// uses. Including this header maps the gl* calls that opengl32.dll does not
// export onto that table; the OpenGL 1.1 calls stay direct, exactly like
// nanovg_gl3_impl.h does for the vendored NanoVG backend.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <GL/gl.h>
#include <GL/glext.h>

#include "nanovg_gl3_functions.h"

#define glActiveTexture nvg_gl3.ActiveTexture
#define glAttachShader nvg_gl3.AttachShader
#define glBindBuffer nvg_gl3.BindBuffer
#define glBindFramebuffer nvg_gl3.BindFramebuffer
#define glBindRenderbuffer nvg_gl3.BindRenderbuffer
#define glBindVertexArray nvg_gl3.BindVertexArray
#define glBufferData nvg_gl3.BufferData
#define glCheckFramebufferStatus nvg_gl3.CheckFramebufferStatus
#define glCompileShader nvg_gl3.CompileShader
#define glCreateProgram nvg_gl3.CreateProgram
#define glCreateShader nvg_gl3.CreateShader
#define glDeleteBuffers nvg_gl3.DeleteBuffers
#define glDeleteFramebuffers nvg_gl3.DeleteFramebuffers
#define glDeleteProgram nvg_gl3.DeleteProgram
#define glDeleteRenderbuffers nvg_gl3.DeleteRenderbuffers
#define glDeleteShader nvg_gl3.DeleteShader
#define glDeleteVertexArrays nvg_gl3.DeleteVertexArrays
#define glEnableVertexAttribArray nvg_gl3.EnableVertexAttribArray
#define glFramebufferRenderbuffer nvg_gl3.FramebufferRenderbuffer
#define glFramebufferTexture2D nvg_gl3.FramebufferTexture2D
#define glGenBuffers nvg_gl3.GenBuffers
#define glGenFramebuffers nvg_gl3.GenFramebuffers
#define glGenRenderbuffers nvg_gl3.GenRenderbuffers
#define glGenVertexArrays nvg_gl3.GenVertexArrays
#define glGenerateMipmap nvg_gl3.GenerateMipmap
#define glGetProgramInfoLog nvg_gl3.GetProgramInfoLog
#define glGetProgramiv nvg_gl3.GetProgramiv
#define glGetShaderInfoLog nvg_gl3.GetShaderInfoLog
#define glGetShaderiv nvg_gl3.GetShaderiv
#define glGetUniformLocation nvg_gl3.GetUniformLocation
#define glLinkProgram nvg_gl3.LinkProgram
#define glRenderbufferStorage nvg_gl3.RenderbufferStorage
#define glShaderSource nvg_gl3.ShaderSource
#define glUniform1f nvg_gl3.Uniform1f
#define glUniform1i nvg_gl3.Uniform1i
#define glUniform2f nvg_gl3.Uniform2f
#define glUniform3f nvg_gl3.Uniform3f
#define glUniform4f nvg_gl3.Uniform4f
#define glUniformMatrix3fv nvg_gl3.UniformMatrix3fv
#define glUniformMatrix4fv nvg_gl3.UniformMatrix4fv
#define glUseProgram nvg_gl3.UseProgram
#define glVertexAttribPointer nvg_gl3.VertexAttribPointer

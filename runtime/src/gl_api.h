// Minimal OpenGL 3.3 core loader (function pointers resolved through SDL).
#pragma once

#include <SDL3/SDL_opengl.h>

// X(return, name, params)
#define GL_FUNCS(X)                                                                                         \
    X(void, glActiveTexture, (GLenum))                                                                      \
    X(void, glAttachShader, (GLuint, GLuint))                                                               \
    X(void, glBindBuffer, (GLenum, GLuint))                                                                 \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                                            \
    X(void, glBindVertexArray, (GLuint))                                                                    \
    X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                             \
    X(void, glBlendEquationSeparate, (GLenum, GLenum))                                                      \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                          \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                        \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                                   \
    X(void, glGenRenderbuffers, (GLsizei, GLuint*))                                                         \
    X(void, glBindRenderbuffer, (GLenum, GLuint))                                                           \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                  \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                     \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                    \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*))                                                \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                                           \
    X(void, glCompileShader, (GLuint))                                                                      \
    X(GLuint, glCreateProgram, (void))                                                                      \
    X(GLuint, glCreateShader, (GLenum))                                                                     \
    X(void, glDeleteBuffers, (GLsizei, const GLuint*))                                                      \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*))                                                 \
    X(void, glDeleteProgram, (GLuint))                                                                      \
    X(void, glDeleteShader, (GLuint))                                                                       \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint*))                                                 \
    X(void, glEnableVertexAttribArray, (GLuint))                                                            \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                \
    X(void, glGenBuffers, (GLsizei, GLuint*))                                                               \
    X(void, glGenFramebuffers, (GLsizei, GLuint*))                                                          \
    X(void, glGenVertexArrays, (GLsizei, GLuint*))                                                          \
    X(GLint, glGetAttribLocation, (GLuint, const GLchar*))                                                  \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                      \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*))                                                       \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                       \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*))                                                        \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*))                                                 \
    X(void, glLinkProgram, (GLuint))                                                                        \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                          \
    X(void, glUniform1f, (GLint, GLfloat))                                                                  \
    X(void, glUniform1i, (GLint, GLint))                                                                    \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                                         \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                       \
    X(void, glUniform2i, (GLint, GLint, GLint))                                                             \
    X(void, glUniform4i, (GLint, GLint, GLint, GLint, GLint))                                               \
    X(void, glUseProgram, (GLuint))                                                                         \
    X(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void*))                          \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))

#define GL_DECLARE(ret, name, params) extern ret(APIENTRY* p_##name) params;
GL_FUNCS(GL_DECLARE)
#undef GL_DECLARE

// Call sites use the normal names.
#define glActiveTexture p_glActiveTexture
#define glAttachShader p_glAttachShader
#define glBindBuffer p_glBindBuffer
#define glBindFramebuffer p_glBindFramebuffer
#define glBindVertexArray p_glBindVertexArray
#define glBlendColor p_glBlendColor
#define glBlendEquationSeparate p_glBlendEquationSeparate
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlitFramebuffer p_glBlitFramebuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorageMultisample p_glRenderbufferStorageMultisample
#define glRenderbufferStorage p_glRenderbufferStorage
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glCompileShader p_glCompileShader
#define glCreateProgram p_glCreateProgram
#define glCreateShader p_glCreateShader
#define glDeleteBuffers p_glDeleteBuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDeleteProgram p_glDeleteProgram
#define glDeleteShader p_glDeleteShader
#define glDeleteVertexArrays p_glDeleteVertexArrays
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glGenBuffers p_glGenBuffers
#define glGenFramebuffers p_glGenFramebuffers
#define glGenVertexArrays p_glGenVertexArrays
#define glGetAttribLocation p_glGetAttribLocation
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glGetProgramiv p_glGetProgramiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetUniformLocation p_glGetUniformLocation
#define glLinkProgram p_glLinkProgram
#define glShaderSource p_glShaderSource
#define glUniform1f p_glUniform1f
#define glUniform1i p_glUniform1i
#define glUniform2f p_glUniform2f
#define glUniform4f p_glUniform4f
#define glUniform2i p_glUniform2i
#define glUniform4i p_glUniform4i
#define glUseProgram p_glUseProgram
#define glVertexAttribIPointer p_glVertexAttribIPointer
#define glVertexAttribPointer p_glVertexAttribPointer

bool gl_load();  // after the context is current; false if anything is missing

// Compiles and links a program; logs and returns 0 on error.
GLuint gl_program(const char* vs, const char* fs);

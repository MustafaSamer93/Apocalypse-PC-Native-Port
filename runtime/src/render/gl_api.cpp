#include "core/psx.h"
#include "render/gl_api.h"

#include <SDL3/SDL.h>

#define GL_DEFINE(ret, name, params) ret(APIENTRY* p_##name) params = nullptr;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

bool gl_load() {
    bool ok = true;
#define GL_LOAD_FN(ret, name, params)                                           \
    p_##name = (ret(APIENTRY*) params)SDL_GL_GetProcAddress(#name);          \
    if (!p_##name) {                                                         \
        log_msg(LogCat::Host, "OpenGL function %s not available", #name);   \
        ok = false;                                                          \
    }
    GL_FUNCS(GL_LOAD_FN)
#undef GL_LOAD_FN
    return ok;
}

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        log_msg(LogCat::Host, "shader compile error:\n%s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint gl_program(const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        log_msg(LogCat::Host, "shader link error:\n%s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

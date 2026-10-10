#include "render/render_backend.h"

namespace {
Backend g_backend = Backend::OpenGL;
}

void render_select(Backend b) { g_backend = b; }
Backend render_backend() { return g_backend; }
const char* render_backend_name(Backend b) { return b == Backend::Vulkan ? "Vulkan" : "OpenGL"; }

#define RENDER_FORWARD(ret, name, params, args) \
    ret name params { return g_backend == Backend::Vulkan ? vk_##name args : gl_##name args; }
RENDER_FUNCS(RENDER_FORWARD)
#undef RENDER_FORWARD

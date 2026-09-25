cmake_minimum_required(VERSION 3.24)
project(Repocalypse C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

option(RECOMP_RA_CHECK "Report functions that return to an unexpected address" ON)

# Static C/C++ runtime: the released exe then runs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------- SDL3
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

# ---------------------------------------------------------------- Vulkan (optional backend)
# Headers and the volk loader are enough to build (the driver's vulkan-1.dll is loaded at run
# time); glslang is built once as a host tool to compile the Vulkan shaders to SPIR-V.
FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.275.tar.gz)
FetchContent_Declare(volk  # used as a plain source file (SOURCE_SUBDIR skips its CMake)
    URL https://github.com/zeux/volk/archive/refs/tags/1.3.270.tar.gz
    SOURCE_SUBDIR none)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    URL https://github.com/KhronosGroup/glslang/archive/refs/tags/14.0.0.tar.gz)
FetchContent_MakeAvailable(VulkanHeaders glslang volk)

# Vulkan shaders (runtime/shaders/*.vert|frag) -> SPIR-V as C arrays in the build tree.
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(GLOB VK_SHADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/shaders/*.vert ${CMAKE_SOURCE_DIR}/runtime/shaders/*.frag)
set(VK_SHADER_HEADERS)
foreach(src ${VK_SHADERS})
    get_filename_component(name ${src} NAME)
    string(REPLACE "." "_" var ${name})
    set(out ${SHADER_OUT}/${name}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
        COMMAND glslang-standalone -V --target-env vulkan1.2 --vn kSpv_${var} -o ${out} ${src}
        DEPENDS ${src} glslang-standalone
        COMMENT "SPIR-V ${name}")
    list(APPEND VK_SHADER_HEADERS ${out})
endforeach()
add_custom_target(vk_shaders DEPENDS ${VK_SHADER_HEADERS})

# ---------------------------------------------------------------- generated code
# Produced by tools/recomp/main.py from the user's own disc; never committed.
set(GEN_DIR ${CMAKE_SOURCE_DIR}/generated)
file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS ${GEN_DIR}/main_*.c)
if(NOT GENERATED_SOURCES)
    message(FATAL_ERROR "No generated code in ${GEN_DIR}. Run:\n"
        "  python tools/discfs.py extract\n  python tools/recomp/main.py")
endif()

add_library(game_code STATIC ${GENERATED_SOURCES})
target_include_directories(game_code PUBLIC runtime/include ${GEN_DIR})
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND MSVC)
    # clang-cl: -w silences warnings about the generated code's style (unused labels etc.)
    target_compile_options(game_code PRIVATE /bigobj -w $<$<CONFIG:Release,RelWithDebInfo>:/O2>)
elseif(MSVC)
    # huge goto-heavy functions: /Ob1 keeps compile times sane; /bigobj for section counts
    target_compile_options(game_code PRIVATE /bigobj /W0 $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Ob1>)
else()
    target_compile_options(game_code PRIVATE -w -fno-strict-aliasing $<$<CONFIG:Release,RelWithDebInfo>:-O2>)
endif()
if(RECOMP_RA_CHECK)
    target_compile_definitions(game_code PUBLIC RECOMP_RA_CHECK)
endif()

# ---------------------------------------------------------------- runtime
add_executable(apocalypse
    runtime/src/main.cpp
    runtime/src/core.cpp
    runtime/src/bios.cpp
    runtime/src/memcard.cpp
    runtime/src/interp.cpp
    runtime/src/gte.cpp
    runtime/src/hw.cpp
    runtime/src/gpu.cpp
    runtime/src/cdrom.cpp
    runtime/src/spu.cpp
    runtime/src/mdec.cpp
    runtime/src/host_sdl.cpp
    runtime/src/gl_api.cpp
    runtime/src/gpu_hw.cpp
    runtime/src/present.cpp
    runtime/src/savestate.cpp
    runtime/src/smooth.cpp
    runtime/src/sim60.cpp
    runtime/src/drawdist.cpp
    runtime/src/render_backend.cpp
    runtime/src/replace.cpp
    runtime/src/vk_context.cpp
    runtime/src/gpu_vk.cpp
    runtime/src/present_vk.cpp
    ${volk_SOURCE_DIR}/volk.c)
target_include_directories(apocalypse PRIVATE runtime/include runtime/src ${volk_SOURCE_DIR} ${SHADER_OUT})
target_compile_definitions(apocalypse PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
add_dependencies(apocalypse vk_shaders)
# The game is a single self-contained exe (static SDL and C runtime) in the project folder,
# next to assets\; a Windows app, so starting it opens no console window.
set_target_properties(apocalypse PROPERTIES
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR})

# GTE known-answer tests (no game code needed)
add_executable(gte_test runtime/tests/gte_test.cpp runtime/src/gte.cpp)
target_include_directories(gte_test PRIVATE runtime/include runtime/src)
find_package(OpenGL REQUIRED)
target_link_libraries(apocalypse PRIVATE game_code SDL3::SDL3-static OpenGL::GL Vulkan::Headers)
if(WIN32)
    target_link_libraries(apocalypse PRIVATE windowscodecs ole32)  # replacement pictures (replace.cpp)
endif()
if(MSVC)
    target_compile_options(apocalypse PRIVATE /W3 /permissive- $<$<NOT:$<CXX_COMPILER_ID:Clang>>:/Zc:preprocessor>)
    target_compile_definitions(apocalypse PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(apocalypse PRIVATE -Wall -Wno-unused-function)
endif()

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
    runtime/src/sim60.cpp)
target_include_directories(apocalypse PRIVATE runtime/include runtime/src)
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
target_link_libraries(apocalypse PRIVATE game_code SDL3::SDL3-static OpenGL::GL)
if(MSVC)
    target_compile_options(apocalypse PRIVATE /W3 /permissive- $<$<NOT:$<CXX_COMPILER_ID:Clang>>:/Zc:preprocessor>)
    target_compile_definitions(apocalypse PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(apocalypse PRIVATE -Wall -Wno-unused-function)
endif()

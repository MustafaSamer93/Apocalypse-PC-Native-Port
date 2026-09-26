// Shared Vulkan state for the Vulkan renderer (gpu_vk.cpp) and presenter (present_vk.cpp).
//
// One command buffer is recorded continuously: the emulator's VRAM work and the frame's
// presentation go into it and it is submitted when the frame is presented, or earlier when
// the CPU needs results (a VRAM read-back, a screenshot). Other submits are waited for, so
// staging memory and descriptors are free again afterwards (frames are the exception: see
// submit_frame). Barriers are deliberately
// coarse (all commands, all memory): the GPU work per frame is small.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES  // volk loads the functions
#endif
#include <volk.h>

#include <SDL3/SDL.h>

#include <cstdint>

struct VkImg {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;  // null for swapchain images
    VkFormat format = VK_FORMAT_UNDEFINED;
    int w = 0, h = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    unsigned id = 0;  // handle for the presenter (vkc::texture), 0 = none
};

namespace vkc {

extern VkInstance instance;
extern VkPhysicalDevice gpu;
extern VkDevice device;
extern VkQueue queue;
extern VkSurfaceKHR surface;

bool init(SDL_Window* window);
void shutdown();
bool ready();

// The command buffer being recorded (begun on first use).
VkCommandBuffer cmd();
// Submits what was recorded and waits for all GPU work; recording continues in a fresh buffer.
void submit_wait();
// The next submit waits for this semaphore (a swapchain image became available).
void wait_next_submit(VkSemaphore s);
// Submits the frame, signalling `done` for presentation, without waiting: the GPU draws it
// while the next frame is emulated (at most two frames in flight).
void submit_frame(VkSemaphore done);
// Which of the two in-flight slots is recording (for per-frame semaphores).
int frame_slot();

// Images and layouts. transition() always records a full barrier, so it also orders two
// operations that use the same layout. Not allowed while rendering (call end_rendering).
bool create_image(VkImg& img, int w, int h, VkFormat format, VkImageUsageFlags usage,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
void destroy_image(VkImg& img);
void transition(VkImg& img, VkImageLayout layout);

// Dynamic rendering into `target` (whole image; draws set viewport/scissor). Starting on the
// target already being rendered to is free; another target ends the current rendering.
void begin_rendering(VkImg& target);
void end_rendering();

// Host-visible memory usable until the next submit: vertex data, uploads, read-backs.
struct Staging {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* ptr;
};
Staging stage(VkDeviceSize size);
VkDeviceSize stage_limit();  // the largest single stage() (bigger transfers go in parts)

// A combined image sampler descriptor for an image (cached per image and filter).
VkDescriptorSetLayout sampler_layout();
VkDescriptorSet sampler_set(const VkImg& img, bool linear);

VkShaderModule shader(const uint32_t* code, size_t bytes);

// Pipelines for dynamic rendering: one colour attachment, dynamic viewport/scissor/blend
// constants, set 0 = one combined image sampler, push constants for the fragment stage
// (and vertex stage).
enum class Blend { None, Half, Add, Subtract, AddQuarter, Alpha };  // PS1 semi modes 0..3, overlay
enum class Vertices { None, Vram, Position };                       // fullscreen quad / Vtx layout
struct PipelineDesc {
    VkShaderModule vs, fs;
    VkPipelineLayout layout;
    VkFormat format;
    VkSampleCountFlagBits samples;
    Blend blend;
    Vertices vertices;
};
VkPipelineLayout make_layout(uint32_t push_bytes);
VkPipeline make_pipeline(const PipelineDesc& d);

// Presenter handles for images (hw_display_texture, hw_capture_texture).
unsigned register_texture(VkImg* img);
VkImg* texture(unsigned id);

}  // namespace vkc

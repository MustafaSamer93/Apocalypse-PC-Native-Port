#include "core/psx.h"
#include "render/vk_context.h"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace vkc {

VkInstance instance = VK_NULL_HANDLE;
VkPhysicalDevice gpu = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;
VkSurfaceKHR surface = VK_NULL_HANDLE;

namespace {

uint32_t g_family = 0;
VkCommandPool g_pool = VK_NULL_HANDLE;
// Two frames can be in flight: each slot has its command buffer, fence and half of the
// staging memory, which are reused only after the slot's fence (two submits later).
constexpr int kSlots = 2;
VkCommandBuffer g_cmds[kSlots] = {};
VkFence g_fences[kSlots] = {};
int g_slot = 0;
VkCommandBuffer g_cmd = VK_NULL_HANDLE;  // g_cmds[g_slot]
bool g_recording = false;
VkSemaphore g_wait_sem = VK_NULL_HANDLE;

constexpr VkDeviceSize kStagingSize = 64ull << 20;  // both slots
constexpr VkDeviceSize kSlotStaging = kStagingSize / kSlots;
VkBuffer g_stage_buf = VK_NULL_HANDLE;
VkDeviceMemory g_stage_mem = VK_NULL_HANDLE;
uint8_t* g_stage_ptr = nullptr;
VkDeviceSize g_stage_off = 0;

VkDescriptorPool g_desc_pool = VK_NULL_HANDLE;
VkDescriptorSetLayout g_sampler_layout = VK_NULL_HANDLE;
VkSampler g_nearest = VK_NULL_HANDLE, g_linear = VK_NULL_HANDLE;
std::map<std::pair<VkImageView, bool>, VkDescriptorSet> g_sets;

VkImg* g_rendering = nullptr;  // image being rendered to, if any
std::map<unsigned, VkImg*> g_textures;
unsigned g_next_texture = 1;

bool check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    log_msg(LogCat::Gpu, "Vulkan: %s failed (%d)", what, (int)r);
    return false;
}

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool pick_gpu() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    int best_score = -1;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
        vkGetPhysicalDeviceFeatures2(d, &f2);
        if (!f13.dynamicRendering) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (uint32_t i = 0; i < qn; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best_score) {
                best_score = score;
                gpu = d;
                g_family = i;
            }
            break;
        }
    }
    if (gpu) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpu, &p);
        log_msg(LogCat::Host, "Vulkan: %s (API %u.%u.%u)", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    }
    return gpu != VK_NULL_HANDLE;
}

void begin_if_needed() {
    if (g_recording) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);
    g_recording = true;
}

// Submits the slot's commands; `wait` = until the GPU has finished all work so far.
void submit(VkSemaphore signal, bool wait) {
    end_rendering();
    begin_if_needed();  // an empty frame still needs a submit to signal / wait on semaphores
    vkEndCommandBuffer(g_cmd);
    g_recording = false;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    if (g_wait_sem) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &g_wait_sem;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    check(vkQueueSubmit(queue, 1, &si, g_fences[g_slot]), "queue submit");
    g_wait_sem = VK_NULL_HANDLE;
    if (wait) vkWaitForFences(device, kSlots, g_fences, VK_TRUE, UINT64_MAX);
    // on to the next slot, once its previous work is done
    g_slot = (g_slot + 1) % kSlots;
    g_cmd = g_cmds[g_slot];
    vkWaitForFences(device, 1, &g_fences[g_slot], VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &g_fences[g_slot]);
    vkResetCommandBuffer(g_cmd, 0);
    g_stage_off = (VkDeviceSize)g_slot * kSlotStaging;
}

}  // namespace

bool ready() { return device != VK_NULL_HANDLE; }

bool init(SDL_Window* window) {
    if (volkInitialize() != VK_SUCCESS) {
        log_msg(LogCat::Host, "Vulkan: no Vulkan driver (vulkan-1.dll) found");
        return false;
    }
    Uint32 next = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&next);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Repocalypse";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = exts;
    if (!check(vkCreateInstance(&ici, nullptr, &instance), "create instance")) return false;
    volkLoadInstance(instance);
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        log_msg(LogCat::Host, "Vulkan: surface: %s", SDL_GetError());
        return false;
    }
    if (!pick_gpu()) {
        log_msg(LogCat::Host, "Vulkan: no GPU with Vulkan 1.3 and dynamic rendering");
        return false;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    if (!check(vkCreateDevice(gpu, &dci, nullptr, &device), "create device")) return false;
    volkLoadDevice(device);
    vkGetDeviceQueue(device, g_family, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    vkCreateCommandPool(device, &pci, nullptr, &g_pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kSlots;
    vkAllocateCommandBuffers(device, &cai, g_cmds);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // slots start out free
    for (VkFence& f : g_fences) vkCreateFence(device, &fci, nullptr, &f);
    g_slot = 0;
    g_cmd = g_cmds[0];
    vkResetFences(device, 1, &g_fences[0]);
    g_stage_off = 0;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = kStagingSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (!check(vkCreateBuffer(device, &bci, nullptr, &g_stage_buf), "staging buffer")) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, g_stage_buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memory_type(mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device, &mai, nullptr, &g_stage_mem), "staging memory"))
        return false;
    vkBindBufferMemory(device, g_stage_buf, g_stage_mem, 0);
    vkMapMemory(device, g_stage_mem, 0, kStagingSize, 0, (void**)&g_stage_ptr);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 256;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    vkCreateDescriptorPool(device, &dpi, nullptr, &g_desc_pool);
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &b;
    vkCreateDescriptorSetLayout(device, &lci, nullptr, &g_sampler_layout);
    for (bool linear : {false, true}) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        vkCreateSampler(device, &sci, nullptr, linear ? &g_linear : &g_nearest);
    }
    return true;
}

void shutdown() {
    if (device) {
        if (g_recording) submit(VK_NULL_HANDLE, true);
        vkDeviceWaitIdle(device);
        g_sets.clear();
        g_textures.clear();
        if (g_desc_pool) vkDestroyDescriptorPool(device, g_desc_pool, nullptr);
        if (g_sampler_layout) vkDestroyDescriptorSetLayout(device, g_sampler_layout, nullptr);
        if (g_nearest) vkDestroySampler(device, g_nearest, nullptr);
        if (g_linear) vkDestroySampler(device, g_linear, nullptr);
        if (g_stage_buf) vkDestroyBuffer(device, g_stage_buf, nullptr);
        if (g_stage_mem) vkFreeMemory(device, g_stage_mem, nullptr);
        for (VkFence f : g_fences)
            if (f) vkDestroyFence(device, f, nullptr);
        if (g_pool) vkDestroyCommandPool(device, g_pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
    surface = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    gpu = VK_NULL_HANDLE;
    g_desc_pool = VK_NULL_HANDLE;
    g_sampler_layout = VK_NULL_HANDLE;
    g_nearest = g_linear = VK_NULL_HANDLE;
    g_stage_buf = VK_NULL_HANDLE;
    g_stage_mem = VK_NULL_HANDLE;
    g_stage_ptr = nullptr;
    for (VkFence& f : g_fences) f = VK_NULL_HANDLE;
    for (VkCommandBuffer& c : g_cmds) c = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    g_cmd = VK_NULL_HANDLE;
    g_recording = false;
    g_rendering = nullptr;
}

VkCommandBuffer cmd() {
    begin_if_needed();
    return g_cmd;
}

void submit_wait() { submit(VK_NULL_HANDLE, true); }
void wait_next_submit(VkSemaphore s) { g_wait_sem = s; }
void submit_frame(VkSemaphore done) { submit(done, false); }
int frame_slot() { return g_slot; }

VkImageAspectFlags aspect_of(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

bool create_image(VkImg& img, int w, int h, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples, int mips) {
    img = VkImg{};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {(uint32_t)w, (uint32_t)h, 1};
    ici.mipLevels = (uint32_t)mips;
    ici.arrayLayers = 1;
    ici.samples = samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ici, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, img.image, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &img.mem) != VK_SUCCESS) {
        vkDestroyImage(device, img.image, nullptr);
        img = VkImg{};
        return false;
    }
    vkBindImageMemory(device, img.image, img.mem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect_of(format), 0, (uint32_t)mips, 0, 1};
    vkCreateImageView(device, &vci, nullptr, &img.view);
    img.format = format;
    img.w = w;
    img.h = h;
    img.samples = samples;
    return true;
}

void destroy_image(VkImg& img) {
    if (!img.image) return;
    submit_wait();  // commands recorded or in flight may still use it
    for (auto it = g_sets.begin(); it != g_sets.end();) {
        if (it->first.first == img.view) {
            vkFreeDescriptorSets(device, g_desc_pool, 1, &it->second);
            it = g_sets.erase(it);
        } else {
            ++it;
        }
    }
    if (img.id) g_textures.erase(img.id);
    if (g_rendering == &img) g_rendering = nullptr;
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.mem) {  // not a swapchain image
        vkDestroyImage(device, img.image, nullptr);
        vkFreeMemory(device, img.mem, nullptr);
    }
    img = VkImg{};
}

void transition(VkImg& img, VkImageLayout layout) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {aspect_of(img.format), 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    img.layout = layout;
}

VkImg* g_rendering_depth = nullptr;
void begin_rendering(VkImg& target, VkImg* depth) {
    if (g_rendering == &target && g_rendering_depth == depth) return;
    end_rendering();
    transition(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo datt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth) {
        transition(*depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        datt.imageView = depth->view;
        datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        datt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {(uint32_t)target.w, (uint32_t)target.h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    ri.pDepthAttachment = depth ? &datt : nullptr;
    vkCmdBeginRendering(cmd(), &ri);
    g_rendering = &target;
    g_rendering_depth = depth;
}

void end_rendering() {
    if (!g_rendering) return;
    vkCmdEndRendering(g_cmd);
    g_rendering = nullptr;
}

VkDeviceSize stage_limit() { return kSlotStaging; }

Staging stage(VkDeviceSize size) {
    size = (size + 255) & ~VkDeviceSize(255);
    if (g_stage_off + size > (VkDeviceSize)(g_slot + 1) * kSlotStaging) submit_wait();  // slot's half is full
    Staging s{g_stage_buf, g_stage_off, g_stage_ptr + g_stage_off};
    g_stage_off += size;
    return s;
}

VkDescriptorSet sampler_set(const VkImg& img, bool linear) {
    auto key = std::make_pair(img.view, linear);
    auto it = g_sets.find(key);
    if (it != g_sets.end()) return it->second;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_sampler_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!check(vkAllocateDescriptorSets(device, &ai, &set), "descriptor set")) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii{linear ? g_linear : g_nearest, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    g_sets[key] = set;
    return set;
}

VkShaderModule shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &ci, nullptr, &m), "shader module");
    return m;
}

VkPipelineLayout make_layout(uint32_t push_bytes, uint32_t sets) {
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout layouts[2] = {g_sampler_layout, g_sampler_layout};
    ci.setLayoutCount = sets;
    ci.pSetLayouts = layouts;
    ci.pushConstantRangeCount = push_bytes ? 1 : 0;
    ci.pPushConstantRanges = &pr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &ci, nullptr, &l), "pipeline layout");
    return l;
}

// Vertex layout shared with gpu_vk.cpp (VkVtx): pos, colour, uv, info, w, box, z.
static constexpr uint32_t kVtxStride = 40;

VkPipeline make_pipeline(const PipelineDesc& d) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = d.vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = d.fs;
    st[1].pName = "main";

    VkVertexInputBindingDescription bind{0, kVtxStride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[7] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 12},
        {3, 0, VK_FORMAT_R32G32_UINT, 20},
        {4, 0, VK_FORMAT_R32_SFLOAT, 28},
        {5, 0, VK_FORMAT_R32_UINT, 32},
        {6, 0, VK_FORMAT_R32_SFLOAT, 36},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (d.vertices != Vertices::None) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = d.vertices == Vertices::Vram ? 7 : 1;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d.vertices == Vertices::None ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    switch (d.blend) {
        case Blend::None: break;
        case Blend::Half:  // B/2 + F/2 (blend constant alpha = 0.5)
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = ba.dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            break;
        case Blend::Add:  // B + F
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case Blend::Subtract:  // B - F
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
            break;
        case Blend::AddQuarter:  // B + F/4 (blend constant alpha = 0.25)
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case Blend::Alpha:
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case Blend::MaskOpaque:  // a pixel with its mask bit (alpha) set keeps colour and bit
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            break;
        case Blend::InsideOpaque:  // (ours) only where the mask bit is set; the bits stay
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case Blend::InsideAdd:  // B + F only where the mask bit is set
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case Blend::MaskAdd:  // B + F where the mask bit is clear; the bits stay
        case Blend::MaskSubtract:  // B - F likewise
            ba.blendEnable = VK_TRUE;
            if (d.blend == Blend::MaskSubtract) ba.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = d.depth != VK_FORMAT_UNDEFINED ? 5 : 3;
    ds.pDynamicStates = dyn;
    // Z-buffer (gpu_vk.cpp): LEQUAL, test and write set per batch (dynamic state)
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    dss.maxDepthBounds = 1.0f;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &d.format;
    rci.depthAttachmentFormat = d.depth;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci};
    pci.stageCount = 2;
    pci.pStages = st;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = d.depth != VK_FORMAT_UNDEFINED ? &dss : nullptr;
    pci.pDynamicState = &ds;
    pci.layout = d.layout;
    VkPipeline p = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &p), "graphics pipeline");
    return p;
}

unsigned register_texture(VkImg* img) {
    if (!img->id) img->id = g_next_texture++;
    g_textures[img->id] = img;
    return img->id;
}

VkImg* texture(unsigned id) {
    auto it = g_textures.find(id);
    return it == g_textures.end() ? nullptr : it->second;
}

}  // namespace vkc

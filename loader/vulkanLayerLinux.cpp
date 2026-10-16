#include <vulkan/vk_layer.h>
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct InstanceState {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr nextGipa = nullptr;
};

struct DeviceState {
    VkDevice device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr nextGipa = nullptr;
    PFN_vkGetDeviceProcAddr nextGdpa = nullptr;
    PFN_vkQueuePresentKHR nextPresent = nullptr;
    std::uint32_t queueFamily = UINT32_MAX;
    std::uint32_t graphicsQueueFamily = UINT32_MAX;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
};

struct SwapchainState {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags imageUsage = 0;
    std::uint32_t minImageCount = 0;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkSemaphore> renderSemaphores;
};

std::mutex stateMutex;
std::unordered_map<void*, InstanceState> instances;
std::unordered_map<void*, InstanceState> physicalDevices;
std::unordered_map<void*, DeviceState> devices;
std::unordered_map<void*, void*> queues;
std::unordered_map<void*, std::uint32_t> queueFamilies;
std::unordered_map<void*, SwapchainState> swapchains;
bool imguiBackendReady = false;
VkDevice imguiBackendDevice = VK_NULL_HANDLE;
std::atomic<std::uint64_t> frameNumber{0};
std::atomic<std::int64_t> lastFrameNs{0};

void setProxyRendererReady(bool ready) {
    using ReadyFn = void (*)(bool);
    static auto setReady = reinterpret_cast<ReadyFn>(dlsym(RTLD_DEFAULT, "wheatSetImGuiRendererReady"));
    if (setReady) setReady(ready);
}

void reportRendererStatus(const char* status) {
    static std::atomic<const char*> lastReported{nullptr};
    if (!status || lastReported.exchange(status, std::memory_order_acq_rel) == status) return;
    using StatusFn = void (*)(const char*);
    static auto report = reinterpret_cast<StatusFn>(dlsym(RTLD_DEFAULT, "wheatSetImGuiRendererStatus"));
    if (report) report(status);
    using LogFn = void (*)(const char*);
    static auto log = reinterpret_cast<LogFn>(dlsym(RTLD_DEFAULT, "wheatLogImGuiDiagnostic"));
    if (log) log(status);
}

void setProxyDisplaySize(float width, float height) {
    using DisplaySizeFn = void (*)(float, float);
    static auto setSize = reinterpret_cast<DisplaySizeFn>(dlsym(RTLD_DEFAULT, "wheatSetImGuiDisplaySize"));
    if (setSize) setSize(width, height);
}

struct ProxyGuiLock {
    using LockFn = void (*)();
    LockFn unlock = nullptr;
    bool held = false;

    explicit ProxyGuiLock(bool acquire) {
        if (!acquire) return;
        static auto lock = reinterpret_cast<LockFn>(dlsym(RTLD_DEFAULT, "wheatLockImGui"));
        unlock = reinterpret_cast<LockFn>(dlsym(RTLD_DEFAULT, "wheatUnlockImGui"));
        if (lock && unlock) {
            lock();
            held = true;
        }
    }
    ~ProxyGuiLock() { release(); }
    void release() {
        if (held && unlock) unlock();
        held = false;
    }
};

template <typename Handle>
void* handleKey(Handle handle) {
    return reinterpret_cast<void*>(handle);
}

VkLayerInstanceCreateInfo* findInstanceLink(const VkInstanceCreateInfo* info) {
    auto* node = reinterpret_cast<const VkBaseInStructure*>(info->pNext);
    while (node) {
        if (node->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto* link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(node));
            if (link->function == VK_LAYER_LINK_INFO) return link;
        }
        node = node->pNext;
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* findDeviceLink(const VkDeviceCreateInfo* info) {
    auto* node = reinterpret_cast<const VkBaseInStructure*>(info->pNext);
    while (node) {
        if (node->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(node));
            if (link->function == VK_LAYER_LINK_INFO) return link;
        }
        node = node->pNext;
    }
    return nullptr;
}

PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(VkInstance, const char*);
PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice, const char*);
VkResult VKAPI_CALL layerCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VkResult VKAPI_CALL layerCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VkResult VKAPI_CALL layerEnumeratePhysicalDevices(VkInstance, std::uint32_t*, VkPhysicalDevice*);
void VKAPI_CALL layerDestroyInstance(VkInstance, const VkAllocationCallbacks*);
void VKAPI_CALL layerDestroyDevice(VkDevice, const VkAllocationCallbacks*);
void VKAPI_CALL layerGetDeviceQueue(VkDevice, std::uint32_t, std::uint32_t, VkQueue*);
void VKAPI_CALL layerGetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
VkResult VKAPI_CALL layerQueuePresent(VkQueue, const VkPresentInfoKHR*);
VkResult VKAPI_CALL layerCreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
void VKAPI_CALL layerDestroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);

InstanceState instanceState(VkInstance instance) {
    std::lock_guard<std::mutex> lock(stateMutex);
    const auto it = instances.find(handleKey(instance));
    return it == instances.end() ? InstanceState{} : it->second;
}

DeviceState deviceState(VkDevice device) {
    std::lock_guard<std::mutex> lock(stateMutex);
    const auto it = devices.find(handleKey(device));
    return it == devices.end() ? DeviceState{} : it->second;
}

VkResult VKAPI_CALL layerCreateInstance(const VkInstanceCreateInfo* info,
                                         const VkAllocationCallbacks* allocator,
                                         VkInstance* instance) {
    if (!info || !instance) return VK_ERROR_INITIALIZATION_FAILED;
    auto* chain = findInstanceLink(info);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto* link = chain->u.pLayerInfo;
    const auto nextGipa = link->pfnNextGetInstanceProcAddr;
    const auto create = nextGipa ? reinterpret_cast<PFN_vkCreateInstance>(nextGipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = link->pNext;
    const VkResult result = create(info, allocator, instance);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(stateMutex);
        instances[handleKey(*instance)] = InstanceState{*instance, nextGipa};
    }
    return result;
}

VkResult VKAPI_CALL layerEnumeratePhysicalDevices(VkInstance instance, std::uint32_t* count,
                                                   VkPhysicalDevice* physicals) {
    const auto parent = instanceState(instance);
    if (!parent.nextGipa) return VK_ERROR_INITIALIZATION_FAILED;
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        parent.nextGipa(instance, "vkEnumeratePhysicalDevices"));
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = enumerate(instance, count, physicals);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count && physicals) {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (std::uint32_t index = 0; index < *count; ++index)
            physicalDevices[handleKey(physicals[index])] = parent;
    }
    return result;
}

VkResult VKAPI_CALL layerCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                       const VkAllocationCallbacks* allocator, VkDevice* device) {
    if (!physical || !info || !device) return VK_ERROR_INITIALIZATION_FAILED;
    InstanceState parent;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        const auto it = physicalDevices.find(handleKey(physical));
        if (it == physicalDevices.end()) return VK_ERROR_INITIALIZATION_FAILED;
        parent = it->second;
    }
    auto* chain = findDeviceLink(info);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto* link = chain->u.pLayerInfo;
    const auto nextGipa = link->pfnNextGetInstanceProcAddr;
    const auto nextGdpa = link->pfnNextGetDeviceProcAddr;
    const auto create = nextGipa ? reinterpret_cast<PFN_vkCreateDevice>(nextGipa(parent.instance, "vkCreateDevice")) : nullptr;
    if (!create || !nextGdpa) return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = link->pNext;
    const VkResult result = create(physical, info, allocator, device);
    if (result == VK_SUCCESS) {
        std::uint32_t graphicsFamily = UINT32_MAX;
        const auto getFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            parent.nextGipa(parent.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        if (getFamilies) {
            std::uint32_t familyCount = 0;
            getFamilies(physical, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            getFamilies(physical, &familyCount, families.data());
            for (std::uint32_t queueInfoIndex = 0; queueInfoIndex < info->queueCreateInfoCount; ++queueInfoIndex) {
                const auto family = info->pQueueCreateInfos[queueInfoIndex].queueFamilyIndex;
                if (family < families.size() && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    graphicsFamily = family;
                    break;
                }
            }
        }
        std::lock_guard<std::mutex> lock(stateMutex);
        devices[handleKey(*device)] = DeviceState{*device, parent.instance, physical, parent.nextGipa, nextGdpa, nullptr,
                                                  UINT32_MAX, graphicsFamily, VK_NULL_HANDLE};
    }
    return result;
}

void rememberQueue(VkDevice device, std::uint32_t family, VkQueue queue) {
    if (!queue) return;
    std::lock_guard<std::mutex> lock(stateMutex);
    if (devices.contains(handleKey(device))) {
        queues[handleKey(queue)] = handleKey(device);
        queueFamilies[handleKey(queue)] = family;
        devices[handleKey(device)].queueFamily = family;
        if (devices[handleKey(device)].graphicsQueueFamily == family)
            devices[handleKey(device)].graphicsQueue = queue;
    }
}

void VKAPI_CALL layerGetDeviceQueue(VkDevice device, std::uint32_t family,
                                    std::uint32_t index, VkQueue* queue) {
    const auto state = deviceState(device);
    if (!state.nextGdpa) return;
    const auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(state.nextGdpa(device, "vkGetDeviceQueue"));
    if (!getQueue) return;
    getQueue(device, family, index, queue);
    if (queue) rememberQueue(device, family, *queue);
}

void VKAPI_CALL layerGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    const auto state = deviceState(device);
    if (!state.nextGdpa) return;
    const auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue2>(state.nextGdpa(device, "vkGetDeviceQueue2"));
    if (!getQueue) return;
    getQueue(device, info, queue);
    if (queue) rememberQueue(device, info ? info->queueFamilyIndex : UINT32_MAX, *queue);
}

template <typename Function>
Function deviceFunction(const DeviceState& state, const char* name) {
    return state.nextGdpa ? reinterpret_cast<Function>(state.nextGdpa(state.device, name)) : nullptr;
}

PFN_vkVoidFunction imguiVulkanFunction(const char* name, void* userData) {
    const auto* device = static_cast<const DeviceState*>(userData);
    if (!name || !device) return nullptr;
    const bool instanceFunction = std::strncmp(name, "vkGetPhysicalDevice", 19) == 0 ||
                                  std::strcmp(name, "vkEnumeratePhysicalDevices") == 0 ||
                                  std::strcmp(name, "vkDestroySurfaceKHR") == 0;
    if (instanceFunction)
        return device->nextGipa ? device->nextGipa(device->instance, name) : nullptr;
    return device->nextGdpa ? device->nextGdpa(device->device, name) : nullptr;
}

void destroySwapchainResources(const DeviceState& device, SwapchainState& chain) {
    const auto destroyFramebuffer = deviceFunction<PFN_vkDestroyFramebuffer>(device, "vkDestroyFramebuffer");
    const auto destroyImageView = deviceFunction<PFN_vkDestroyImageView>(device, "vkDestroyImageView");
    const auto destroyRenderPass = deviceFunction<PFN_vkDestroyRenderPass>(device, "vkDestroyRenderPass");
    const auto destroySemaphore = deviceFunction<PFN_vkDestroySemaphore>(device, "vkDestroySemaphore");
    const auto destroyDescriptorPool = deviceFunction<PFN_vkDestroyDescriptorPool>(device, "vkDestroyDescriptorPool");
    const auto destroyCommandPool = deviceFunction<PFN_vkDestroyCommandPool>(device, "vkDestroyCommandPool");
    for (VkFramebuffer framebuffer : chain.framebuffers)
        if (destroyFramebuffer && framebuffer) destroyFramebuffer(chain.device, framebuffer, nullptr);
    for (VkImageView view : chain.views)
        if (destroyImageView && view) destroyImageView(chain.device, view, nullptr);
    for (VkSemaphore semaphore : chain.renderSemaphores)
        if (destroySemaphore && semaphore) destroySemaphore(chain.device, semaphore, nullptr);
    if (chain.renderPass && destroyRenderPass) destroyRenderPass(chain.device, chain.renderPass, nullptr);
    if (chain.descriptorPool && destroyDescriptorPool) destroyDescriptorPool(chain.device, chain.descriptorPool, nullptr);
    if (chain.commandPool && destroyCommandPool) destroyCommandPool(chain.device, chain.commandPool, nullptr);
    chain.renderPass = VK_NULL_HANDLE;
    chain.commandPool = VK_NULL_HANDLE;
    chain.descriptorPool = VK_NULL_HANDLE;
    chain.images.clear();
    chain.views.clear();
    chain.framebuffers.clear();
    chain.commandBuffers.clear();
    chain.renderSemaphores.clear();
}

bool initializeSwapchainRenderer(const DeviceState& device, SwapchainState& chain) {
    const char* enableRenderer = std::getenv("WHEAT_ENABLE_IMGUI_RENDERER");
    if (!enableRenderer || std::strcmp(enableRenderer, "1") != 0) {
        reportRendererStatus("ImGui Vulkan renderer is paused; Vulkan mod callbacks remain active");
        return false;
    }
    if (imguiBackendReady) return true;
    if (!ImGui::GetCurrentContext()) {
        reportRendererStatus("ImGui setup waiting: no SDL-created ImGui context is available");
        return false;
    }
    if (device.graphicsQueueFamily == UINT32_MAX || !device.graphicsQueue) {
        reportRendererStatus("ImGui setup waiting: no graphics queue was captured");
        return imguiBackendReady;
    }
    reportRendererStatus("ImGui setup: beginning Vulkan swapchain resource creation");
    const auto getImages = deviceFunction<PFN_vkGetSwapchainImagesKHR>(device, "vkGetSwapchainImagesKHR");
    const auto createImageView = deviceFunction<PFN_vkCreateImageView>(device, "vkCreateImageView");
    const auto createRenderPass = deviceFunction<PFN_vkCreateRenderPass>(device, "vkCreateRenderPass");
    const auto createFramebuffer = deviceFunction<PFN_vkCreateFramebuffer>(device, "vkCreateFramebuffer");
    const auto createCommandPool = deviceFunction<PFN_vkCreateCommandPool>(device, "vkCreateCommandPool");
    const auto allocateCommandBuffers = deviceFunction<PFN_vkAllocateCommandBuffers>(device, "vkAllocateCommandBuffers");
    const auto createSemaphore = deviceFunction<PFN_vkCreateSemaphore>(device, "vkCreateSemaphore");
    const auto createDescriptorPool = deviceFunction<PFN_vkCreateDescriptorPool>(device, "vkCreateDescriptorPool");
    if (!(chain.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        reportRendererStatus("ImGui setup unsupported: swapchain images cannot be used as color attachments");
        return false;
    }
    if (!getImages || !createImageView || !createRenderPass || !createFramebuffer || !createCommandPool ||
        !allocateCommandBuffers || !createSemaphore || !createDescriptorPool) {
        reportRendererStatus("ImGui setup failed: required Vulkan device functions are unavailable");
        return false;
    }

    struct ResourceGuard {
        const DeviceState& device;
        SwapchainState& chain;
        bool keep = false;
        ~ResourceGuard() { if (!keep) destroySwapchainResources(device, chain); }
    } guard{device, chain};

    std::uint32_t imageCount = 0;
    if (getImages(chain.device, chain.swapchain, &imageCount, nullptr) != VK_SUCCESS || imageCount < 2) {
        reportRendererStatus("ImGui setup failed: could not query swapchain image count");
        return false;
    }
    chain.images.resize(imageCount);
    if (getImages(chain.device, chain.swapchain, &imageCount, chain.images.data()) != VK_SUCCESS) {
        reportRendererStatus("ImGui setup failed: could not retrieve swapchain images");
        return false;
    }
    chain.images.resize(imageCount);
    chain.views.resize(imageCount);
    chain.framebuffers.resize(imageCount);
    chain.renderSemaphores.resize(imageCount);

    for (std::uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = chain.images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = chain.format;
        viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (createImageView(chain.device, &viewInfo, nullptr, &chain.views[i]) != VK_SUCCESS) return false;
    }

    VkAttachmentDescription attachment{};
    attachment.format = chain.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    if (createRenderPass(chain.device, &passInfo, nullptr, &chain.renderPass) != VK_SUCCESS) return false;

    for (std::uint32_t i = 0; i < imageCount; ++i) {
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = chain.renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &chain.views[i];
        framebufferInfo.width = chain.extent.width;
        framebufferInfo.height = chain.extent.height;
        framebufferInfo.layers = 1;
        if (createFramebuffer(chain.device, &framebufferInfo, nullptr, &chain.framebuffers[i]) != VK_SUCCESS) return false;
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.graphicsQueueFamily;
    if (createCommandPool(chain.device, &poolInfo, nullptr, &chain.commandPool) != VK_SUCCESS) return false;
    chain.commandBuffers.resize(imageCount);
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = chain.commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = imageCount;
    if (allocateCommandBuffers(chain.device, &commandInfo, chain.commandBuffers.data()) != VK_SUCCESS) return false;

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& semaphore : chain.renderSemaphores)
        if (createSemaphore(chain.device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 16},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
    };
    VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptorInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptorInfo.maxSets = 88;
    descriptorInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(poolSizes));
    descriptorInfo.pPoolSizes = poolSizes;
    if (createDescriptorPool(chain.device, &descriptorInfo, nullptr, &chain.descriptorPool) != VK_SUCCESS) return false;

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_0;
    init.Instance = device.instance;
    init.PhysicalDevice = device.physical;
    init.Device = chain.device;
    init.QueueFamily = device.graphicsQueueFamily;
    init.Queue = device.graphicsQueue;
    init.DescriptorPool = chain.descriptorPool;
    init.MinImageCount = std::max(2u, chain.minImageCount);
    init.ImageCount = imageCount;
    init.PipelineInfoMain.RenderPass = chain.renderPass;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    reportRendererStatus("ImGui setup: creating Vulkan renderer pipeline and device objects");
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, imguiVulkanFunction,
                                        const_cast<DeviceState*>(&device))) {
        reportRendererStatus("ImGui setup failed: could not load Vulkan functions through the layer dispatch table");
        return false;
    }
    if (!init.Queue || !ImGui_ImplVulkan_Init(&init)) {
        reportRendererStatus("ImGui setup failed: Vulkan backend initialization failed");
        return false;
    }
    imguiBackendReady = true;
    imguiBackendDevice = chain.device;
    setProxyRendererReady(true);
    reportRendererStatus("Linux ImGui Vulkan renderer initialized successfully");
    guard.keep = true;
    return true;
}

bool drawImGui(const DeviceState& device, SwapchainState& chain, VkQueue queue, std::uint32_t imageIndex,
               const VkPresentInfoKHR* presentInfo) {
    const char* enableDrawing = std::getenv("WHEAT_ENABLE_IMGUI_DRAW");
    if (!enableDrawing || std::strcmp(enableDrawing, "1") != 0) {
        reportRendererStatus("ImGui renderer initialized; drawing is paused until WHEAT_ENABLE_IMGUI_DRAW=1");
        return false;
    }
    if (imageIndex >= chain.commandBuffers.size() || imageIndex >= chain.renderSemaphores.size()) return false;
    using DrawDataFn = ImDrawData* (*)();
    static auto getDrawData = reinterpret_cast<DrawDataFn>(dlsym(RTLD_DEFAULT, "wheatGetImGuiDrawData"));
    ImDrawData* drawData = getDrawData ? getDrawData() : nullptr;
    if (!drawData || drawData->CmdListsCount == 0) return false;

    const auto resetCommandBuffer = deviceFunction<PFN_vkResetCommandBuffer>(device, "vkResetCommandBuffer");
    const auto beginCommandBuffer = deviceFunction<PFN_vkBeginCommandBuffer>(device, "vkBeginCommandBuffer");
    const auto endCommandBuffer = deviceFunction<PFN_vkEndCommandBuffer>(device, "vkEndCommandBuffer");
    const auto cmdPipelineBarrier = deviceFunction<PFN_vkCmdPipelineBarrier>(device, "vkCmdPipelineBarrier");
    const auto cmdBeginRenderPass = deviceFunction<PFN_vkCmdBeginRenderPass>(device, "vkCmdBeginRenderPass");
    const auto cmdEndRenderPass = deviceFunction<PFN_vkCmdEndRenderPass>(device, "vkCmdEndRenderPass");
    const auto queueSubmit = deviceFunction<PFN_vkQueueSubmit>(device, "vkQueueSubmit");
    if (!resetCommandBuffer || !beginCommandBuffer || !endCommandBuffer || !cmdPipelineBarrier ||
        !cmdBeginRenderPass || !cmdEndRenderPass || !queueSubmit) return false;

    VkCommandBuffer command = chain.commandBuffers[imageIndex];
    if (resetCommandBuffer(command, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (beginCommandBuffer(command, &beginInfo) != VK_SUCCESS) return false;

    VkImageMemoryBarrier toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toColor.srcAccessMask = 0;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.image = chain.images[imageIndex];
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    cmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &toColor);

    VkRenderPassBeginInfo renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderInfo.renderPass = chain.renderPass;
    renderInfo.framebuffer = chain.framebuffers[imageIndex];
    renderInfo.renderArea = {{0, 0}, chain.extent};
    cmdBeginRenderPass(command, &renderInfo, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(drawData, command);
    cmdEndRenderPass(command);

    VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = chain.images[imageIndex];
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    cmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &toPresent);
    if (endCommandBuffer(command) != VK_SUCCESS) return false;

    std::vector<VkPipelineStageFlags> waitStages(presentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
    submit.pWaitSemaphores = presentInfo->pWaitSemaphores;
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &chain.renderSemaphores[imageIndex];
    if (!queue || queueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    return true;
}

VkResult VKAPI_CALL layerQueuePresent(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceState state;
    SwapchainState* chain = nullptr;
    bool graphicsPresentQueue = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        const auto queueIt = queues.find(handleKey(queue));
        if (queueIt == queues.end()) return VK_ERROR_INITIALIZATION_FAILED;
        const auto deviceIt = devices.find(queueIt->second);
        if (deviceIt == devices.end()) return VK_ERROR_INITIALIZATION_FAILED;
        state = deviceIt->second;
        const auto familyIt = queueFamilies.find(handleKey(queue));
        graphicsPresentQueue = familyIt != queueFamilies.end() && familyIt->second == state.graphicsQueueFamily;
        if (info && info->swapchainCount > 0 && info->pSwapchains) {
            const auto swapchainIt = swapchains.find(handleKey(info->pSwapchains[0]));
            if (swapchainIt != swapchains.end() && swapchainIt->second.device == state.device) chain = &swapchainIt->second;
        }
    }
    if (!state.nextPresent) return VK_ERROR_INITIALIZATION_FAILED;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto previous = lastFrameNs.exchange(now, std::memory_order_relaxed);
    const double dt = previous > 0 ? std::min(1.0, static_cast<double>(now - previous) / 1'000'000'000.0) : 0.0;
    const auto frame = frameNumber.fetch_add(1, std::memory_order_relaxed) + 1;
    ProxyGuiLock guiLock(chain && graphicsPresentQueue);
    if (chain && graphicsPresentQueue)
        setProxyDisplaySize(static_cast<float>(chain->extent.width), static_cast<float>(chain->extent.height));
    if (!chain) reportRendererStatus("ImGui setup waiting: Vulkan layer did not match the presented swapchain");
    else if (!graphicsPresentQueue) reportRendererStatus("ImGui setup waiting: present queue is not the captured graphics queue");
    using FrameBridge = void (*)(double, std::uint64_t);
    static auto bridge = reinterpret_cast<FrameBridge>(dlsym(RTLD_DEFAULT, "wheatDispatchVulkanFrame"));
    if (bridge) bridge(dt, frame);
    if (chain && graphicsPresentQueue && !imguiBackendReady) initializeSwapchainRenderer(state, *chain);
    if (graphicsPresentQueue && imguiBackendReady && imguiBackendDevice == state.device) ImGui_ImplVulkan_NewFrame();
    if (chain && graphicsPresentQueue && info && info->pImageIndices && imguiBackendReady && imguiBackendDevice == state.device &&
        drawImGui(state, *chain, queue, info->pImageIndices[0], info)) {
        VkPresentInfoKHR modified = *info;
        modified.waitSemaphoreCount = 1;
        modified.pWaitSemaphores = &chain->renderSemaphores[info->pImageIndices[0]];
        guiLock.release();
        return state.nextPresent(queue, &modified);
    }
    guiLock.release();
    return state.nextPresent(queue, info);
}

VkResult VKAPI_CALL layerCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                         const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    const auto state = deviceState(device);
    const auto create = deviceFunction<PFN_vkCreateSwapchainKHR>(state, "vkCreateSwapchainKHR");
    if (!create || !info || !swapchain) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = create(device, info, allocator, swapchain);
    if (result == VK_SUCCESS) {
        SwapchainState value;
        value.device = device;
        value.swapchain = *swapchain;
        value.format = info->imageFormat;
        value.extent = info->imageExtent;
        value.imageUsage = info->imageUsage;
        value.minImageCount = info->minImageCount;
        std::lock_guard<std::mutex> lock(stateMutex);
        swapchains[handleKey(*swapchain)] = std::move(value);
    }
    return result;
}

void VKAPI_CALL layerDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
                                      const VkAllocationCallbacks* allocator) {
    const auto state = deviceState(device);
    auto destroy = deviceFunction<PFN_vkDestroySwapchainKHR>(state, "vkDestroySwapchainKHR");
    if (!destroy) return;
    SwapchainState resources;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        const auto found = swapchains.find(handleKey(swapchain));
        if (found != swapchains.end()) {
            resources = std::move(found->second);
            swapchains.erase(found);
        }
    }
    if (imguiBackendReady && imguiBackendDevice == device) {
        ImGui_ImplVulkan_Shutdown();
        imguiBackendReady = false;
        imguiBackendDevice = VK_NULL_HANDLE;
        setProxyRendererReady(false);
    }
    destroySwapchainResources(state, resources);
    destroy(device, swapchain, allocator);
}

void VKAPI_CALL layerDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    const auto state = deviceState(device);
    if (!state.nextGdpa) return;
    const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(state.nextGdpa(device, "vkDestroyDevice"));
    if (imguiBackendReady && imguiBackendDevice == device) {
        ImGui_ImplVulkan_Shutdown();
        imguiBackendReady = false;
        imguiBackendDevice = VK_NULL_HANDLE;
        setProxyRendererReady(false);
    }
    std::vector<SwapchainState> chains;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (auto it = swapchains.begin(); it != swapchains.end();) {
            if (it->second.device == device) {
                chains.push_back(std::move(it->second));
                it = swapchains.erase(it);
            } else ++it;
        }
    }
    for (auto& chain : chains) destroySwapchainResources(state, chain);
    if (destroy) destroy(device, allocator);
    std::lock_guard<std::mutex> lock(stateMutex);
    devices.erase(handleKey(device));
    for (auto it = queues.begin(); it != queues.end();) {
        if (it->second == handleKey(device)) {
            queueFamilies.erase(it->first);
            it = queues.erase(it);
        }
        else ++it;
    }
}

void VKAPI_CALL layerDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    const auto state = instanceState(instance);
    if (!state.nextGipa) return;
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(state.nextGipa(instance, "vkDestroyInstance"));
    if (destroy) destroy(instance, allocator);
    std::lock_guard<std::mutex> lock(stateMutex);
    instances.erase(handleKey(instance));
    for (auto it = physicalDevices.begin(); it != physicalDevices.end();) {
        if (it->second.instance == instance) it = physicalDevices.erase(it);
        else ++it;
    }
}

PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerGetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceProcAddr);
    if (std::strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerCreateInstance);
    if (std::strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerCreateDevice);
    if (!instance) return nullptr;
    if (std::strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerEnumeratePhysicalDevices);
    if (std::strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerDestroyInstance);
    const auto state = instanceState(instance);
    return state.nextGipa ? state.nextGipa(instance, name) : nullptr;
}

PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceProcAddr);
    const auto state = deviceState(device);
    if (!state.nextGdpa) return nullptr;
    if (std::strcmp(name, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceQueue);
    if (std::strcmp(name, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceQueue2);
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerCreateSwapchain);
    if (std::strcmp(name, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerDestroySwapchain);
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        auto present = reinterpret_cast<PFN_vkQueuePresentKHR>(state.nextGdpa(device, name));
        if (!present) return nullptr;
        std::lock_guard<std::mutex> lock(stateMutex);
        auto it = devices.find(handleKey(device));
        if (it != devices.end()) it->second.nextPresent = present;
        return reinterpret_cast<PFN_vkVoidFunction>(layerQueuePresent);
    }
    if (std::strcmp(name, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layerDestroyDevice);
    return state.nextGdpa(device, name);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        version->loaderLayerInterfaceVersion < 1)
        return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = std::min(
        version->loaderLayerInterfaceVersion,
        static_cast<std::uint32_t>(CURRENT_LOADER_LAYER_INTERFACE_VERSION));
    version->pfnGetInstanceProcAddr = layerGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = layerGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

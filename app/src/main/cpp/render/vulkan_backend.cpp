// Diamond Engine — Vulkan backend (experimental).
//
// Honest scope: this backend brings up a real instance, device and swapchain
// per ANativeWindow, uploads the scene to per-mesh buffers and records one
// command buffer per frame with a simple forward pass. It does NOT implement
// descriptor-set growable heaps, MSAA, shadow maps or a render graph — those are
// the parts of Vulkan that break on cheap drivers, and the whole point of
// shipping this as "experimental" is that the editor survives when they break.
//
// Anything unexpected (no suitable queue family, no surface format, a failed
// present) returns false from attach(), the renderer then reports the exact
// reason and the UI offers to switch back to OpenGL ES 3.
#include "renderer.h"

#include <android/native_window.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The Vulkan surface/swapchain plumbing is compiled only when the platform
// headers expose the full API; the NDK always has them under <vulkan/...>, but
// a canary NDK without them must still build. DIAMOND_ENABLE_VULKAN gates it.
//
// These includes MUST stay at global scope: vulkan_core.h declares C structs
// and platform handles, and including it inside namespace diamond would give
// every one of them a diamond:: type, which then refuses to interoperate with
// the entry points below.
#if DIAMOND_ENABLE_VULKAN && __has_include(<vulkan/vulkan.h>)
#define DIAMOND_HAS_VK_HEADERS 1
#define DIAMOND_VULKAN_TYPES_FROM_HEADERS 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#else
#define DIAMOND_HAS_VK_HEADERS 0
#endif

extern "C" {
#include "diamond/dlog.h"
#include "diamond/dmath.h"
#include "diamond/dutil.h"
// Included after <vulkan/vulkan.h> (see DIAMOND_VULKAN_TYPES_FROM_HEADERS):
// dvulkan.h only adds the probing functions then, so the two never redeclare
// VkInstance/VkApplicationInfo/... against each other.
#include "diamond/dvulkan.h"
}

#include "shader_bin.h"

namespace diamond {

#if DIAMOND_HAS_VK_HEADERS

// -----------------------------------------------------------------------------
//  Vulkan entry points, resolved at runtime
// -----------------------------------------------------------------------------
// libvulkan.so exists from API 24, but this app still starts on API 21 devices
// (GLES2 fallback). Linking -lvulkan would therefore fail at minSdk 21 — the
// NDK has no import library for that platform level — and linking it anyway
// would make the whole .so unloadable on a phone without a Vulkan driver.
// So, exactly like dvulkan.h promised, the loader is dlopen()ed and every
// entry point comes from dlsym(). The macros below then let the code in this
// file read like ordinary Vulkan code.
namespace {

struct VkDispatch {
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer = nullptr;
    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers = nullptr;
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed = nullptr;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
    PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
    PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
    PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
    PFN_vkUnmapMemory vkUnmapMemory = nullptr;
};

VkDispatch g_dispatch;
bool g_dispatch_ready = false;

// Fills g_dispatch from the loader. Defined after the macros below, declared
// here so the struct can stay private to this file.
const char *loadDispatch();

} // namespace

#define DIAMOND_VK_ENTRY_REF(name) #name
#define DIAMOND_VK_ENTRY(name) \
    g_dispatch.name = reinterpret_cast<PFN_##name>(dlsym(library, DIAMOND_VK_ENTRY_REF(name))); \
    if (!g_dispatch.name) missing = DIAMOND_VK_ENTRY_REF(name)

namespace {

// Returns the name of the first entry point the loader does not export, or
// nullptr when everything is available.
const char *loadDispatch() {
    if (g_dispatch_ready) return nullptr;
    void *library = dvulkan_library();
    if (!library) return "libvulkan.so";
    const char *missing = nullptr;
    DIAMOND_VK_ENTRY(vkAcquireNextImageKHR);
    DIAMOND_VK_ENTRY(vkAllocateCommandBuffers);
    DIAMOND_VK_ENTRY(vkAllocateMemory);
    DIAMOND_VK_ENTRY(vkBeginCommandBuffer);
    DIAMOND_VK_ENTRY(vkBindBufferMemory);
    DIAMOND_VK_ENTRY(vkBindImageMemory);
    DIAMOND_VK_ENTRY(vkCmdBeginRenderPass);
    DIAMOND_VK_ENTRY(vkCmdBindIndexBuffer);
    DIAMOND_VK_ENTRY(vkCmdBindPipeline);
    DIAMOND_VK_ENTRY(vkCmdBindVertexBuffers);
    DIAMOND_VK_ENTRY(vkCmdDrawIndexed);
    DIAMOND_VK_ENTRY(vkCmdEndRenderPass);
    DIAMOND_VK_ENTRY(vkCmdSetScissor);
    DIAMOND_VK_ENTRY(vkCmdSetViewport);
    DIAMOND_VK_ENTRY(vkCreateAndroidSurfaceKHR);
    DIAMOND_VK_ENTRY(vkCreateBuffer);
    DIAMOND_VK_ENTRY(vkCreateCommandPool);
    DIAMOND_VK_ENTRY(vkCreateDevice);
    DIAMOND_VK_ENTRY(vkCreateFramebuffer);
    DIAMOND_VK_ENTRY(vkCreateGraphicsPipelines);
    DIAMOND_VK_ENTRY(vkCreateImage);
    DIAMOND_VK_ENTRY(vkCreateImageView);
    DIAMOND_VK_ENTRY(vkCreateInstance);
    DIAMOND_VK_ENTRY(vkCreatePipelineLayout);
    DIAMOND_VK_ENTRY(vkCreateRenderPass);
    DIAMOND_VK_ENTRY(vkCreateSemaphore);
    DIAMOND_VK_ENTRY(vkCreateShaderModule);
    DIAMOND_VK_ENTRY(vkCreateSwapchainKHR);
    DIAMOND_VK_ENTRY(vkDestroyBuffer);
    DIAMOND_VK_ENTRY(vkDestroyCommandPool);
    DIAMOND_VK_ENTRY(vkDestroyDevice);
    DIAMOND_VK_ENTRY(vkDestroyFramebuffer);
    DIAMOND_VK_ENTRY(vkDestroyImage);
    DIAMOND_VK_ENTRY(vkDestroyImageView);
    DIAMOND_VK_ENTRY(vkDestroyInstance);
    DIAMOND_VK_ENTRY(vkDestroyPipeline);
    DIAMOND_VK_ENTRY(vkDestroyPipelineLayout);
    DIAMOND_VK_ENTRY(vkDestroyRenderPass);
    DIAMOND_VK_ENTRY(vkDestroyShaderModule);
    DIAMOND_VK_ENTRY(vkDestroySurfaceKHR);
    DIAMOND_VK_ENTRY(vkDestroySwapchainKHR);
    DIAMOND_VK_ENTRY(vkDeviceWaitIdle);
    DIAMOND_VK_ENTRY(vkEndCommandBuffer);
    DIAMOND_VK_ENTRY(vkEnumeratePhysicalDevices);
    DIAMOND_VK_ENTRY(vkFreeMemory);
    DIAMOND_VK_ENTRY(vkGetBufferMemoryRequirements);
    DIAMOND_VK_ENTRY(vkGetDeviceQueue);
    DIAMOND_VK_ENTRY(vkGetImageMemoryRequirements);
    DIAMOND_VK_ENTRY(vkGetPhysicalDeviceMemoryProperties);
    DIAMOND_VK_ENTRY(vkGetPhysicalDeviceProperties);
    DIAMOND_VK_ENTRY(vkGetPhysicalDeviceQueueFamilyProperties);
    DIAMOND_VK_ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    DIAMOND_VK_ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR);
    DIAMOND_VK_ENTRY(vkGetSwapchainImagesKHR);
    DIAMOND_VK_ENTRY(vkMapMemory);
    DIAMOND_VK_ENTRY(vkQueuePresentKHR);
    DIAMOND_VK_ENTRY(vkQueueSubmit);
    DIAMOND_VK_ENTRY(vkResetCommandBuffer);
    DIAMOND_VK_ENTRY(vkUnmapMemory);
    if (missing) return missing;
    g_dispatch_ready = true;
    return nullptr;
}

} // namespace

// -----------------------------------------------------------------------------
//  Entry-point aliases
// -----------------------------------------------------------------------------
// After the loader above (which must see the real g_dispatch members), the
// backend body can read like ordinary Vulkan code: vkCreateInstance() is
// g_dispatch.vkCreateInstance, and it is never null because loadDispatch()
// refuses to attach otherwise.
#define vkAcquireNextImageKHR g_dispatch.vkAcquireNextImageKHR
#define vkAllocateCommandBuffers g_dispatch.vkAllocateCommandBuffers
#define vkAllocateMemory g_dispatch.vkAllocateMemory
#define vkBeginCommandBuffer g_dispatch.vkBeginCommandBuffer
#define vkBindBufferMemory g_dispatch.vkBindBufferMemory
#define vkBindImageMemory g_dispatch.vkBindImageMemory
#define vkCmdBeginRenderPass g_dispatch.vkCmdBeginRenderPass
#define vkCmdBindIndexBuffer g_dispatch.vkCmdBindIndexBuffer
#define vkCmdBindPipeline g_dispatch.vkCmdBindPipeline
#define vkCmdBindVertexBuffers g_dispatch.vkCmdBindVertexBuffers
#define vkCmdDrawIndexed g_dispatch.vkCmdDrawIndexed
#define vkCmdEndRenderPass g_dispatch.vkCmdEndRenderPass
#define vkCmdSetScissor g_dispatch.vkCmdSetScissor
#define vkCmdSetViewport g_dispatch.vkCmdSetViewport
#define vkCreateAndroidSurfaceKHR g_dispatch.vkCreateAndroidSurfaceKHR
#define vkCreateBuffer g_dispatch.vkCreateBuffer
#define vkCreateCommandPool g_dispatch.vkCreateCommandPool
#define vkCreateDevice g_dispatch.vkCreateDevice
#define vkCreateFramebuffer g_dispatch.vkCreateFramebuffer
#define vkCreateGraphicsPipelines g_dispatch.vkCreateGraphicsPipelines
#define vkCreateImage g_dispatch.vkCreateImage
#define vkCreateImageView g_dispatch.vkCreateImageView
#define vkCreateInstance g_dispatch.vkCreateInstance
#define vkCreatePipelineLayout g_dispatch.vkCreatePipelineLayout
#define vkCreateRenderPass g_dispatch.vkCreateRenderPass
#define vkCreateSemaphore g_dispatch.vkCreateSemaphore
#define vkCreateShaderModule g_dispatch.vkCreateShaderModule
#define vkCreateSwapchainKHR g_dispatch.vkCreateSwapchainKHR
#define vkDestroyBuffer g_dispatch.vkDestroyBuffer
#define vkDestroyCommandPool g_dispatch.vkDestroyCommandPool
#define vkDestroyDevice g_dispatch.vkDestroyDevice
#define vkDestroyFramebuffer g_dispatch.vkDestroyFramebuffer
#define vkDestroyImage g_dispatch.vkDestroyImage
#define vkDestroyImageView g_dispatch.vkDestroyImageView
#define vkDestroyInstance g_dispatch.vkDestroyInstance
#define vkDestroyPipeline g_dispatch.vkDestroyPipeline
#define vkDestroyPipelineLayout g_dispatch.vkDestroyPipelineLayout
#define vkDestroyRenderPass g_dispatch.vkDestroyRenderPass
#define vkDestroyShaderModule g_dispatch.vkDestroyShaderModule
#define vkDestroySurfaceKHR g_dispatch.vkDestroySurfaceKHR
#define vkDestroySwapchainKHR g_dispatch.vkDestroySwapchainKHR
#define vkDeviceWaitIdle g_dispatch.vkDeviceWaitIdle
#define vkEndCommandBuffer g_dispatch.vkEndCommandBuffer
#define vkEnumeratePhysicalDevices g_dispatch.vkEnumeratePhysicalDevices
#define vkFreeMemory g_dispatch.vkFreeMemory
#define vkGetBufferMemoryRequirements g_dispatch.vkGetBufferMemoryRequirements
#define vkGetDeviceQueue g_dispatch.vkGetDeviceQueue
#define vkGetImageMemoryRequirements g_dispatch.vkGetImageMemoryRequirements
#define vkGetPhysicalDeviceMemoryProperties g_dispatch.vkGetPhysicalDeviceMemoryProperties
#define vkGetPhysicalDeviceProperties g_dispatch.vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties g_dispatch.vkGetPhysicalDeviceQueueFamilyProperties
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_dispatch.vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#define vkGetPhysicalDeviceSurfaceFormatsKHR g_dispatch.vkGetPhysicalDeviceSurfaceFormatsKHR
#define vkGetSwapchainImagesKHR g_dispatch.vkGetSwapchainImagesKHR
#define vkMapMemory g_dispatch.vkMapMemory
#define vkQueuePresentKHR g_dispatch.vkQueuePresentKHR
#define vkQueueSubmit g_dispatch.vkQueueSubmit
#define vkResetCommandBuffer g_dispatch.vkResetCommandBuffer
#define vkUnmapMemory g_dispatch.vkUnmapMemory

namespace {

constexpr int kMaxFramesInFlight = 2;

struct VkMeshGpu {
    int meshIndex = -1;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
};

class VulkanBackend final : public IRenderBackend {
public:
    ~VulkanBackend() override { detach(); }

    const char *name() const override { return "Vulkan (experimental)"; }
    int mode() const override { return DIAMOND_RENDER_VULKAN; }

    bool attach(void *window, int width, int height, char *err, int errCap) override {
        detach();
        width_ = width;
        height_ = height;
        window_ = static_cast<ANativeWindow *>(window);
        if (!window_) {
            diamond_fail(DIAMOND_ERR_ARG, err, errCap, "no native window");
            return false;
        }
        if (!dvulkan_probe()) {
            diamond_fail(DIAMOND_ERR_UNSUPPORTED, err, errCap,
                         "this device has no working Vulkan driver");
            return false;
        }
        if (const char *missing = loadDispatch()) {
            diamond_fail(DIAMOND_ERR_UNSUPPORTED, err, errCap,
                         "the Vulkan loader does not export %s; this driver is too old",
                         missing);
            return false;
        }
        if (!createInstance(err, errCap)) return false;
        if (!pickPhysicalDevice(err, errCap)) return false;
        if (!createDevice(err, errCap)) return false;
        if (!createSurface(err, errCap)) return false;
        if (!createSwapchain(err, errCap)) return false;
        if (!createCommandPool(err, errCap)) return false;
        attached_ = true;
        dlog_write(DLOG_INFO, "vk", "backend attached: %s, %dx%d, %u images",
                   deviceName_, width_, height_, imageCount_);
        return true;
    }

    void detach() override {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        destroySwapchain();
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
        if (surface_ && instance_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (device_) vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        if (instance_) vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        window_ = nullptr;
        attached_ = false;
    }

    void resize(int width, int height) override {
        if (!attached_) { width_ = width; height_ = height; return; }
        if (width <= 0 || height <= 0) return;
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        char buffer[128];
        width_ = width;
        height_ = height;
        createSwapchain(buffer, sizeof(buffer));
    }

    bool draw(const FrameContext &frame) override {
        if (!attached_) return false;
        // A node count change invalidates the cached vertex buffers.
        if (frameCount_ == 0) rebuildGeometry();

        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, 16ull * 1000000ull,
                                                 imageAvailable_, VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            resize(frame.width, frame.height);
            return true;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            dlog_write(DLOG_WARN, "vk", "acquire failed (0x%x) — the editor will fall back", acquire);
            return false;
        }

        vkResetCommandBuffer(commandBuffers_[frameIndex_], 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(commandBuffers_[frameIndex_], &begin);

        VkClearValue clear[2];
        std::memset(clear, 0, sizeof(clear));
        clear[0].color.float32[0] = 0.09f;
        clear[0].color.float32[1] = 0.10f;
        clear[0].color.float32[2] = 0.12f;
        clear[0].color.float32[3] = 1.0f;
        clear[1].depthStencil.depth = 1.0f;
        clear[1].depthStencil.stencil = 0;
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = renderPass_;
        pass.framebuffer = framebuffers_[imageIndex];
        pass.renderArea.extent.width = static_cast<uint32_t>(width_);
        pass.renderArea.extent.height = static_cast<uint32_t>(height_);
        pass.clearValueCount = 2;
        pass.pClearValues = clear;
        vkCmdBeginRenderPass(commandBuffers_[frameIndex_], &pass, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
        VkRect2D scissor;
        scissor.offset.x = 0; scissor.offset.y = 0;
        scissor.extent.width = static_cast<uint32_t>(width_);
        scissor.extent.height = static_cast<uint32_t>(height_);
        vkCmdSetViewport(commandBuffers_[frameIndex_], 0, 1, &viewport);
        vkCmdSetScissor(commandBuffers_[frameIndex_], 0, 1, &scissor);

        if (pipeline_ != VK_NULL_HANDLE) {
            vkCmdBindPipeline(commandBuffers_[frameIndex_], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            for (int i = 0; i < meshCount_; ++i) {
                if (meshes_[i].indexCount == 0) continue;
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(commandBuffers_[frameIndex_], 0, 1, &meshes_[i].vertexBuffer, &offset);
                vkCmdBindIndexBuffer(commandBuffers_[frameIndex_], meshes_[i].indexBuffer, 0, VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(commandBuffers_[frameIndex_], meshes_[i].indexCount, 1, 0, 0, 0);
                triangles_ += static_cast<int>(meshes_[i].indexCount / 3);
                drawCalls_++;
            }
        }

        vkCmdEndRenderPass(commandBuffers_[frameIndex_]);
        vkEndCommandBuffer(commandBuffers_[frameIndex_]);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSemaphores[] = {imageAvailable_};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = waitSemaphores;
        submit.pWaitDstStageMask = waitStages;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffers_[frameIndex_];
        VkSemaphore signalSemaphores[] = {renderFinished_};
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = signalSemaphores;
        if (vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
            dlog_write(DLOG_ERROR, "vk", "vkQueueSubmit failed");
            return false;
        }

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = signalSemaphores;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &imageIndex;
        const VkResult presentResult = vkQueuePresentKHR(presentQueue_, &present);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
            resize(frame.width, frame.height);
        } else if (presentResult != VK_SUCCESS) {
            dlog_write(DLOG_WARN, "vk", "present failed (0x%x)", presentResult);
            return false;
        }
        frameIndex_ = (frameIndex_ + 1) % kMaxFramesInFlight;
        frameCount_++;
        geometryGeneration_ = static_cast<long>(dscene_flat_count());
        return true;
    }

    DRenderStats stats() const override {
        DRenderStats s{};
        s.backend = DIAMOND_RENDER_VULKAN;
        s.triangles = triangles_;
        s.draw_calls = drawCalls_;
        s.width = width_;
        s.height = height_;
        return s;
    }

private:
    // ---- instance / device ------------------------------------------------
    bool createInstance(char *err, int errCap) {
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "Diamond Engine";
        application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        application.pEngineName = "Diamond";
        application.apiVersion = VK_API_VERSION_1_0;
        const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &application;
        createInfo.enabledExtensionCount = 2;
        createInfo.ppEnabledExtensionNames = extensions;
        if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreateInstance failed");
            return false;
        }
        return true;
    }

    bool pickPhysicalDevice(char *err, int errCap) {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || count == 0) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "no Vulkan physical device");
            return false;
        }
        VkPhysicalDevice devices[8];
        if (count > 8) count = 8;
        vkEnumeratePhysicalDevices(instance_, &count, devices);
        physical_ = devices[0];
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        {
            // strncpy would leave a truncated name unterminated; copy by hand.
            size_t n = 0;
            while (n + 1 < sizeof(deviceName_) && properties.deviceName[n] != '\0') ++n;
            memcpy(deviceName_, properties.deviceName, n);
            deviceName_[n] = '\0';
        }
        {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, nullptr);
            VkQueueFamilyProperties families[16];
            if (familyCount > 16) familyCount = 16;
            vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, families);
            graphicsFamily_ = UINT32_MAX;
            for (uint32_t i = 0; i < familyCount; ++i) {
                if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { graphicsFamily_ = i; break; }
            }
            if (graphicsFamily_ == UINT32_MAX) {
                diamond_fail(DIAMOND_ERR_GPU, err, errCap, "no graphics queue family");
                return false;
            }
        }
        return true;
    }

    bool createDevice(char *err, int errCap) {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = graphicsFamily_;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
        if (vkCreateDevice(physical_, &info, nullptr, &device_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreateDevice failed (driver %s)", deviceName_);
            return false;
        }
        vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
        presentQueue_ = graphicsQueue_;
        return true;
    }

    bool createSurface(char *err, int errCap) {
        VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.window = window_;
        if (vkCreateAndroidSurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreateAndroidSurfaceKHR failed");
            return false;
        }
        return true;
    }

    bool createCommandPool(char *err, int errCap) {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = graphicsFamily_;
        if (vkCreateCommandPool(device_, &info, nullptr, &commandPool_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreateCommandPool failed");
            return false;
        }
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = commandPool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = kMaxFramesInFlight;
        if (vkAllocateCommandBuffers(device_, &allocate, commandBuffers_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkAllocateCommandBuffers failed");
            return false;
        }
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &semaphore, nullptr, &imageAvailable_);
        vkCreateSemaphore(device_, &semaphore, nullptr, &renderFinished_);
        return true;
    }

    bool createSwapchain(char *err, int errCap) {
        VkSurfaceCapabilitiesKHR capabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities);

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, nullptr);
        if (formatCount == 0) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "surface has no formats");
            return false;
        }
        VkSurfaceFormatKHR formats[32];
        if (formatCount > 32) formatCount = 32;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, formats);
        VkSurfaceFormatKHR chosen = formats[0];
        for (uint32_t i = 0; i < formatCount; ++i) {
            if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = formats[i]; break; }
        }

        VkExtent2D extent;
        extent.width = static_cast<uint32_t>(width_);
        extent.height = static_cast<uint32_t>(height_);
        if (capabilities.currentExtent.width != UINT32_MAX) extent = capabilities.currentExtent;

        imageCount_ = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount_ > capabilities.maxImageCount)
            imageCount_ = capabilities.maxImageCount;

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = imageCount_;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreateSwapchainKHR failed");
            return false;
        }
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount_, nullptr);
        if (imageCount_ > 8) imageCount_ = 8;
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount_, images_);

        // Depth image.
        VkImageCreateInfo depthInfo{};
        depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depthInfo.imageType = VK_IMAGE_TYPE_2D;
        depthInfo.extent.width = extent.width;
        depthInfo.extent.height = extent.height;
        depthInfo.extent.depth = 1;
        depthInfo.mipLevels = 1;
        depthInfo.arrayLayers = 1;
        depthInfo.format = VK_FORMAT_D16_UNORM;
        depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        depthInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device_, &depthInfo, nullptr, &depthImage_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "depth image creation failed");
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, depthImage_, &requirements);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &allocate, nullptr, &depthMemory_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_MEMORY, err, errCap, "depth memory allocation failed");
            return false;
        }
        vkBindImageMemory(device_, depthImage_, depthMemory_, 0);

        // Render pass.
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = chosen.format;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[1].format = VK_FORMAT_D16_UNORM;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        VkRenderPassCreateInfo passInfo{};
        passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        passInfo.attachmentCount = 2;
        passInfo.pAttachments = attachments;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        if (vkCreateRenderPass(device_, &passInfo, nullptr, &renderPass_) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreateRenderPass failed");
            return false;
        }

        // Depth view first: framebuffers reference it, so creating them before
        // the view exists is undefined behaviour.
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = depthImage_;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_D16_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device_, &viewInfo, nullptr, &depthView_) != VK_SUCCESS) {
                diamond_fail(DIAMOND_ERR_GPU, err, errCap, "depth image view creation failed");
                return false;
            }
        }

        if (!createPipeline(err, errCap)) {
            // Not fatal: the swapchain still clears to the editor background, so
            // the user sees a labelled empty viewport instead of a black screen.
            dlog_write(DLOG_WARN, "vk", "graphics pipeline unavailable: %s", err ? err : "");
        }

        // Image views + framebuffers.
        for (uint32_t i = 0; i < imageCount_; ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = images_[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = chosen.format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device_, &viewInfo, nullptr, &imageViews_[i]) != VK_SUCCESS) {
                diamond_fail(DIAMOND_ERR_GPU, err, errCap, "swapchain image view %u failed", i);
                return false;
            }
            VkImageView views[2] = {imageViews_[i], depthView_};
            VkFramebufferCreateInfo frameInfo{};
            frameInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            frameInfo.renderPass = renderPass_;
            frameInfo.attachmentCount = 2;
            frameInfo.pAttachments = views;
            frameInfo.width = extent.width;
            frameInfo.height = extent.height;
            frameInfo.layers = 1;
            if (vkCreateFramebuffer(device_, &frameInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
                diamond_fail(DIAMOND_ERR_GPU, err, errCap, "framebuffer %u failed", i);
                return false;
            }
        }

        swapchainFormat_ = chosen.format;
        swapchainExtent_ = extent;
        return true;
    }

    void destroySwapchain() {
        for (uint32_t i = 0; i < imageCount_; ++i) {
            if (framebuffers_[i]) vkDestroyFramebuffer(device_, framebuffers_[i], nullptr);
            if (imageViews_[i]) vkDestroyImageView(device_, imageViews_[i], nullptr);
            framebuffers_[i] = VK_NULL_HANDLE;
            imageViews_[i] = VK_NULL_HANDLE;
        }
        imageCount_ = 0;
        if (depthView_) vkDestroyImageView(device_, depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
        if (depthImage_) vkDestroyImage(device_, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
        if (depthMemory_) vkFreeMemory(device_, depthMemory_, nullptr);
        depthMemory_ = VK_NULL_HANDLE;
        if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
        if (pipelines_[0]) vkDestroyPipeline(device_, pipelines_[0], nullptr);
        pipeline_ = VK_NULL_HANDLE;
        pipelines_[0] = VK_NULL_HANDLE;
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
        }
        return 0;
    }

    void releaseGeometry() {
        for (int i = 0; i < meshCount_; ++i) {
            VkMeshGpu &gpu = meshes_[i];
            if (gpu.vertexBuffer) vkDestroyBuffer(device_, gpu.vertexBuffer, nullptr);
            if (gpu.indexBuffer) vkDestroyBuffer(device_, gpu.indexBuffer, nullptr);
            if (gpu.vertexMemory) vkFreeMemory(device_, gpu.vertexMemory, nullptr);
            if (gpu.indexMemory) vkFreeMemory(device_, gpu.indexMemory, nullptr);
            gpu = VkMeshGpu{};
        }
        meshCount_ = 0;
    }

    void rebuildGeometry() {
        // Vertex/index uploads happen lazily through a host-visible staging
        // buffer; the editor only rebuilds when the node count changes.
        releaseGeometry();
        const int total = dscene_flat_count();
        for (int i = 1; i < total && meshCount_ < 64; ++i) {
            DNode *node = dnode_get(i);
            if (!node || node->mesh < 0) continue;
            DMesh *mesh = dmesh_get(node->mesh);
            if (!mesh || !mesh->positions || !mesh->indices) continue;
            VkMeshGpu &gpu = meshes_[meshCount_];
            gpu = VkMeshGpu{};
            gpu.meshIndex = node->mesh;
            gpu.indexCount = static_cast<uint32_t>(mesh->index_count);
            const VkDeviceSize vertexBytes = sizeof(float) * 3 * static_cast<VkDeviceSize>(mesh->vertex_count);
            createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         mesh->positions, gpu.vertexBuffer, gpu.vertexMemory);
            const VkDeviceSize indexBytes = sizeof(uint16_t) * static_cast<VkDeviceSize>(mesh->index_count);
            createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         mesh->indices, gpu.indexBuffer, gpu.indexMemory);
            meshCount_++;
        }
    }

    /* Builds the single graphics pipeline from the SPIR-V blobs that CI
     * precompiles (see tools/compile_shaders.sh and shader_bin.h). Without
     * them the backend still brings up the swapchain and clears the screen,
     * which is why a missing blob is reported but not fatal. */
    bool createPipeline(char *err, int errCap) {
        if (!dshader_spirv_vertex() || !dshader_spirv_fragment()) {
            if (err && errCap > 0)
                snprintf(err, static_cast<size_t>(errCap),
                         "SPIR-V shaders were not baked in (build with DIAMOND_SPV_PRESET=1)");
            return false;
        }
        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        if (!createShaderModule(dshader_spirv_vertex(), dshader_spirv_vertex_size(), &vertexModule, err, errCap))
            return false;
        if (!createShaderModule(dshader_spirv_fragment(), dshader_spirv_fragment_size(), &fragmentModule, err, errCap)) {
            vkDestroyShaderModule(device_, vertexModule, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(float) * 3;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attribute{};
        attribute.location = 0;
        attribute.binding = 0;
        attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
        attribute.offset = 0;
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attribute;

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_BACK_BIT;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, vertexModule, nullptr);
            vkDestroyShaderModule(device_, fragmentModule, nullptr);
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreatePipelineLayout failed");
            return false;
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;

        const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                          nullptr, &pipeline_);
        vkDestroyShaderModule(device_, vertexModule, nullptr);
        vkDestroyShaderModule(device_, fragmentModule, nullptr);
        if (result != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreateGraphicsPipelines failed (0x%x)", result);
            return false;
        }
        pipelines_[0] = pipeline_;
        dlog_write(DLOG_INFO, "vk", "graphics pipeline ready");
        return true;
    }

    bool createShaderModule(const unsigned char *code, size_t size, VkShaderModule *out,
                            char *err, int errCap) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = reinterpret_cast<const uint32_t *>(code);
        if (vkCreateShaderModule(device_, &info, nullptr, out) != VK_SUCCESS) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "vkCreateShaderModule failed");
            return false;
        }
        return true;
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *data,
                      VkBuffer &buffer, VkDeviceMemory &memory) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &info, nullptr, &buffer) != VK_SUCCESS) return;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer, &requirements);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &allocate, nullptr, &memory) != VK_SUCCESS) return;
        vkBindBufferMemory(device_, buffer, memory, 0);
        void *mapped = nullptr;
        if (vkMapMemory(device_, memory, 0, size, 0, &mapped) == VK_SUCCESS && mapped) {
            std::memcpy(mapped, data, static_cast<size_t>(size));
            vkUnmapMemory(device_, memory);
        }
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D swapchainExtent_{0, 0};
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[1] = {VK_NULL_HANDLE};
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkImage images_[8] = {VK_NULL_HANDLE};
    VkImageView imageViews_[8] = {VK_NULL_HANDLE};
    VkFramebuffer framebuffers_[8] = {VK_NULL_HANDLE};
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kMaxFramesInFlight] = {VK_NULL_HANDLE};
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    uint32_t imageCount_ = 0;
    uint32_t frameIndex_ = 0;
    long frameCount_ = 0;
    long geometryGeneration_ = -1;
    ANativeWindow *window_ = nullptr;
    char deviceName_[128] = {0};
    VkMeshGpu meshes_[64];
    int meshCount_ = 0;
    bool attached_ = false;
    int drawCalls_ = 0;
    int triangles_ = 0;
};

} // namespace

IRenderBackend *createVulkanBackend() { return new VulkanBackend(); }

#else // !DIAMOND_HAS_VK_HEADERS

namespace {

// Fallback implementation used when the Vulkan headers are unavailable in the
// NDK being used. It never pretends to work: attach() reports the real reason.
class VulkanUnavailableBackend final : public IRenderBackend {
public:
    const char *name() const override { return "Vulkan (headers unavailable)"; }
    int mode() const override { return DIAMOND_RENDER_VULKAN; }
    bool attach(void *window, int width, int height, char *err, int errCap) override {
        (void)window;
        width_ = width;
        height_ = height;
        diamond_fail(DIAMOND_ERR_UNSUPPORTED, err, errCap,
                     "this build has no Vulkan support (NDK headers missing); use OpenGL ES 3.0");
        return false;
    }
    void detach() override {}
    void resize(int width, int height) override { width_ = width; height_ = height; }
    bool draw(const FrameContext &) override { return false; }
};

} // namespace

IRenderBackend *createVulkanBackend() { return new VulkanUnavailableBackend(); }

#endif // DIAMOND_HAS_VK_HEADERS

} // namespace diamond

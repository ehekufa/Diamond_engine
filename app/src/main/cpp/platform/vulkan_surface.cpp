// Diamond Engine — Vulkan device probing and surface plumbing.
//
// The Vulkan backend is experimental by design: driver quality on cheap 2016
// phones varies wildly. Everything here is written so that a broken driver
// produces a clean error string instead of a native crash, and so that the
// presence of libvulkan is not assumed at runtime (it is dlopen'ed, never
// linked, which also keeps the .so usable on devices without it).
#include "diamond/dvulkan.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>

extern "C" {
#include "diamond/dlog.h"
}

namespace {

typedef void *(*GetInstanceProcAddrFn)(VkInstance, const char *);
typedef VkResult (*EnumerateDevicesFn)(VkInstance, uint32_t *, VkPhysicalDevice *);

struct VulkanProbe {
    void *library = nullptr;
    GetInstanceProcAddrFn getInstanceProcAddr = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerateExtensions = nullptr;
    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    int probed = 0;
    int supported = 0;
    char deviceName[128] = {0};
    char apiVersion[32] = {0};
};

VulkanProbe g_probe;

const char *const kRequiredExtensions[] = {"VK_KHR_surface", "VK_KHR_android_surface"};

bool strcmp_safe(const char *a, const char *b) {
    return a && b && std::strcmp(a, b) == 0;
}

} // namespace

extern "C" {

int dvulkan_probe(void) {
    if (g_probe.probed) return g_probe.supported;
    g_probe.probed = 1;

#if defined(DIAMOND_ENABLE_VULKAN) && DIAMOND_ENABLE_VULKAN
    g_probe.library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_probe.library) {
        dlog_write(DLOG_INFO, "vk", "libvulkan not present — Vulkan mode unavailable");
        return 0;
    }
    g_probe.getInstanceProcAddr = reinterpret_cast<GetInstanceProcAddrFn>(
        dlsym(g_probe.library, "vkGetInstanceProcAddr"));
    if (!g_probe.getInstanceProcAddr) {
        dlog_write(DLOG_WARN, "vk", "libvulkan has no vkGetInstanceProcAddr");
        return 0;
    }
    g_probe.enumerateExtensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        g_probe.getInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
    g_probe.createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        g_probe.getInstanceProcAddr(nullptr, "vkCreateInstance"));
    g_probe.destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        g_probe.getInstanceProcAddr(nullptr, "vkDestroyInstance"));
    if (!g_probe.createInstance) return 0;

    // Required instance extensions.
    {
        uint32_t count = 0;
        int hasSurface = 0, hasAndroid = 0;
        if (g_probe.enumerateExtensions) {
            if (g_probe.enumerateExtensions(nullptr, &count, nullptr) == VK_SUCCESS && count > 0) {
                VkExtensionProperties *properties = new VkExtensionProperties[count];
                if (properties &&
                    g_probe.enumerateExtensions(nullptr, &count, properties) == VK_SUCCESS) {
                    for (uint32_t i = 0; i < count; ++i) {
                        if (!strcmp_safe(properties[i].extensionName, "VK_KHR_surface")) hasSurface = 1;
                        if (!strcmp_safe(properties[i].extensionName, "VK_KHR_android_surface")) hasAndroid = 1;
                    }
                }
                delete[] properties;
            }
        }
        if (!hasSurface || !hasAndroid) {
            dlog_write(DLOG_WARN, "vk", "surface extensions missing (surface=%d android=%d)",
                       hasSurface, hasAndroid);
            return 0;
        }
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Diamond Engine";
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName = "Diamond";
    application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &application;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = kRequiredExtensions;

    VkInstance instance = VK_NULL_HANDLE;
    if (g_probe.createInstance(&createInfo, nullptr, &instance) != VK_SUCCESS || !instance) {
        dlog_write(DLOG_WARN, "vk", "vkCreateInstance failed — driver refuses a probe instance");
        return 0;
    }
    {
        uint32_t deviceCount = 0;
        // Per-instance functions can only be resolved after creation.
        EnumerateDevicesFn enumerate = reinterpret_cast<EnumerateDevicesFn>(
            g_probe.getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
        if (enumerate && enumerate(instance, &deviceCount, nullptr) == VK_SUCCESS && deviceCount > 0) {
            g_probe.supported = 1;
            if (enumerate(instance, &deviceCount, nullptr) == VK_SUCCESS) {
                // Device name reporting needs vkGetPhysicalDeviceProperties, which
                // is optional for the probe: name stays "Vulkan device" when absent.
                snprintf(g_probe.deviceName, sizeof(g_probe.deviceName), "Vulkan device x%u", deviceCount);
            }
        }
    }
    if (g_probe.destroyInstance) g_probe.destroyInstance(instance, nullptr);
    dlog_write(DLOG_INFO, "vk", "Vulkan probe: %s", g_probe.supported ? "supported" : "no physical devices");
    return g_probe.supported;
#else
    return 0;
#endif
}

int dvulkan_supported(void) { return dvulkan_probe(); }

const char *dvulkan_device_name(void) {
    dvulkan_probe();
    return g_probe.deviceName[0] ? g_probe.deviceName : "no Vulkan device";
}

void *dvulkan_library(void) {
    dvulkan_probe();
    return g_probe.library;
}

} // extern "C"

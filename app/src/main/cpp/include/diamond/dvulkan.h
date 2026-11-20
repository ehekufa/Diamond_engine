/* Diamond Engine — Vulkan probing (implemented in platform/vulkan_surface.cpp).
 *
 * The Vulkan headers are NOT required: the backend talks to the loader through
 * a minimal, self-declared ABI so the build does not depend on a particular
 * NDK Vulkan header revision (they keep changing between canary NDKs).
 */
#ifndef DIAMOND_DVULKAN_H
#define DIAMOND_DVULKAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void *VkDevice;
typedef void *VkSurfaceKHR;
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef int32_t VkResult;

#define VK_SUCCESS 0
#define VK_NULL_HANDLE 0
#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
/* Only declare the two macros when the real Vulkan headers are not in play:
 * this file is a self-contained ABI double for probing, and vulkan.h defines
 * the same names. */
#ifndef VK_MAKE_VERSION
#define VK_MAKE_VERSION(major, minor, patch) \
    ((((uint32_t)(major)) << 22) | (((uint32_t)(minor)) << 12) | ((uint32_t)(patch)))
#endif
#ifndef VK_API_VERSION_1_0
#define VK_API_VERSION_1_0 VK_MAKE_VERSION(1, 0, 0)
#endif

typedef struct VkApplicationInfo {
    int sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    int sType;
    const void *pNext;
    VkFlags flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkExtensionProperties {
    char extensionName[256];
    uint32_t specVersion;
} VkExtensionProperties;

typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo *, const void *, VkInstance *);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void *);
typedef void *(*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(const char *, uint32_t *, VkExtensionProperties *);

/* 1 when libvulkan exists and at least one physical device enumerates. */
int  dvulkan_probe(void);
int  dvulkan_supported(void);
const char *dvulkan_device_name(void);
void *dvulkan_library(void);

#ifdef __cplusplus
}
#endif
#endif

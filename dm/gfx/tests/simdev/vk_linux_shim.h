// t_vkdev on desktop Linux (x86-64, WSL): nvn_vk.c is the Android / MoltenVK engine; these are the few declarations it takes from Android's headers that a Linux build
//   lacks. Forced in with -include ahead of nvn_vk.c; nothing here changes what the engine does (the Android window path is never reached in the test).
#ifndef VK_LINUX_SHIM_H
#define VK_LINUX_SHIM_H
#define VK_NO_PROTOTYPES   // (as nvn_vk.c: every Vulkan function from the dispatch table)
#include <vulkan/vulkan.h>
#ifndef VK_KHR_android_surface
typedef VkFlags VkAndroidSurfaceCreateFlagsKHR;
typedef struct VkAndroidSurfaceCreateInfoKHR { VkStructureType sType; const void *pNext; VkAndroidSurfaceCreateFlagsKHR flags; struct ANativeWindow *window; } VkAndroidSurfaceCreateInfoKHR;
typedef VkResult (VKAPI_PTR *PFN_vkCreateAndroidSurfaceKHR)(VkInstance, const VkAndroidSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
#endif
#endif

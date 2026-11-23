// PC stand-in (dm/pc/compat), included first (-include) in every file of the x86-64 Linux build of the NVN replay layer (dm/pc/Makefile): what the Android build gets
// from dm/android/compat/darwin.h (Darwin-only libc extras) without its arm64 parts, plus the Android Vulkan types vk_dispatch.h names. glibc, any Vulkan loader.
#pragma once
#define _GNU_SOURCE 1
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/param.h>
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC
static inline int pthread_threadid_np(pthread_t t, uint64_t *id) { (void)t; *id = (uint64_t)gettid(); return 0; }
static inline int pthread_main_np(void) { return getpid() == gettid(); }
// vk_dispatch.h lists vkCreateAndroidSurfaceKHR among the instance functions: its types come from vulkan_android.h, which only forward-declares the Android structs.
// VK_NO_PROTOTYPES as nvn_vk.c sets it (every Vulkan function is a pointer the replay loads itself)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

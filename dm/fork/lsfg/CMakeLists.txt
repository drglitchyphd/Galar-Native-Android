# FORK: libforklsfg.so (fork_lsfg.h), built by scripts/build_fork.sh with the NDK's CMake toolchain (arm64-v8a, C++ runtime linked in statically).
# Sources fetched by build_fork.sh at pinned commits into $WORK/fork-lsfg (never copied into this repository):
#   LSFG_SRC      FrankBarretta/lsfg-vk-android (MIT): framegen/, src/extract/, thirdparty/pe-parse (MIT), thirdparty/volk (MIT)
#   DXBC_SRC      exyorha/dxbc (zlib, from DXVK): DXBC to SPIR-V
#   SPIRV_HEADERS KhronosGroup/SPIRV-Headers (MIT-style)
cmake_minimum_required(VERSION 3.20)
project(forklsfg LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
foreach(v LSFG_SRC DXBC_SRC SPIRV_HEADERS)
    if(NOT ${v})
        message(FATAL_ERROR "${v} is not set")
    endif()
endforeach()

add_compile_definitions(VK_USE_PLATFORM_ANDROID_KHR)   # (the AHardwareBuffer types in vulkan_android.h, for framegen and volk)
# volk: Vulkan through dlopen("libvulkan.so"), the phone's own loader (framegen runs its own instance and device)
add_subdirectory(${LSFG_SRC}/thirdparty/volk volk EXCLUDE_FROM_ALL)
# pe-parse: reads Lossless.dll's resources
add_subdirectory(${LSFG_SRC}/thirdparty/pe-parse/pe-parser-library pe-parse EXCLUDE_FROM_ALL)
# framegen: LSFG 3.1 and 3.1 performance
add_subdirectory(${LSFG_SRC}/framegen framegen EXCLUDE_FROM_ALL)

# dxbc (its own CMake wants a desktop Vulkan SDK; the sources are listed here instead)
file(GLOB DXBC_SOURCES ${DXBC_SRC}/src/dxbc/*.cpp ${DXBC_SRC}/src/spirv/*.cpp ${DXBC_SRC}/src/util/*.cpp ${DXBC_SRC}/src/util/*/*.cpp)
add_library(forkdxbc STATIC ${DXBC_SOURCES})
target_include_directories(forkdxbc SYSTEM PUBLIC ${DXBC_SRC}/src ${DXBC_SRC}/src/dxbc ${DXBC_SRC}/src/util ${DXBC_SRC}/src/spirv ${DXBC_SRC}/include/windows ${SPIRV_HEADERS}/include)
target_compile_options(forkdxbc PRIVATE -fexceptions -w)

add_library(forklsfg SHARED fork_lsfg.cpp fork_trans.cpp ${LSFG_SRC}/src/extract/extract.cpp)
target_include_directories(forklsfg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${LSFG_SRC}/include ${SPIRV_HEADERS}/include)
target_link_libraries(forklsfg PRIVATE lsfg-vk-framegen pe-parse forkdxbc android log)
target_compile_options(forklsfg PRIVATE -fexceptions -Wno-unused-parameter)
target_link_options(forklsfg PRIVATE -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL)

cmake_minimum_required(VERSION 3.24)

project(RT2DlssRRProbe LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The A0 DLSS Ray Reconstruction probe is Windows-only.")
endif()

find_package(Vulkan REQUIRED)

set(RT2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(NGX_ROOT "${RT2_ROOT}/RT2App/vendor/DLSS")
set(NGX_INCLUDE "${NGX_ROOT}/include")
set(NGX_RELEASE_LIBRARY "${NGX_ROOT}/lib/Windows_x86_64/khr/x64/nvsdk_ngx_khr_d.lib")
set(NGX_DEBUG_LIBRARY "${NGX_ROOT}/lib/Windows_x86_64/khr/x64/nvsdk_ngx_khr_d_dbg.lib")
set(NGX_RUNTIME_DIR "${NGX_ROOT}/lib/Windows_x86_64/rel")

foreach(required_path
        "${NGX_INCLUDE}/nvsdk_ngx_vk.h"
        "${NGX_INCLUDE}/nvsdk_ngx_helpers_dlssd_vk.h"
        "${NGX_RELEASE_LIBRARY}"
        "${NGX_DEBUG_LIBRARY}"
        "${NGX_RUNTIME_DIR}/nvngx_dlss.dll"
        "${NGX_RUNTIME_DIR}/nvngx_dlssd.dll")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Pinned NGX SDK payload is incomplete: ${required_path}")
    endif()
endforeach()

add_executable(RT2DlssRRProbe main.cpp)
target_compile_features(RT2DlssRRProbe PRIVATE cxx_std_20)
target_compile_definitions(RT2DlssRRProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_USE_PLATFORM_WIN32_KHR)
target_include_directories(RT2DlssRRProbe PRIVATE "${NGX_INCLUDE}")
target_link_libraries(RT2DlssRRProbe PRIVATE
    Vulkan::Vulkan
    optimized "${NGX_RELEASE_LIBRARY}"
    debug "${NGX_DEBUG_LIBRARY}")

if(MSVC)
    target_compile_options(RT2DlssRRProbe PRIVATE /W4 /permissive- /EHsc)
endif()

foreach(runtime_dll nvngx_dlss.dll nvngx_dlssd.dll)
    add_custom_command(TARGET RT2DlssRRProbe POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NGX_RUNTIME_DIR}/${runtime_dll}"
            "$<TARGET_FILE_DIR:RT2DlssRRProbe>/${runtime_dll}"
        VERBATIM)
endforeach()

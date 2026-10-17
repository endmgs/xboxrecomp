# Xbox D3D8 Compatibility Layer
#
# The renderer: the Xbox D3D8 device, its formats, swizzle, palettes, and
# the vertex program and register combiner translators, all drawing through
# rhi.h. The same sources build everywhere; what differs is the backend
# behind rhi.h:
#   Windows -> Direct3D 11 (the default) and Vulkan (RECOMP_D3D8_BACKEND=vulkan)
#   POSIX   -> Vulkan (the default and only one; MoltenVK on macOS)
# docs/technical/vulkan-backend.md is the design.

set(XBR_D3D8_SOURCES
    d3d8_device.c
    d3d8_resources.c
    d3d8_shaders.c
    d3d8_states.c
    d3d8_combiners.c
    d3d8_vsh.c
    d3d8_overlay.c
    d3d8_subtitle.c
    d3d8_movie.c
    d3d8_screencopy.c
    d3d8_clear.c
    d3d8_display.c
    d3d8_compile.c
    rhi.c
    rhi_shader_cache.c
)

if(WIN32)
    add_library(xbox_d3d8 STATIC ${XBR_D3D8_SOURCES} rhi_d3d11.c)
    # gdi32: the overlay renders its text with GDI rather than an embedded font.
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler gdi32)
else()
    # No backend of its own: without the Vulkan pieces below this still
    # links (a title then runs with RECOMP_HLE_D3D8=off, or with no
    # picture), so nothing above it has to know.
    add_library(xbox_d3d8 STATIC ${XBR_D3D8_SOURCES})
    target_link_libraries(xbox_d3d8 PUBLIC platform ${CMAKE_DL_LIBS})
endif()

# The Vulkan backend, built when the vendored headers are checked out
# (git submodule update --init) and DXC's headers are found. Nothing links a
# Vulkan loader or DXC: volk finds the loader and rhi_vulkan_dxc.cpp finds
# dxcompiler at run time, so an executable built with this still runs on a
# machine with neither (and says so).
#
# DXC's headers: third_party/dxc (the Windows drop, inc/ and bin/x64/), and
# off Windows also a Vulkan SDK's include/dxc ($VULKAN_SDK, or the
# system-wide install the macOS SDK installer makes under /usr/local).
set(XBR_THIRD_PARTY ${CMAKE_CURRENT_LIST_DIR}/../../third_party)
set(XBR_DXC_INCLUDE "")
set(XBR_DXC_SEARCH ${XBR_THIRD_PARTY}/dxc/inc)
if(NOT WIN32)       # Windows keeps to the drop, as it always has
    list(APPEND XBR_DXC_SEARCH $ENV{VULKAN_SDK}/include/dxc /usr/local/include/dxc)
endif()
foreach(dir ${XBR_DXC_SEARCH})
    if(NOT XBR_DXC_INCLUDE AND EXISTS ${dir}/dxcapi.h)
        set(XBR_DXC_INCLUDE ${dir})
    endif()
endforeach()
if(EXISTS ${XBR_THIRD_PARTY}/Vulkan-Headers/include/vulkan/vulkan.h AND
   EXISTS ${XBR_THIRD_PARTY}/volk/volk.c AND
   EXISTS ${XBR_THIRD_PARTY}/VulkanMemoryAllocator/include/vk_mem_alloc.h AND
   XBR_DXC_INCLUDE)
    set(XBR_VULKAN_DEFAULT ON)
else()
    set(XBR_VULKAN_DEFAULT OFF)
endif()
option(XBOXRECOMP_VULKAN "Build the Vulkan renderer backend" ${XBR_VULKAN_DEFAULT})
if(XBOXRECOMP_VULKAN)
    if(NOT XBR_DXC_INCLUDE)
        message(FATAL_ERROR "XBOXRECOMP_VULKAN needs DXC's headers (dxcapi.h): third_party/dxc/inc, "
                            "or a Vulkan SDK (set VULKAN_SDK)")
    endif()
    target_sources(xbox_d3d8 PRIVATE
        rhi_vulkan.c
        rhi_vulkan_dxc.cpp
        rhi_vulkan_vma.cpp
        ${XBR_THIRD_PARTY}/volk/volk.c)
    target_include_directories(xbox_d3d8 PRIVATE
        ${XBR_THIRD_PARTY}/Vulkan-Headers/include
        ${XBR_THIRD_PARTY}/volk
        ${XBR_THIRD_PARTY}/VulkanMemoryAllocator/include
        ${XBR_DXC_INCLUDE})
    # VK_ENABLE_BETA_EXTENSIONS: VK_KHR_portability_subset (MoltenVK) is
    # still declared in vulkan_beta.h.
    target_compile_definitions(xbox_d3d8 PRIVATE
        RHI_HAVE_VULKAN VK_NO_PROTOTYPES VK_ENABLE_BETA_EXTENSIONS)
    if(WIN32)
        target_compile_definitions(xbox_d3d8 PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    elseif(APPLE)
        target_compile_definitions(xbox_d3d8 PRIVATE VK_USE_PLATFORM_METAL_EXT)
    endif()
    # Where the build found the compiler, as the last place to look.
    if(WIN32 AND EXISTS ${XBR_THIRD_PARTY}/dxc/bin/x64/dxcompiler.dll)
        set(XBR_DXC_LIB ${XBR_THIRD_PARTY}/dxc/bin/x64/dxcompiler.dll)
    elseif(APPLE AND EXISTS ${XBR_DXC_INCLUDE}/../../lib/libdxcompiler.dylib)
        get_filename_component(XBR_DXC_LIB ${XBR_DXC_INCLUDE}/../../lib/libdxcompiler.dylib ABSOLUTE)
    elseif(UNIX AND EXISTS ${XBR_DXC_INCLUDE}/../../lib/libdxcompiler.so)
        get_filename_component(XBR_DXC_LIB ${XBR_DXC_INCLUDE}/../../lib/libdxcompiler.so ABSOLUTE)
    endif()
    if(XBR_DXC_LIB)
        file(TO_NATIVE_PATH ${XBR_DXC_LIB} XBR_DXC_LIB)
        string(REPLACE "\\" "\\\\" XBR_DXC_LIB "${XBR_DXC_LIB}")
        set_source_files_properties(rhi_vulkan_dxc.cpp PROPERTIES
            COMPILE_DEFINITIONS "RHI_DXC_LIBRARY=\"${XBR_DXC_LIB}\"")
    endif()
    # Vendored code keeps its own warnings.
    if(MSVC)
        set_source_files_properties(${XBR_THIRD_PARTY}/volk/volk.c rhi_vulkan_vma.cpp
                                    PROPERTIES COMPILE_OPTIONS "/W0")
    else()
        set_source_files_properties(${XBR_THIRD_PARTY}/volk/volk.c rhi_vulkan_vma.cpp
                                    PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    message(STATUS "xbox_d3d8: Vulkan backend on (DXC headers: ${XBR_DXC_INCLUDE})")
    # For tests/nv2a_vsh_hlsl and tests/nv2a_combiners_hlsl, which compile
    # the generators' output with the backend's own DXC call.
    set(XBOXRECOMP_DXC_INCLUDE "${XBR_DXC_INCLUDE}" CACHE INTERNAL "")
    set(XBOXRECOMP_DXC_LIBRARY "${XBR_DXC_LIB}" CACHE INTERNAL "")
else()
    set(XBOXRECOMP_DXC_INCLUDE "" CACHE INTERNAL "")
    set(XBOXRECOMP_DXC_LIBRARY "" CACHE INTERNAL "")
endif()
if(NOT XBOXRECOMP_VULKAN AND NOT WIN32)
    message(WARNING "xbox_d3d8: no Vulkan backend (vendored Vulkan headers, volk, VMA or DXC's "
                    "headers missing), so this build has no renderer")
endif()

# Offline smoke tests for the pure format layer (no device needed); off
# Windows only the two that need no D3D11.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                 ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Whatever links this has the whole renderer (src/hle keys its shadow path on
# it). True on every platform since the renderer stopped being Win32-only;
# kept so nothing above has to assume it.
target_compile_definitions(xbox_d3d8 INTERFACE XBOXRECOMP_HOST_RENDERER=1)
set(XBOXRECOMP_HOST_RENDERER ON CACHE INTERNAL "xbox_d3d8 is the full renderer")
target_link_libraries(xbox_d3d8 PUBLIC xbox_config)

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2> /Zi)  # /Zi: see src/kernel/CMakeLists.txt
else()
    # -Wno-missing-field-initializers: Vulkan's `{ VK_STRUCTURE_TYPE_... }` idiom.
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter
                           -Wno-missing-field-initializers -O2)
endif()

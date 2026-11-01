#pragma once
#include <optional>
#include <vulkan/vulkan.h>
#include <GLES3/gl32.h>
#include <libdrm/drm_fourcc.h>
namespace Offload {
struct TransportFormat { VkFormat vk; GLenum gl; unsigned bytes; bool opaque, floating; };
inline std::optional<TransportFormat> transportFormat(uint32_t format) {
    switch(format) {
        case DRM_FORMAT_ABGR8888:return TransportFormat{VK_FORMAT_R8G8B8A8_UNORM,GL_RGBA8,4,false,false};
        case DRM_FORMAT_XBGR8888:return TransportFormat{VK_FORMAT_R8G8B8A8_UNORM,GL_RGBA8,4,true,false};
        case DRM_FORMAT_ARGB8888:return TransportFormat{VK_FORMAT_B8G8R8A8_UNORM,GL_RGBA8,4,false,false};
        case DRM_FORMAT_XRGB8888:return TransportFormat{VK_FORMAT_B8G8R8A8_UNORM,GL_RGBA8,4,true,false};
        case DRM_FORMAT_ABGR2101010:return TransportFormat{VK_FORMAT_A2B10G10R10_UNORM_PACK32,GL_RGB10_A2,4,false,false};
        case DRM_FORMAT_XBGR2101010:return TransportFormat{VK_FORMAT_A2B10G10R10_UNORM_PACK32,GL_RGB10_A2,4,true,false};
        case DRM_FORMAT_ARGB2101010:return TransportFormat{VK_FORMAT_A2R10G10B10_UNORM_PACK32,GL_RGB10_A2,4,false,false};
        case DRM_FORMAT_XRGB2101010:return TransportFormat{VK_FORMAT_A2R10G10B10_UNORM_PACK32,GL_RGB10_A2,4,true,false};
        case DRM_FORMAT_ABGR16161616F:return TransportFormat{VK_FORMAT_R16G16B16A16_SFLOAT,GL_RGBA16F,8,false,true};
        case DRM_FORMAT_XBGR16161616F:return TransportFormat{VK_FORMAT_R16G16B16A16_SFLOAT,GL_RGBA16F,8,true,true};
        default:return std::nullopt;
    }
}
}

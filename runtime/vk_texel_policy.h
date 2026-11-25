// SPDX-License-Identifier: MIT
// The current translator represents sampled fetch results as float-register
// bits. A 32-bit integer buffer therefore needs a bit-preserving float view,
// rather than a numeric integer-to-float conversion. Storage images keep their
// declared numeric type. Smaller integer channels need a separate lowering.
#pragma once
static VkFormat vk_texel_sample_format(VkFormat storage_format, unsigned scalar, int storage) {
    switch (scalar) {
    case GFX_SAMPLED_FLOAT:
        switch (storage_format) {
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return storage ? VK_FORMAT_UNDEFINED : VK_FORMAT_R32_SFLOAT;
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return storage ? VK_FORMAT_UNDEFINED : VK_FORMAT_R32G32_SFLOAT;
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return storage ? VK_FORMAT_UNDEFINED : VK_FORMAT_R32G32B32A32_SFLOAT;
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return VK_FORMAT_UNDEFINED;
        default: return storage_format;
        }
    case GFX_SAMPLED_UINT:
        switch (storage_format) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return storage_format;
        default: return VK_FORMAT_UNDEFINED;
        }
    case GFX_SAMPLED_SINT:
        switch (storage_format) {
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return storage_format;
        default: return VK_FORMAT_UNDEFINED;
        }
    default: return VK_FORMAT_UNDEFINED;
    }
}

static inline VkFormat vk_texel_zero_format(unsigned scalar) {
    switch (scalar) {
    case GFX_SAMPLED_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case GFX_SAMPLED_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case GFX_SAMPLED_SINT: return VK_FORMAT_R32G32B32A32_SINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

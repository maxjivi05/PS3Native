// SPDX-FileCopyrightText: Copyright 2026 qwertypower (DEVAR Entertainment LLC)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "stdafx.h"
#include "dis_flow.h"
#include "dis_shaders.hpp"

#include "../vkutils/device.h"

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

namespace dis {

namespace {

constexpr u32 DIS_SHARED_SETS =
    DIS_SLOTS * DIS_MAX_LEVELS * DIS_SHARED_SETS_PER_LEVEL + DIS_SLOTS;
constexpr u32 DIS_VR_SETS = DIS_SLOTS + DIS_VR_SHARED_SETS;
constexpr u32 DIS_TOTAL_SETS = DIS_SHARED_SETS + DIS_VR_SETS;

constexpr u32 DIS_SAMPLER_DESCRIPTORS =
    DIS_SHARED_SETS * DIS_SET_SAMPLERS + DIS_VR_SETS * DIS_VR_SAMPLER_BINDINGS;
constexpr u32 DIS_STORAGE_DESCRIPTORS =
    DIS_SHARED_SETS * DIS_SET_STORAGE + DIS_VR_SETS * DIS_VR_STORAGE_BINDINGS;

struct DisGradientPush {
    float lesser;
    float upper;
    float norm_val;
};

struct DisInversePush {
    s32 level;
    s32 coarse_level;
};

struct DisPropPush {
    s32 dist;
};

struct DisInterpPush {
    float timestamp;
    s32 debug_mode;
};

struct DisVrWeightPush {
    float alpha2;
    float eps2;
};

struct DisVrCoefPush {
    float delta2;
    float gamma2;
    float zeta2;
    float eps2;
};

struct DisVrSorPush {
    float omega;
    s32 parity;
};

static_assert(sizeof(DisGradientPush) == 12);
static_assert(sizeof(DisInversePush) == 8);
static_assert(sizeof(DisPropPush) == 4);
static_assert(sizeof(DisInterpPush) == 8);
static_assert(sizeof(DisVrWeightPush) == 8);
static_assert(sizeof(DisVrCoefPush) == 16);
static_assert(sizeof(DisVrSorPush) == 8);
static_assert(sizeof(DisVrCoefPush) <= DIS_PUSH_CONSTANT_SIZE);

const char* FormatName(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16_SFLOAT:
        return "R16_SFLOAT";
    case VK_FORMAT_R32_SFLOAT:
        return "R32_SFLOAT";
    case VK_FORMAT_R32G32_SFLOAT:
        return "R32G32_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case VK_FORMAT_UNDEFINED:
        return "none";
    default:
        return "format";
    }
}

std::string MissingFeatures(VkFormatFeatureFlags missing) {
    struct FeatureName {
        VkFormatFeatureFlags bit;
        const char* name;
    };

    constexpr std::array<FeatureName, 5> names{
        FeatureName{VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "STORAGE_IMAGE"},
        FeatureName{VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "SAMPLED_IMAGE"},
        FeatureName{VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                    "SAMPLED_IMAGE_FILTER_LINEAR"},
        FeatureName{VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, "TRANSFER_SRC"},
        FeatureName{VK_FORMAT_FEATURE_TRANSFER_DST_BIT, "TRANSFER_DST"},
    };

    std::string out;
    for (const FeatureName& entry : names) {
        if ((missing & entry.bit) == 0) continue;
        if (!out.empty()) out += '+';
        out += entry.name;
    }
    if (out.empty()) out = "none";
    return out;
}

void ComputeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VK_GET_SYMBOL(vkCmdPipelineBarrier)(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                                        nullptr, 0, nullptr);
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                  VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = 1;
    VK_GET_SYMBOL(vkCmdPipelineBarrier)(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                                        &barrier);
}

void BlitRect(VkCommandBuffer cmd, VkImage src, s32 sx, s32 sy, u32 sw, u32 sh, VkImage dst, s32 dx,
              s32 dy, u32 dw, u32 dh, VkFilter filter) {
    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].x = sx;
    blit.srcOffsets[0].y = sy;
    blit.srcOffsets[1].x = sx + static_cast<s32>(sw);
    blit.srcOffsets[1].y = sy + static_cast<s32>(sh);
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0].x = dx;
    blit.dstOffsets[0].y = dy;
    blit.dstOffsets[1].x = dx + static_cast<s32>(dw);
    blit.dstOffsets[1].y = dy + static_cast<s32>(dh);
    blit.dstOffsets[1].z = 1;
    VK_GET_SYMBOL(vkCmdBlitImage)(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst,
                                  VK_IMAGE_LAYOUT_GENERAL, 1, &blit, filter);
}

void BlitMip(VkCommandBuffer cmd, VkImage image, u32 src_level, u32 dst_level, u32 src_w, u32 src_h,
             u32 dst_w, u32 dst_h) {
    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = src_level;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = static_cast<s32>(src_w);
    blit.srcOffsets[1].y = static_cast<s32>(src_h);
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = dst_level;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1].x = static_cast<s32>(dst_w);
    blit.dstOffsets[1].y = static_cast<s32>(dst_h);
    blit.dstOffsets[1].z = 1;
    VK_GET_SYMBOL(vkCmdBlitImage)(cmd, image, VK_IMAGE_LAYOUT_GENERAL, image,
                                  VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
}

VkPipeline CreateComputePipeline(const Device& device, DisShader shader, VkPipelineLayout layout,
                                 const VkSpecializationInfo* spec, bool& load_failed) {
    const std::vector<u32>* code = GetDisShader(shader);
    if (code == nullptr || code->empty()) {
        load_failed = true;
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo module_ci{};
    module_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_ci.codeSize = code->size() * sizeof(u32);
    module_ci.pCode = code->data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (VK_GET_SYMBOL(vkCreateShaderModule)(device.Handle(), &module_ci, nullptr, &module) !=
        VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    stage.pSpecializationInfo = spec;

    VkComputePipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_ci.stage = stage;
    pipeline_ci.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = VK_GET_SYMBOL(vkCreateComputePipelines)(
        device.Handle(), VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline);
    VK_GET_SYMBOL(vkDestroyShaderModule)(device.Handle(), module, nullptr);
    return result == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

std::vector<VkDescriptorSet> AllocateDescriptorSets(const Device& device, VkDescriptorPool pool,
                                                    VkDescriptorSetLayout layout, u32 count) {
    if (pool == VK_NULL_HANDLE || layout == VK_NULL_HANDLE || count == 0) return {};

    const std::vector<VkDescriptorSetLayout> layouts(count, layout);
    VkDescriptorSetAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = pool;
    allocate_info.descriptorSetCount = count;
    allocate_info.pSetLayouts = layouts.data();

    std::vector<VkDescriptorSet> sets(count, VK_NULL_HANDLE);
    const VkResult result =
        VK_GET_SYMBOL(vkAllocateDescriptorSets)(device.Handle(), &allocate_info, sets.data());
    if (result != VK_SUCCESS) {
        rsx_log.error("Frame generation (DIS): descriptor allocation failed (%d) asking for %u sets",
                      static_cast<int>(result), count);
        return {};
    }
    return sets;
}

class DisDescriptorWriter {
public:
    DisDescriptorWriter& Sampled(VkDescriptorSet set, u32 binding, VkImageView view,
                                 VkSampler sampler) {
        return Push(set, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler, view);
    }

    DisDescriptorWriter& Storage(VkDescriptorSet set, u32 binding, VkImageView view) {
        return Push(set, binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, view);
    }

    void Build(const Device& device) {
        if (writes.empty()) return;
        VK_GET_SYMBOL(vkUpdateDescriptorSets)(device.Handle(),
                                              static_cast<u32>(writes.size()), writes.data(), 0,
                                              nullptr);
        writes.clear();
        image_infos.clear();
    }

private:
    DisDescriptorWriter& Push(VkDescriptorSet set, u32 binding, VkDescriptorType type,
                              VkSampler sampler, VkImageView view) {
        if (set == VK_NULL_HANDLE || view == VK_NULL_HANDLE) return *this;

        VkDescriptorImageInfo& info = image_infos.emplace_back();
        info.sampler = sampler;
        info.imageView = view;
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet& write = writes.emplace_back();
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pImageInfo = &info;
        return *this;
    }

    std::deque<VkDescriptorImageInfo> image_infos;
    std::vector<VkWriteDescriptorSet> writes;
};

}

Device::Device(VkDevice device_, VkPhysicalDevice physical_device_)
    : device{device_}, physical_device{physical_device_} {
    VK_GET_SYMBOL(vkGetPhysicalDeviceMemoryProperties)(physical_device, &memory_properties);
}

u32 Device::FindMemoryType(u32 bits, VkMemoryPropertyFlags properties) const {
    for (u32 i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((bits & (1u << i)) == 0) continue;
        if ((memory_properties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return UINT32_MAX;
}

DisImage::DisImage(const Device& device_, u32 width, u32 height, VkFormat format_, u32 mip_levels_,
                   VkImageUsageFlags usage)
    : device{device_.Handle()} {
    VkImageCreateInfo image_ci{};
    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = format_;
    image_ci.extent = {std::max(1u, width), std::max(1u, height), 1};
    image_ci.mipLevels = std::max(1u, mip_levels_);
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = usage;
    image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VK_GET_SYMBOL(vkCreateImage)(device, &image_ci, nullptr, &image) != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        return;
    }

    VkMemoryRequirements requirements;
    VK_GET_SYMBOL(vkGetImageMemoryRequirements)(device, image, &requirements);

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex =
        device_.FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate_info.memoryTypeIndex == UINT32_MAX ||
        VK_GET_SYMBOL(vkAllocateMemory)(device, &allocate_info, nullptr, &memory) != VK_SUCCESS) {
        Release();
        return;
    }
    if (VK_GET_SYMBOL(vkBindImageMemory)(device, image, memory, 0) != VK_SUCCESS) {
        Release();
    }
}

DisImage::~DisImage() {
    Release();
}

DisImage::DisImage(DisImage&& other) noexcept
    : device{other.device}, image{other.image}, memory{other.memory} {
    other.device = VK_NULL_HANDLE;
    other.image = VK_NULL_HANDLE;
    other.memory = VK_NULL_HANDLE;
}

DisImage& DisImage::operator=(DisImage&& other) noexcept {
    if (this != &other) {
        Release();
        device = other.device;
        image = other.image;
        memory = other.memory;
        other.device = VK_NULL_HANDLE;
        other.image = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
    }
    return *this;
}

void DisImage::Release() {
    if (device == VK_NULL_HANDLE) return;
    if (image) VK_GET_SYMBOL(vkDestroyImage)(device, image, nullptr);
    if (memory) VK_GET_SYMBOL(vkFreeMemory)(device, memory, nullptr);
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

DisFlow::DisFlow(const vk::render_device& device_) : device{device_, device_.gpu()} {
    luma_format = PickLumaFormat();

    if (!AuditFormats()) {
        rsx_log.error("Frame generation (DIS): this device's format support cannot run DIS; "
                      "frame generation stays off");
        unavailable = true;
        return;
    }

    if (!CreateSampler() || !CreatePipelines()) {
        if (shader_load_failed) {
            rsx_log.error("Frame generation (DIS): the compute shader set is incomplete; "
                          "frame generation stays off");
        } else {
            rsx_log.error("Frame generation (DIS): shaders could not be built; "
                          "frame generation stays off");
        }
        unavailable = true;
        return;
    }

    if (!AllocateSets()) {
        rsx_log.error("Frame generation (DIS): descriptor sets could not be allocated; "
                      "frame generation stays off");
        unavailable = true;
        return;
    }

    ready = true;
    rsx_log.notice("Frame generation (DIS): ready");
}

DisFlow::~DisFlow() = default;

VkFormat DisFlow::PickLumaFormat() const {
    constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    constexpr std::array<VkFormat, 2> candidates{VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT};

    for (usz i = 0; i < candidates.size(); i++) {
        VkFormatProperties properties{};
        VK_GET_SYMBOL(vkGetPhysicalDeviceFormatProperties)(device.Gpu(), candidates[i],
                                                           &properties);
        if ((properties.optimalTilingFeatures & need) == need) {
            if (i != 0) {
                rsx_log.notice("Frame generation (DIS): R16F unusable for the luminance plane; "
                               "using R32F");
            }
            return candidates[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

bool DisFlow::AuditFormats() {
    constexpr VkFormatFeatureFlags store = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    constexpr VkFormatFeatureFlags read = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    constexpr VkFormatFeatureFlags filter = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

    if (luma_format == VK_FORMAT_UNDEFINED) {
        rsx_log.error("Frame generation (DIS): needs a single-channel float plane it can both "
                      "write and filter; neither R16_SFLOAT nor R32_SFLOAT qualifies here");
        return false;
    }

    struct Requirement {
        VkFormat format;
        VkFormatFeatureFlags need;
        const char* purpose;
    };

    const std::array<Requirement, 5> requirements{
        Requirement{DIS_FLOW_FORMAT, store | read, "optical flow"},
        Requirement{DIS_SPARSE_FORMAT, store | read, "sparse flow and refinement"},
        Requirement{DIS_WEIGHT_FORMAT, store | read, "refinement weights"},
        Requirement{DIS_OUTPUT_FORMAT, store | read, "interpolated output"},
        Requirement{luma_format, store | read | filter, "luminance plane"},
    };

    std::string line;
    bool ok = true;

    for (const Requirement& requirement : requirements) {
        VkFormatProperties properties{};
        VK_GET_SYMBOL(vkGetPhysicalDeviceFormatProperties)(device.Gpu(), requirement.format,
                                                           &properties);
        const VkFormatFeatureFlags missing = requirement.need & ~properties.optimalTilingFeatures;

        if (!line.empty()) line += ' ';
        line += FormatName(requirement.format);
        line += missing ? "=MISSING" : "=ok";

        if (missing) {
            rsx_log.error("Frame generation (DIS): needs %s on %s for %s, and this device does "
                          "not report it",
                          MissingFeatures(missing), FormatName(requirement.format),
                          requirement.purpose);
            ok = false;
        }
    }

    VkFormatProperties flow_properties{};
    VK_GET_SYMBOL(vkGetPhysicalDeviceFormatProperties)(device.Gpu(), DIS_FLOW_FORMAT,
                                                       &flow_properties);
    manual_flow_filter = (flow_properties.optimalTilingFeatures & filter) == 0;

    rsx_log.notice("Frame generation (DIS): format support: %s | flow filtering: %s", line,
                   manual_flow_filter ? "in shader (driver cannot filter R32G32_SFLOAT)"
                                      : "sampler");
    return ok;
}

bool DisFlow::CreateSampler() {
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.minLod = 0.0f;
    sampler_ci.maxLod = static_cast<float>(DIS_MAX_LEVELS);

    VkSampler handle = VK_NULL_HANDLE;
    if (VK_GET_SYMBOL(vkCreateSampler)(device.Handle(), &sampler_ci, nullptr, &handle) !=
        VK_SUCCESS) {
        return false;
    }
    sampler = DisSampler(device.Handle(), handle);
    return true;
}

bool DisFlow::CreatePipelines() {
    rsx_log.notice("Frame generation: compiling the DIS shader chain");

    std::array<VkDescriptorSetLayoutBinding, DIS_SET_SAMPLERS + DIS_SET_STORAGE> bindings{};
    for (u32 i = 0; i < DIS_SET_SAMPLERS; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[DIS_SET_SAMPLERS].binding = DIS_SET_SAMPLERS;
    bindings[DIS_SET_SAMPLERS].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[DIS_SET_SAMPLERS].descriptorCount = 1;
    bindings[DIS_SET_SAMPLERS].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = static_cast<u32>(bindings.size());
    layout_ci.pBindings = bindings.data();

    VkDescriptorSetLayout set_layout_handle = VK_NULL_HANDLE;
    if (VK_GET_SYMBOL(vkCreateDescriptorSetLayout)(device.Handle(), &layout_ci, nullptr,
                                                   &set_layout_handle) != VK_SUCCESS) {
        return false;
    }
    set_layout = DisSetLayout(device.Handle(), set_layout_handle);

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = DIS_PUSH_CONSTANT_SIZE;

    VkPipelineLayoutCreateInfo pipeline_layout_ci{};
    pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_ci.setLayoutCount = 1;
    pipeline_layout_ci.pSetLayouts = &set_layout_handle;
    pipeline_layout_ci.pushConstantRangeCount = 1;
    pipeline_layout_ci.pPushConstantRanges = &push_range;

    VkPipelineLayout pipeline_layout_handle = VK_NULL_HANDLE;
    if (VK_GET_SYMBOL(vkCreatePipelineLayout)(device.Handle(), &pipeline_layout_ci, nullptr,
                                              &pipeline_layout_handle) != VK_SUCCESS) {
        return false;
    }
    pipeline_layout = DisPipelineLayout(device.Handle(), pipeline_layout_handle);

    std::array<VkDescriptorPoolSize, 2> sizes{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = DIS_SAMPLER_DESCRIPTORS;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = DIS_STORAGE_DESCRIPTORS;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = DIS_TOTAL_SETS;
    pool_ci.poolSizeCount = static_cast<u32>(sizes.size());
    pool_ci.pPoolSizes = sizes.data();

    VkDescriptorPool pool_handle = VK_NULL_HANDLE;
    if (VK_GET_SYMBOL(vkCreateDescriptorPool)(device.Handle(), &pool_ci, nullptr, &pool_handle) !=
        VK_SUCCESS) {
        return false;
    }
    pool = DisDescriptorPool(device.Handle(), pool_handle);

    std::array<VkDescriptorSetLayoutBinding, DIS_VR_SAMPLER_BINDINGS + DIS_VR_STORAGE_BINDINGS>
        vr_bindings{};
    for (u32 i = 0; i < DIS_VR_SAMPLER_BINDINGS; i++) {
        vr_bindings[i].binding = i;
        vr_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        vr_bindings[i].descriptorCount = 1;
        vr_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    for (u32 i = 0; i < DIS_VR_STORAGE_BINDINGS; i++) {
        VkDescriptorSetLayoutBinding& binding = vr_bindings[DIS_VR_SAMPLER_BINDINGS + i];
        binding.binding = DIS_VR_FIRST_STORAGE + i;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo vr_layout_ci{};
    vr_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    vr_layout_ci.bindingCount = static_cast<u32>(vr_bindings.size());
    vr_layout_ci.pBindings = vr_bindings.data();

    VkDescriptorSetLayout vr_set_layout_handle = VK_NULL_HANDLE;
    if (VK_GET_SYMBOL(vkCreateDescriptorSetLayout)(device.Handle(), &vr_layout_ci, nullptr,
                                                   &vr_set_layout_handle) != VK_SUCCESS) {
        return false;
    }
    vr_set_layout = DisSetLayout(device.Handle(), vr_set_layout_handle);

    VkPushConstantRange vr_push_range{};
    vr_push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    vr_push_range.offset = 0;
    vr_push_range.size = DIS_PUSH_CONSTANT_SIZE;

    VkPipelineLayoutCreateInfo vr_pipeline_layout_ci{};
    vr_pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    vr_pipeline_layout_ci.setLayoutCount = 1;
    vr_pipeline_layout_ci.pSetLayouts = &vr_set_layout_handle;
    vr_pipeline_layout_ci.pushConstantRangeCount = 1;
    vr_pipeline_layout_ci.pPushConstantRanges = &vr_push_range;

    VkPipelineLayout vr_pipeline_layout_handle = VK_NULL_HANDLE;
    if (VK_GET_SYMBOL(vkCreatePipelineLayout)(device.Handle(), &vr_pipeline_layout_ci, nullptr,
                                              &vr_pipeline_layout_handle) != VK_SUCCESS) {
        return false;
    }
    vr_pipeline_layout = DisPipelineLayout(device.Handle(), vr_pipeline_layout_handle);

    const VkPipelineLayout shared = pipeline_layout.Get();
    const VkPipelineLayout refine = vr_pipeline_layout.Get();

    const DisShader luma_shader =
        luma_format == VK_FORMAT_R16_SFLOAT ? DisShader::luma_r16 : DisShader::luma_r32;

    pass_luma = DisPipeline(device.Handle(), CreateComputePipeline(device, luma_shader, shared,
                                                                   nullptr, shader_load_failed));
    pass_gradient = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::gradient, shared, nullptr, shader_load_failed));
    pass_inverse =
        DisPipeline(device.Handle(), CreateComputePipeline(device, DisShader::inverse_search,
                                                          shared, nullptr, shader_load_failed));
    pass_propagate = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::propagate, shared, nullptr, shader_load_failed));
    pass_densify = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::densify, shared, nullptr, shader_load_failed));

    const VkBool32 manual_filter = manual_flow_filter ? VK_TRUE : VK_FALSE;
    VkSpecializationMapEntry spec_entry{};
    spec_entry.constantID = 0;
    spec_entry.offset = 0;
    spec_entry.size = sizeof(manual_filter);
    VkSpecializationInfo spec{};
    spec.mapEntryCount = 1;
    spec.pMapEntries = &spec_entry;
    spec.dataSize = sizeof(manual_filter);
    spec.pData = &manual_filter;

    pass_interp = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::interpolate, shared, &spec, shader_load_failed));

    pass_vr_prep = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::vr_prep, refine, nullptr, shader_load_failed));
    pass_vr_d1 = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::vr_d1, refine, nullptr, shader_load_failed));
    pass_vr_d2 = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::vr_d2, refine, nullptr, shader_load_failed));
    pass_vr_w = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::vr_w, refine, nullptr, shader_load_failed));
    pass_vr_coef = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::vr_coef, refine, nullptr, shader_load_failed));
    pass_vr_sor = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::vr_sor, refine, nullptr, shader_load_failed));
    pass_vr_add = DisPipeline(
        device.Handle(),
        CreateComputePipeline(device, DisShader::vr_add, refine, nullptr, shader_load_failed));

    return pass_luma.Valid() && pass_gradient.Valid() && pass_inverse.Valid() &&
           pass_propagate.Valid() && pass_densify.Valid() && pass_interp.Valid() &&
           pass_vr_prep.Valid() && pass_vr_d1.Valid() && pass_vr_d2.Valid() && pass_vr_w.Valid() &&
           pass_vr_coef.Valid() && pass_vr_sor.Valid() && pass_vr_add.Valid();
}

bool DisFlow::AllocateSets() {
    for (u32 s = 0; s < DIS_SLOTS; s++) {
        for (u32 l = 0; l < DIS_MAX_LEVELS; l++) {
            const std::vector<VkDescriptorSet> sets = AllocateDescriptorSets(
                device, pool.Get(), set_layout.Get(), DIS_SHARED_SETS_PER_LEVEL);
            if (sets.size() != DIS_SHARED_SETS_PER_LEVEL) return false;
            grad_sets[s][l] = sets[0];
            inverse_sets[s][l] = sets[1];
            densify_sets[s][l] = sets[2];
            prop_ab_sets[s][l] = sets[3];
            prop_ba_sets[s][l] = sets[4];
            luma_sets[s][l] = sets[5];
        }

        const std::vector<VkDescriptorSet> interp =
            AllocateDescriptorSets(device, pool.Get(), set_layout.Get(), 1);
        if (interp.size() != 1) return false;
        interp_sets[s] = interp[0];

        const std::vector<VkDescriptorSet> prep =
            AllocateDescriptorSets(device, pool.Get(), vr_set_layout.Get(), 1);
        if (prep.size() != 1) return false;
        vr_prep_sets[s] = prep[0];
    }

    const std::vector<VkDescriptorSet> vr_sets =
        AllocateDescriptorSets(device, pool.Get(), vr_set_layout.Get(), DIS_VR_SHARED_SETS);
    if (vr_sets.size() != DIS_VR_SHARED_SETS) return false;
    vr_d1_set = vr_sets[0];
    vr_d2_set = vr_sets[1];
    vr_w_set = vr_sets[2];
    vr_coef_set = vr_sets[3];
    vr_sor_ab_set = vr_sets[4];
    vr_sor_ba_set = vr_sets[5];
    vr_add_set = vr_sets[6];
    return true;
}

void DisFlow::DestroyResources() {
    for (u32 s = 0; s < DIS_SLOTS; s++) {
        view_color[s] = DisImageView();
        for (u32 l = 0; l < DIS_MAX_LEVELS; l++) {
            view_flow_color[s][l] = DisImageView();
            view_flow_luma[s][l] = DisImageView();
        }
    }
    for (u32 l = 0; l < DIS_MAX_LEVELS; l++) {
        view_grad[l] = DisImageView();
        view_sparse[l] = DisImageView();
        view_sparse_b[l] = DisImageView();
        view_dense[l] = DisImageView();
    }
    view_interp_out = DisImageView();
    view_vr_prep = DisImageView();
    view_vr_d1 = DisImageView();
    view_vr_d2 = DisImageView();
    view_vr_a = DisImageView();
    view_vr_b = DisImageView();
    view_vr_wt = DisImageView();
    view_vr_dw[0] = DisImageView();
    view_vr_dw[1] = DisImageView();
    view_flow_refined = DisImageView();

    for (u32 s = 0; s < DIS_SLOTS; s++) {
        color[s] = DisImage();
        flow_color[s] = DisImage();
        flow_luma[s] = DisImage();
    }
    grad = DisImage();
    for (u32 l = 0; l < DIS_MAX_LEVELS; l++) {
        flow_sparse[l] = DisImage();
        flow_sparse_b[l] = DisImage();
    }
    flow_dense = DisImage();
    interp_out = DisImage();
    vr_prep = DisImage();
    vr_d1 = DisImage();
    vr_d2 = DisImage();
    vr_a = DisImage();
    vr_b = DisImage();
    vr_wt = DisImage();
    vr_dw[0] = DisImage();
    vr_dw[1] = DisImage();
    flow_refined = DisImage();

    built = false;
    built_extent = VkExtent2D{};
    built_full_extent = VkExtent2D{};
    built_format = VK_FORMAT_UNDEFINED;
    built_min_side = 0;
    levels = 0;
    frame_count = 0;
    active_slot = 0;
    last_generations = 0;
}

bool DisFlow::CreateResources(u32 width, u32 height, u32 full_width, u32 full_height,
                              VkFormat format) {
    if (built) {
        VK_GET_SYMBOL(vkDeviceWaitIdle)(device.Handle());
    }

    DestroyResources();
    layouts_primed = false;

    const u32 level_count = levels;

    constexpr VkImageUsageFlags color_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageUsageFlags compute_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

    for (u32 s = 0; s < DIS_SLOTS; s++) {
        color[s] = DisImage(device, full_width, full_height, format, 1, color_usage);
        if (!color[s].Valid()) return false;
        flow_color[s] = DisImage(device, width, height, format, level_count, color_usage);
        if (!flow_color[s].Valid()) return false;
        flow_luma[s] = DisImage(device, width, height, luma_format, level_count, compute_usage);
        if (!flow_luma[s].Valid()) return false;
    }

    grad = DisImage(device, width, height, DIS_FLOW_FORMAT, level_count, compute_usage);
    if (!grad.Valid()) return false;

    for (u32 l = 0; l < level_count; l++) {
        const u32 sparse_w = SparseExtent(width >> l);
        const u32 sparse_h = SparseExtent(height >> l);
        flow_sparse[l] = DisImage(device, sparse_w, sparse_h, DIS_SPARSE_FORMAT, 1, compute_usage);
        if (!flow_sparse[l].Valid()) return false;
        flow_sparse_b[l] =
            DisImage(device, sparse_w, sparse_h, DIS_SPARSE_FORMAT, 1, compute_usage);
        if (!flow_sparse_b[l].Valid()) return false;
    }

    flow_dense = DisImage(device, width, height, DIS_FLOW_FORMAT, level_count, compute_usage);
    if (!flow_dense.Valid()) return false;

    interp_out = DisImage(device, full_width, full_height, DIS_OUTPUT_FORMAT, 1,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!interp_out.Valid()) return false;

    vr_prep = DisImage(device, width, height, DIS_FLOW_FORMAT, 1, compute_usage);
    if (!vr_prep.Valid()) return false;
    vr_d1 = DisImage(device, width, height, DIS_SPARSE_FORMAT, 1, compute_usage);
    if (!vr_d1.Valid()) return false;
    vr_d2 = DisImage(device, width, height, DIS_SPARSE_FORMAT, 1, compute_usage);
    if (!vr_d2.Valid()) return false;
    vr_a = DisImage(device, width, height, DIS_SPARSE_FORMAT, 1, compute_usage);
    if (!vr_a.Valid()) return false;
    vr_b = DisImage(device, width, height, DIS_FLOW_FORMAT, 1, compute_usage);
    if (!vr_b.Valid()) return false;
    vr_wt = DisImage(device, width, height, DIS_WEIGHT_FORMAT, 1, compute_usage);
    if (!vr_wt.Valid()) return false;
    vr_dw[0] = DisImage(device, width, height, DIS_FLOW_FORMAT, 1, compute_usage);
    if (!vr_dw[0].Valid()) return false;
    vr_dw[1] = DisImage(device, width, height, DIS_FLOW_FORMAT, 1, compute_usage);
    if (!vr_dw[1].Valid()) return false;
    flow_refined = DisImage(device, width, height, DIS_FLOW_FORMAT, 1, compute_usage);
    if (!flow_refined.Valid()) return false;

    const auto make_view = [this](const DisImage& image, VkFormat view_format, u32 base_level,
                                  DisImageView& out) {
        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = image.Handle();
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = view_format;
        view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_ci.subresourceRange.baseMipLevel = base_level;
        view_ci.subresourceRange.levelCount = 1;
        view_ci.subresourceRange.layerCount = 1;

        VkImageView handle = VK_NULL_HANDLE;
        if (VK_GET_SYMBOL(vkCreateImageView)(device.Handle(), &view_ci, nullptr, &handle) !=
            VK_SUCCESS) {
            return false;
        }
        out = DisImageView(device.Handle(), handle);
        return true;
    };

    for (u32 s = 0; s < DIS_SLOTS; s++) {
        if (!make_view(color[s], format, 0, view_color[s])) return false;
    }
    for (u32 l = 0; l < level_count; l++) {
        for (u32 s = 0; s < DIS_SLOTS; s++) {
            if (!make_view(flow_color[s], format, l, view_flow_color[s][l])) return false;
            if (!make_view(flow_luma[s], luma_format, l, view_flow_luma[s][l])) return false;
        }
        if (!make_view(grad, DIS_FLOW_FORMAT, l, view_grad[l])) return false;
        if (!make_view(flow_sparse[l], DIS_SPARSE_FORMAT, 0, view_sparse[l])) return false;
        if (!make_view(flow_sparse_b[l], DIS_SPARSE_FORMAT, 0, view_sparse_b[l])) return false;
        if (!make_view(flow_dense, DIS_FLOW_FORMAT, l, view_dense[l])) return false;
    }
    if (!make_view(interp_out, DIS_OUTPUT_FORMAT, 0, view_interp_out)) return false;
    if (!make_view(vr_prep, DIS_FLOW_FORMAT, 0, view_vr_prep)) return false;
    if (!make_view(vr_d1, DIS_SPARSE_FORMAT, 0, view_vr_d1)) return false;
    if (!make_view(vr_d2, DIS_SPARSE_FORMAT, 0, view_vr_d2)) return false;
    if (!make_view(vr_a, DIS_SPARSE_FORMAT, 0, view_vr_a)) return false;
    if (!make_view(vr_b, DIS_FLOW_FORMAT, 0, view_vr_b)) return false;
    if (!make_view(vr_wt, DIS_WEIGHT_FORMAT, 0, view_vr_wt)) return false;
    if (!make_view(vr_dw[0], DIS_FLOW_FORMAT, 0, view_vr_dw[0])) return false;
    if (!make_view(vr_dw[1], DIS_FLOW_FORMAT, 0, view_vr_dw[1])) return false;
    if (!make_view(flow_refined, DIS_FLOW_FORMAT, 0, view_flow_refined)) return false;

    Reset();
    WriteAllDescriptors();
    return true;
}

void DisFlow::WriteAllDescriptors() {
    DisDescriptorWriter writer;
    const u32 level_count = levels;
    const u32 coarse = level_count - 1;
    const VkSampler handle = sampler.Get();

    for (u32 s = 0; s < DIS_SLOTS; s++) {
        const u32 next = s;
        const u32 prev = (s + DIS_SLOTS - 1) % DIS_SLOTS;

        for (u32 l = 0; l < level_count; l++) {
            writer.Sampled(luma_sets[s][l], 0, view_flow_color[next][l].Get(), handle);
            writer.Storage(luma_sets[s][l], 5, view_flow_luma[next][l].Get());

            writer.Sampled(grad_sets[s][l], 0, view_flow_luma[prev][l].Get(), handle);
            writer.Storage(grad_sets[s][l], 5, view_grad[l].Get());

            writer.Sampled(inverse_sets[s][l], 0, view_flow_luma[prev][l].Get(), handle);
            writer.Sampled(inverse_sets[s][l], 1, view_flow_luma[next][l].Get(), handle);
            writer.Sampled(inverse_sets[s][l], 2, view_grad[l].Get(), handle);
            writer.Sampled(inverse_sets[s][l], 3,
                           view_dense[l + 1 < level_count ? l + 1 : coarse].Get(), handle);
            writer.Sampled(inverse_sets[s][l], 4, view_dense[coarse].Get(), handle);
            writer.Storage(inverse_sets[s][l], 5, view_sparse[l].Get());

            writer.Sampled(prop_ab_sets[s][l], 0, view_flow_luma[prev][l].Get(), handle);
            writer.Sampled(prop_ab_sets[s][l], 1, view_flow_luma[next][l].Get(), handle);
            writer.Sampled(prop_ab_sets[s][l], 2, view_sparse[l].Get(), handle);
            writer.Storage(prop_ab_sets[s][l], 5, view_sparse_b[l].Get());

            writer.Sampled(prop_ba_sets[s][l], 0, view_flow_luma[prev][l].Get(), handle);
            writer.Sampled(prop_ba_sets[s][l], 1, view_flow_luma[next][l].Get(), handle);
            writer.Sampled(prop_ba_sets[s][l], 2, view_sparse_b[l].Get(), handle);
            writer.Storage(prop_ba_sets[s][l], 5, view_sparse[l].Get());

            writer.Sampled(densify_sets[s][l], 0, view_sparse[l].Get(), handle);
            writer.Sampled(densify_sets[s][l], 1, view_flow_luma[prev][l].Get(), handle);
            writer.Sampled(densify_sets[s][l], 2, view_flow_luma[next][l].Get(), handle);
            writer.Storage(densify_sets[s][l], 5, view_dense[l].Get());
        }

        writer.Sampled(interp_sets[s], 0, view_color[prev].Get(), handle);
        writer.Sampled(interp_sets[s], 1, view_color[next].Get(), handle);
        writer.Sampled(interp_sets[s], 2, view_flow_refined.Get(), handle);
        writer.Storage(interp_sets[s], 5, view_interp_out.Get());

        writer.Sampled(vr_prep_sets[s], 0, view_flow_color[prev][0].Get(), handle);
        writer.Sampled(vr_prep_sets[s], 1, view_flow_color[next][0].Get(), handle);
        writer.Sampled(vr_prep_sets[s], 2, view_dense[0].Get(), handle);
        writer.Storage(vr_prep_sets[s], DIS_VR_FIRST_STORAGE, view_vr_prep.Get());
        writer.Storage(vr_prep_sets[s], DIS_VR_FIRST_STORAGE + 1, view_vr_dw[0].Get());
    }

    writer.Sampled(vr_d1_set, 0, view_vr_prep.Get(), handle);
    writer.Storage(vr_d1_set, DIS_VR_FIRST_STORAGE, view_vr_d1.Get());

    writer.Sampled(vr_d2_set, 0, view_vr_d1.Get(), handle);
    writer.Storage(vr_d2_set, DIS_VR_FIRST_STORAGE, view_vr_d2.Get());

    writer.Sampled(vr_w_set, 0, view_dense[0].Get(), handle);
    writer.Sampled(vr_w_set, 1, view_vr_dw[0].Get(), handle);
    writer.Storage(vr_w_set, DIS_VR_FIRST_STORAGE, view_vr_wt.Get());

    writer.Sampled(vr_coef_set, 0, view_vr_prep.Get(), handle);
    writer.Sampled(vr_coef_set, 1, view_vr_d1.Get(), handle);
    writer.Sampled(vr_coef_set, 2, view_vr_d2.Get(), handle);
    writer.Sampled(vr_coef_set, 3, view_vr_dw[0].Get(), handle);
    writer.Sampled(vr_coef_set, 4, view_dense[0].Get(), handle);
    writer.Sampled(vr_coef_set, 5, view_vr_wt.Get(), handle);
    writer.Storage(vr_coef_set, DIS_VR_FIRST_STORAGE, view_vr_a.Get());
    writer.Storage(vr_coef_set, DIS_VR_FIRST_STORAGE + 1, view_vr_b.Get());

    writer.Sampled(vr_sor_ab_set, 0, view_vr_a.Get(), handle);
    writer.Sampled(vr_sor_ab_set, 1, view_vr_b.Get(), handle);
    writer.Sampled(vr_sor_ab_set, 2, view_vr_wt.Get(), handle);
    writer.Sampled(vr_sor_ab_set, 3, view_vr_dw[0].Get(), handle);
    writer.Storage(vr_sor_ab_set, DIS_VR_FIRST_STORAGE, view_vr_dw[1].Get());

    writer.Sampled(vr_sor_ba_set, 0, view_vr_a.Get(), handle);
    writer.Sampled(vr_sor_ba_set, 1, view_vr_b.Get(), handle);
    writer.Sampled(vr_sor_ba_set, 2, view_vr_wt.Get(), handle);
    writer.Sampled(vr_sor_ba_set, 3, view_vr_dw[1].Get(), handle);
    writer.Storage(vr_sor_ba_set, DIS_VR_FIRST_STORAGE, view_vr_dw[0].Get());

    writer.Sampled(vr_add_set, 0, view_dense[0].Get(), handle);
    writer.Sampled(vr_add_set, 1, view_vr_dw[0].Get(), handle);
    writer.Storage(vr_add_set, DIS_VR_FIRST_STORAGE, view_flow_refined.Get());

    writer.Build(device);
}

void DisFlow::PrimeLayouts(VkCommandBuffer cmd) {
    if (layouts_primed) return;

    std::vector<VkImage> images;
    images.reserve(DIS_SLOTS * 3 + DIS_MAX_LEVELS * 2 + 12);

    const auto push = [&images](const DisImage& image) {
        if (image.Valid()) images.push_back(image.Handle());
    };

    for (u32 s = 0; s < DIS_SLOTS; s++) {
        push(color[s]);
        push(flow_color[s]);
        push(flow_luma[s]);
    }
    push(grad);
    for (u32 l = 0; l < DIS_MAX_LEVELS; l++) {
        push(flow_sparse[l]);
        push(flow_sparse_b[l]);
    }
    push(flow_dense);
    push(interp_out);
    push(vr_prep);
    push(vr_d1);
    push(vr_d2);
    push(vr_a);
    push(vr_b);
    push(vr_wt);
    push(vr_dw[0]);
    push(vr_dw[1]);
    push(flow_refined);

    if (images.empty()) return;

    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(images.size());

    for (const VkImage image : images) {
        VkImageMemoryBarrier& barrier = barriers.emplace_back();
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.layerCount = 1;
    }

    VK_GET_SYMBOL(vkCmdPipelineBarrier)(
        cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
        nullptr, static_cast<u32>(barriers.size()), barriers.data());
    layouts_primed = true;
}

void DisFlow::Dispatch(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, u32 width,
                       u32 height) const {
    VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                           pipeline_layout.Get(), 0, 1, &set, 0, nullptr);
    VK_GET_SYMBOL(vkCmdDispatch)(cmd, DispatchGroups(width), DispatchGroups(height), 1);
}

bool DisFlow::Configure(u32 flow_min_side_) {
    if (!Valid()) return false;

    flow_min_side = std::clamp(flow_min_side_, DIS_FLOW_MIN_SIDE_FLOOR, DIS_FLOW_MIN_SIDE_CEIL);
    return true;
}

bool DisFlow::NeedsRebuild(u32 width, u32 height, VkFormat format) const {
    if (!Valid()) return false;

    return !built || built_full_extent.width != width || built_full_extent.height != height ||
           built_format != format || built_min_side != flow_min_side;
}

bool DisFlow::Prepare(u32 width, u32 height, VkFormat format) {
    if (!Valid()) return false;
    if (width == 0 || height == 0 || format == VK_FORMAT_UNDEFINED) return false;

    VkExtent2D flow = FlowExtentFor(flow_min_side, width, height);
    flow.width = std::max(flow.width, DIS_MIN_EXTENT);
    flow.height = std::max(flow.height, DIS_MIN_EXTENT);

    const u32 level_count = LevelsFor(flow.width, flow.height);

    if (built && built_extent.width == flow.width && built_extent.height == flow.height &&
        built_full_extent.width == width && built_full_extent.height == height &&
        built_format == format && built_min_side == flow_min_side && levels == level_count) {
        return true;
    }

    if (!formats_audited) {
        formats_audited = true;

        constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        VkFormatProperties properties{};
        VK_GET_SYMBOL(vkGetPhysicalDeviceFormatProperties)(device.Gpu(), format, &properties);
        const VkFormatFeatureFlags missing = need & ~properties.optimalTilingFeatures;
        if (missing) {
            rsx_log.error("Frame generation (DIS): needs %s on the guest frame format (%d) and "
                          "this device does not report it; frame generation stays off",
                          MissingFeatures(missing), static_cast<int>(format));
            unavailable = true;
            return false;
        }
    }

    levels = level_count;

    if (!CreateResources(flow.width, flow.height, width, height, format)) {
        rsx_log.error("Frame generation (DIS): resource build failed at %ux%u; frame generation "
                      "unavailable",
                      flow.width, flow.height);
        DestroyResources();
        unavailable = true;
        return false;
    }

    built_extent = flow;
    built_full_extent = VkExtent2D{width, height};
    built_format = format;
    built_min_side = flow_min_side;
    built = true;
    frame_count = 0;
    active_slot = 0;
    last_generations = 0;

    rsx_log.notice("Frame generation (DIS): resources built at %ux%u (flow min side %u, %u "
                   "levels) for a %ux%u frame",
                   flow.width, flow.height, flow_min_side, level_count, width, height);
    return true;
}

void DisFlow::Process(VkCommandBuffer cmd, VkImage source, u32 width, u32 height,
                      u32 generations) {
    if (!built || !Valid()) return;
    if (width == 0 || height == 0 || source == VK_NULL_HANDLE) return;

    last_generations = generations;

    PrimeLayouts(cmd);

    const DisRefine refine = RefineFor(generations);
    const u32 level_count = levels;
    const u32 coarse = level_count - 1;
    const u32 flow_w = built_extent.width;
    const u32 flow_h = built_extent.height;
    const u32 full_w = built_full_extent.width;
    const u32 full_h = built_full_extent.height;

    const u32 slot = static_cast<u32>(frame_count % DIS_SLOTS);
    const VkImage full_dst = color[slot].Handle();
    const VkImage flow_dst = flow_color[slot].Handle();

    ImageBarrier(cmd, full_dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    BlitRect(cmd, source, 0, 0, width, height, full_dst, 0, 0, full_w, full_h, VK_FILTER_LINEAR);
    ImageBarrier(cmd, full_dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    ImageBarrier(cmd, flow_dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    BlitRect(cmd, source, 0, 0, width, height, flow_dst, 0, 0, flow_w, flow_h, VK_FILTER_LINEAR);
    for (u32 l = 1; l < level_count; l++) {
        ImageBarrier(cmd, flow_dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        BlitMip(cmd, flow_dst, l - 1, l, flow_w >> (l - 1), flow_h >> (l - 1), flow_w >> l,
                flow_h >> l);
    }
    ImageBarrier(cmd, flow_dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    active_slot = slot;
    frame_count++;

    for (u32 l = 0; l < level_count; l++) {
        VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_luma.Get());
        VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                               pipeline_layout.Get(), 0, 1, &luma_sets[slot][l], 0,
                                               nullptr);
        VK_GET_SYMBOL(vkCmdDispatch)(cmd, DispatchGroups(flow_w >> l), DispatchGroups(flow_h >> l),
                                     1);
    }

    ComputeBarrier(cmd);

    if (generations == 0) return;

    DisGradientPush gradient_push{};
    gradient_push.lesser = 3.0f;
    gradient_push.upper = 10.0f;
    gradient_push.norm_val = 1.0f / (2.0f * 10.0f + 4.0f * 3.0f);

    for (u32 l = 0; l < level_count; l++) {
        VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_gradient.Get());
        VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                               pipeline_layout.Get(), 0, 1, &grad_sets[slot][l], 0,
                                               nullptr);
        VK_GET_SYMBOL(vkCmdPushConstants)(cmd, pipeline_layout.Get(), VK_SHADER_STAGE_COMPUTE_BIT,
                                          0, sizeof(gradient_push), &gradient_push);
        VK_GET_SYMBOL(vkCmdDispatch)(cmd, DispatchGroups(flow_w >> l), DispatchGroups(flow_h >> l),
                                     1);
    }

    ComputeBarrier(cmd);

    for (u32 index = 0; index < level_count; index++) {
        const u32 l = coarse - index;
        const u32 level_w = flow_w >> l;
        const u32 level_h = flow_h >> l;
        const u32 sparse_w = SparseExtent(level_w);
        const u32 sparse_h = SparseExtent(level_h);

        DisInversePush inverse_push{};
        inverse_push.level = static_cast<s32>(l);
        inverse_push.coarse_level = static_cast<s32>(coarse);

        VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_inverse.Get());
        VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                               pipeline_layout.Get(), 0, 1,
                                               &inverse_sets[slot][l], 0, nullptr);
        VK_GET_SYMBOL(vkCmdPushConstants)(cmd, pipeline_layout.Get(), VK_SHADER_STAGE_COMPUTE_BIT,
                                          0, sizeof(inverse_push), &inverse_push);
        VK_GET_SYMBOL(vkCmdDispatch)(cmd, DispatchGroups(sparse_w), DispatchGroups(sparse_h), 1);

        ComputeBarrier(cmd);

        u32 prop_passes = PropStepsFor(l, level_count, refine.prop_floor);
        const u32 prop_doubling = prop_passes;
        if (prop_passes & 1u) prop_passes++;

        for (u32 k = 0; k < prop_passes; k++) {
            const VkDescriptorSet prop_set =
                (k & 1u) ? prop_ba_sets[slot][l] : prop_ab_sets[slot][l];

            DisPropPush prop_push{};
            prop_push.dist = k < prop_doubling ? static_cast<s32>(1u << k) : 1;

            VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                             pass_propagate.Get());
            VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   pipeline_layout.Get(), 0, 1, &prop_set, 0,
                                                   nullptr);
            VK_GET_SYMBOL(vkCmdPushConstants)(cmd, pipeline_layout.Get(),
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(prop_push),
                                              &prop_push);
            VK_GET_SYMBOL(vkCmdDispatch)(cmd, DispatchGroups(sparse_w), DispatchGroups(sparse_h),
                                         1);

            ComputeBarrier(cmd);
        }

        Dispatch(cmd, pass_densify.Get(), densify_sets[slot][l], level_w, level_h);

        ComputeBarrier(cmd);
    }

    const u32 groups_w = DispatchGroups(flow_w);
    const u32 groups_h = DispatchGroups(flow_h);

    VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_vr_prep.Get());
    VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                           vr_pipeline_layout.Get(), 0, 1, &vr_prep_sets[slot], 0,
                                           nullptr);
    VK_GET_SYMBOL(vkCmdDispatch)(cmd, groups_w, groups_h, 1);
    ComputeBarrier(cmd);

    VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_vr_d1.Get());
    VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                           vr_pipeline_layout.Get(), 0, 1, &vr_d1_set, 0, nullptr);
    VK_GET_SYMBOL(vkCmdDispatch)(cmd, groups_w, groups_h, 1);
    ComputeBarrier(cmd);

    VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_vr_d2.Get());
    VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                           vr_pipeline_layout.Get(), 0, 1, &vr_d2_set, 0, nullptr);
    VK_GET_SYMBOL(vkCmdDispatch)(cmd, groups_w, groups_h, 1);
    ComputeBarrier(cmd);

    for (u32 k = 0; k < refine.vr_fixed_point; k++) {
        DisVrWeightPush weight_push{};
        weight_push.alpha2 = DIS_VR_ALPHA * 0.5f;
        weight_push.eps2 = DIS_VR_EPS * DIS_VR_EPS;

        VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_vr_w.Get());
        VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                               vr_pipeline_layout.Get(), 0, 1, &vr_w_set, 0,
                                               nullptr);
        VK_GET_SYMBOL(vkCmdPushConstants)(cmd, vr_pipeline_layout.Get(),
                                          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(weight_push),
                                          &weight_push);
        VK_GET_SYMBOL(vkCmdDispatch)(cmd, groups_w, groups_h, 1);
        ComputeBarrier(cmd);

        DisVrCoefPush coef_push{};
        coef_push.delta2 = DIS_VR_DELTA * 0.5f;
        coef_push.gamma2 = DIS_VR_GAMMA * 0.5f;
        coef_push.zeta2 = DIS_VR_ZETA * DIS_VR_ZETA;
        coef_push.eps2 = DIS_VR_EPS * DIS_VR_EPS;

        VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_vr_coef.Get());
        VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                               vr_pipeline_layout.Get(), 0, 1, &vr_coef_set, 0,
                                               nullptr);
        VK_GET_SYMBOL(vkCmdPushConstants)(cmd, vr_pipeline_layout.Get(),
                                          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(coef_push),
                                          &coef_push);
        VK_GET_SYMBOL(vkCmdDispatch)(cmd, groups_w, groups_h, 1);
        ComputeBarrier(cmd);

        for (u32 sweep = 0; sweep < refine.vr_sor; sweep++) {
            DisVrSorPush sor_push{};
            sor_push.omega = DIS_VR_OMEGA;
            sor_push.parity = 0;

            VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                             pass_vr_sor.Get());
            VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   vr_pipeline_layout.Get(), 0, 1, &vr_sor_ab_set,
                                                   0, nullptr);
            VK_GET_SYMBOL(vkCmdPushConstants)(cmd, vr_pipeline_layout.Get(),
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sor_push),
                                              &sor_push);
            VK_GET_SYMBOL(vkCmdDispatch)(cmd, groups_w, groups_h, 1);
            ComputeBarrier(cmd);

            sor_push.parity = 1;
            VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   vr_pipeline_layout.Get(), 0, 1, &vr_sor_ba_set,
                                                   0, nullptr);
            VK_GET_SYMBOL(vkCmdPushConstants)(cmd, vr_pipeline_layout.Get(),
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sor_push),
                                              &sor_push);
            VK_GET_SYMBOL(vkCmdDispatch)(cmd, groups_w, groups_h, 1);
            ComputeBarrier(cmd);
        }
    }

    VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_vr_add.Get());
    VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                           vr_pipeline_layout.Get(), 0, 1, &vr_add_set, 0,
                                           nullptr);
    VK_GET_SYMBOL(vkCmdDispatch)(cmd, groups_w, groups_h, 1);

    ComputeBarrier(cmd);
}

void DisFlow::RenderInto(VkCommandBuffer cmd, float timestamp, VkImage target_image, u32 width,
                         u32 height) {
    const u32 content_w = built_full_extent.width;
    const u32 content_h = built_full_extent.height;

    DisInterpPush interp_push{};
    interp_push.timestamp = timestamp;
    interp_push.debug_mode = 0;

    VK_GET_SYMBOL(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_interp.Get());
    VK_GET_SYMBOL(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                           pipeline_layout.Get(), 0, 1, &interp_sets[active_slot],
                                           0, nullptr);
    VK_GET_SYMBOL(vkCmdPushConstants)(cmd, pipeline_layout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(interp_push), &interp_push);
    VK_GET_SYMBOL(vkCmdDispatch)(cmd, DispatchGroups(content_w), DispatchGroups(content_h), 1);

    ImageBarrier(cmd, interp_out.Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    ImageBarrier(cmd, target_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);

    BlitRect(cmd, interp_out.Handle(), 0, 0, content_w, content_h, target_image, 0, 0, width,
             height, VK_FILTER_LINEAR);

    ImageBarrier(cmd, interp_out.Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
}

void DisFlow::GenerateInto(VkCommandBuffer cmd, u32 generation, VkImage target_image, u32 width,
                           u32 height) {
    if (!built || !Valid()) return;
    if (last_generations == 0) return;
    if (width == 0 || height == 0 || target_image == VK_NULL_HANDLE) return;

    const float timestamp =
        static_cast<float>(generation + 1) / static_cast<float>(last_generations + 1);
    RenderInto(cmd, timestamp, target_image, width, height);
}

void DisFlow::ForgetTargets() {
}

void DisFlow::Reset() {
    frame_count = 0;
    last_generations = 0;
    active_slot = 0;
}

}

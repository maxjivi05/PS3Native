// SPDX-FileCopyrightText: Copyright 2026 qwertypower (DEVAR Entertainment LLC)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "../VulkanAPI.h"
#include "util/types.hpp"

namespace vk {
class render_device;
}

namespace dis {

constexpr u32 DIS_MAX_GENERATIONS = 3;
constexpr u32 DIS_LOCAL_SIZE = 8;
constexpr u32 DIS_PATCH_STRIDE = 3;
constexpr u32 DIS_MIN_EXTENT = 16;
constexpr u32 DIS_DEFAULT_FLOW_MIN_SIDE = 180;
constexpr u32 DIS_FLOW_MIN_SIDE_FLOOR = 64;
constexpr u32 DIS_FLOW_MIN_SIDE_CEIL = 1080;
constexpr u32 DIS_MAX_LEVELS = 8;
constexpr u32 DIS_SLOTS = 6;
constexpr u32 DIS_PROP_STEPS_MAX = 4;

constexpr u32 DIS_SET_SAMPLERS = 5;
constexpr u32 DIS_SET_STORAGE = 1;
constexpr u32 DIS_SHARED_SETS_PER_LEVEL = 6;
constexpr u32 DIS_VR_SHARED_SETS = 7;
constexpr u32 DIS_VR_SAMPLER_BINDINGS = 8;
constexpr u32 DIS_VR_STORAGE_BINDINGS = 2;
constexpr u32 DIS_VR_FIRST_STORAGE = 8;

constexpr u32 DIS_PUSH_CONSTANT_SIZE = 32;

constexpr float DIS_VR_ALPHA = 20.0f;
constexpr float DIS_VR_DELTA = 5.0f;
constexpr float DIS_VR_GAMMA = 10.0f;
constexpr float DIS_VR_OMEGA = 1.6f;
constexpr float DIS_VR_ZETA = 0.1f;
constexpr float DIS_VR_EPS = 0.001f;

constexpr VkFormat DIS_FLOW_FORMAT = VK_FORMAT_R32G32_SFLOAT;
constexpr VkFormat DIS_SPARSE_FORMAT = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat DIS_WEIGHT_FORMAT = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat DIS_OUTPUT_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

struct DisRefine {
    u32 vr_fixed_point;
    u32 vr_sor;
    u32 prop_floor;
};

[[nodiscard]] constexpr DisRefine RefineFor(u32 generations) {
    if (generations >= 3) return DisRefine{2, 5, 2};
    if (generations == 2) return DisRefine{2, 4, 1};
    return DisRefine{1, 3, 1};
}

[[nodiscard]] constexpr VkExtent2D FlowExtentFor(u32 min_side, u32 content_w, u32 content_h) {
    const u32 minor = content_w < content_h ? content_w : content_h;
    if (minor == 0 || min_side == 0 || min_side >= minor) {
        return VkExtent2D{content_w, content_h};
    }
    const double k = static_cast<double>(min_side) / static_cast<double>(minor);
    return VkExtent2D{static_cast<u32>(static_cast<double>(content_w) * k + 0.5),
                      static_cast<u32>(static_cast<double>(content_h) * k + 0.5)};
}

[[nodiscard]] constexpr u32 LevelsFor(u32 width, u32 height) {
    u32 levels = 1;
    while ((width >> levels) >= 16 && (height >> levels) >= 16 && levels < DIS_MAX_LEVELS) {
        ++levels;
    }
    return levels;
}

[[nodiscard]] constexpr u32 SparseExtent(u32 extent) {
    return extent > 8 ? 1 + (extent - 8) / DIS_PATCH_STRIDE : 1;
}

[[nodiscard]] constexpr u32 PropStepsFor(u32 level, u32 levels, u32 floor_steps) {
    constexpr std::array<u32, DIS_PROP_STEPS_MAX> profile{4, 3, 2, 1};
    const u32 from_coarse = (levels - 1) - level;
    const u32 base = from_coarse < DIS_PROP_STEPS_MAX ? profile[from_coarse] : 1;
    return base > floor_steps ? base : floor_steps;
}

[[nodiscard]] constexpr u32 DispatchGroups(u32 extent) {
    return (extent + DIS_LOCAL_SIZE - 1) / DIS_LOCAL_SIZE;
}

class Device {
public:
    Device() = default;
    Device(VkDevice device_, VkPhysicalDevice physical_device_);

    [[nodiscard]] VkDevice Handle() const {
        return device;
    }

    [[nodiscard]] VkPhysicalDevice Gpu() const {
        return physical_device;
    }

    [[nodiscard]] u32 FindMemoryType(u32 bits, VkMemoryPropertyFlags properties) const;

private:
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_properties{};
};

enum class DisObjectKind {
    sampler,
    set_layout,
    pipeline_layout,
    descriptor_pool,
    pipeline,
    image_view
};

template <typename Handle, DisObjectKind Kind>
class DisHandle {
public:
    DisHandle() = default;
    DisHandle(VkDevice device_, Handle handle_) : device{device_}, handle{handle_} {}

    ~DisHandle() {
        Release();
    }

    DisHandle(const DisHandle&) = delete;
    DisHandle& operator=(const DisHandle&) = delete;

    DisHandle(DisHandle&& other) noexcept : device{other.device}, handle{other.handle} {
        other.device = VK_NULL_HANDLE;
        other.handle = VK_NULL_HANDLE;
    }

    DisHandle& operator=(DisHandle&& other) noexcept {
        if (this != &other) {
            Release();
            device = other.device;
            handle = other.handle;
            other.device = VK_NULL_HANDLE;
            other.handle = VK_NULL_HANDLE;
        }
        return *this;
    }

    [[nodiscard]] Handle Get() const {
        return handle;
    }

    [[nodiscard]] bool Valid() const {
        return handle != VK_NULL_HANDLE;
    }

private:
    void Release() {
        if (device != VK_NULL_HANDLE && handle != VK_NULL_HANDLE) {
            if constexpr (Kind == DisObjectKind::sampler) {
                VK_GET_SYMBOL(vkDestroySampler)(device, handle, nullptr);
            } else if constexpr (Kind == DisObjectKind::set_layout) {
                VK_GET_SYMBOL(vkDestroyDescriptorSetLayout)(device, handle, nullptr);
            } else if constexpr (Kind == DisObjectKind::pipeline_layout) {
                VK_GET_SYMBOL(vkDestroyPipelineLayout)(device, handle, nullptr);
            } else if constexpr (Kind == DisObjectKind::descriptor_pool) {
                VK_GET_SYMBOL(vkDestroyDescriptorPool)(device, handle, nullptr);
            } else if constexpr (Kind == DisObjectKind::pipeline) {
                VK_GET_SYMBOL(vkDestroyPipeline)(device, handle, nullptr);
            } else if constexpr (Kind == DisObjectKind::image_view) {
                VK_GET_SYMBOL(vkDestroyImageView)(device, handle, nullptr);
            }
        }
        device = VK_NULL_HANDLE;
        handle = VK_NULL_HANDLE;
    }

    VkDevice device{VK_NULL_HANDLE};
    Handle handle{VK_NULL_HANDLE};
};

using DisSampler = DisHandle<VkSampler, DisObjectKind::sampler>;
using DisSetLayout = DisHandle<VkDescriptorSetLayout, DisObjectKind::set_layout>;
using DisPipelineLayout = DisHandle<VkPipelineLayout, DisObjectKind::pipeline_layout>;
using DisDescriptorPool = DisHandle<VkDescriptorPool, DisObjectKind::descriptor_pool>;
using DisPipeline = DisHandle<VkPipeline, DisObjectKind::pipeline>;
using DisImageView = DisHandle<VkImageView, DisObjectKind::image_view>;

class DisImage {
public:
    DisImage() = default;
    DisImage(const Device& device_, u32 width, u32 height, VkFormat format_, u32 mip_levels_,
             VkImageUsageFlags usage);
    ~DisImage();

    DisImage(const DisImage&) = delete;
    DisImage& operator=(const DisImage&) = delete;
    DisImage(DisImage&& other) noexcept;
    DisImage& operator=(DisImage&& other) noexcept;

    [[nodiscard]] VkImage Handle() const {
        return image;
    }

    [[nodiscard]] VkExtent2D Extent() const {
        return extent;
    }

    [[nodiscard]] VkFormat Format() const {
        return format;
    }

    [[nodiscard]] u32 MipLevels() const {
        return mip_levels;
    }

    [[nodiscard]] bool Valid() const {
        return image != VK_NULL_HANDLE;
    }

private:
    void Release();

    VkDevice device{VK_NULL_HANDLE};
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    u32 mip_levels{};
};

class DisFlow {
public:
    explicit DisFlow(const vk::render_device& device_);
    ~DisFlow();

    DisFlow(const DisFlow&) = delete;
    DisFlow& operator=(const DisFlow&) = delete;
    DisFlow(DisFlow&&) = delete;
    DisFlow& operator=(DisFlow&&) = delete;

    bool Configure(u32 flow_min_side_, u32 target_fps_, float refresh_rate_);

    [[nodiscard]] bool NeedsRebuild(u32 width, u32 height, VkFormat format) const;

    bool Prepare(u32 width, u32 height, VkFormat format);

    void Process(VkCommandBuffer cmd, VkImage source, u32 width, u32 height, u32 generations);

    void GenerateInto(VkCommandBuffer cmd, u32 generation, u32 target_index, VkImage target_image,
                      VkImageView target_view, u32 width, u32 height, VkImage base_image);

    void ForgetTargets();

    void Reset();

    [[nodiscard]] bool Valid() const {
        return ready && !unavailable;
    }

    [[nodiscard]] VkExtent2D FlowExtent() const {
        return built_extent;
    }

    [[nodiscard]] u32 Levels() const {
        return levels;
    }

private:
    [[nodiscard]] VkFormat PickLumaFormat() const;
    bool AuditFormats();
    bool CreateSampler();
    bool CreatePipelines();
    bool AllocateSets();
    bool CreateResources(u32 width, u32 height, u32 full_width, u32 full_height, VkFormat format);
    void DestroyResources();
    void WriteAllDescriptors();
    void PrimeLayouts(VkCommandBuffer cmd);
    void Dispatch(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, u32 width,
                  u32 height) const;
    void RenderInto(VkCommandBuffer cmd, float timestamp, VkImage target_image, u32 width,
                    u32 height, VkImage base_image);

    Device device;

    u32 flow_min_side{DIS_DEFAULT_FLOW_MIN_SIDE};
    u32 target_fps{};
    float refresh_rate{};

    VkExtent2D built_extent{};
    VkExtent2D built_full_extent{};
    u32 built_min_side{};
    VkFormat built_format{VK_FORMAT_UNDEFINED};
    VkFormat luma_format{VK_FORMAT_UNDEFINED};
    u32 levels{};

    bool ready{};
    bool built{};
    bool unavailable{};
    bool layouts_primed{};
    bool formats_audited{};
    bool manual_flow_filter{};
    bool shader_load_failed{};

    std::array<DisImage, DIS_SLOTS> color;
    std::array<DisImage, DIS_SLOTS> flow_color;
    std::array<DisImage, DIS_SLOTS> flow_luma;
    DisImage grad;
    std::array<DisImage, DIS_MAX_LEVELS> flow_sparse;
    std::array<DisImage, DIS_MAX_LEVELS> flow_sparse_b;
    DisImage flow_dense;
    DisImage interp_out;
    DisImage vr_prep;
    DisImage vr_d1;
    DisImage vr_d2;
    DisImage vr_a;
    DisImage vr_b;
    DisImage vr_wt;
    std::array<DisImage, 2> vr_dw;
    DisImage flow_refined;

    std::array<DisImageView, DIS_SLOTS> view_color;
    std::array<std::array<DisImageView, DIS_MAX_LEVELS>, DIS_SLOTS> view_flow_color;
    std::array<std::array<DisImageView, DIS_MAX_LEVELS>, DIS_SLOTS> view_flow_luma;
    std::array<DisImageView, DIS_MAX_LEVELS> view_grad;
    std::array<DisImageView, DIS_MAX_LEVELS> view_sparse;
    std::array<DisImageView, DIS_MAX_LEVELS> view_sparse_b;
    std::array<DisImageView, DIS_MAX_LEVELS> view_dense;
    DisImageView view_interp_out;
    DisImageView view_vr_prep;
    DisImageView view_vr_d1;
    DisImageView view_vr_d2;
    DisImageView view_vr_a;
    DisImageView view_vr_b;
    DisImageView view_vr_wt;
    std::array<DisImageView, 2> view_vr_dw;
    DisImageView view_flow_refined;

    DisSampler sampler;
    DisSetLayout set_layout;
    DisPipelineLayout pipeline_layout;
    DisSetLayout vr_set_layout;
    DisPipelineLayout vr_pipeline_layout;
    DisDescriptorPool pool;

    std::array<std::array<VkDescriptorSet, DIS_MAX_LEVELS>, DIS_SLOTS> luma_sets{};
    std::array<std::array<VkDescriptorSet, DIS_MAX_LEVELS>, DIS_SLOTS> grad_sets{};
    std::array<std::array<VkDescriptorSet, DIS_MAX_LEVELS>, DIS_SLOTS> inverse_sets{};
    std::array<std::array<VkDescriptorSet, DIS_MAX_LEVELS>, DIS_SLOTS> densify_sets{};
    std::array<std::array<VkDescriptorSet, DIS_MAX_LEVELS>, DIS_SLOTS> prop_ab_sets{};
    std::array<std::array<VkDescriptorSet, DIS_MAX_LEVELS>, DIS_SLOTS> prop_ba_sets{};
    std::array<VkDescriptorSet, DIS_SLOTS> interp_sets{};
    std::array<VkDescriptorSet, DIS_SLOTS> vr_prep_sets{};
    VkDescriptorSet vr_d1_set{VK_NULL_HANDLE};
    VkDescriptorSet vr_d2_set{VK_NULL_HANDLE};
    VkDescriptorSet vr_w_set{VK_NULL_HANDLE};
    VkDescriptorSet vr_coef_set{VK_NULL_HANDLE};
    VkDescriptorSet vr_sor_ab_set{VK_NULL_HANDLE};
    VkDescriptorSet vr_sor_ba_set{VK_NULL_HANDLE};
    VkDescriptorSet vr_add_set{VK_NULL_HANDLE};

    DisPipeline pass_luma;
    DisPipeline pass_gradient;
    DisPipeline pass_inverse;
    DisPipeline pass_propagate;
    DisPipeline pass_densify;
    DisPipeline pass_interp;
    DisPipeline pass_vr_prep;
    DisPipeline pass_vr_d1;
    DisPipeline pass_vr_d2;
    DisPipeline pass_vr_w;
    DisPipeline pass_vr_coef;
    DisPipeline pass_vr_sor;
    DisPipeline pass_vr_add;

    u64 frame_count{};
    u32 active_slot{};
    u32 last_generations{};
};

}

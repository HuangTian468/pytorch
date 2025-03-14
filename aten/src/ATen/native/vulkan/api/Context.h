#pragma once

#ifdef USE_VULKAN_API

#include <ATen/native/vulkan/api/Adapter.h>
#include <ATen/native/vulkan/api/Command.h>
#include <ATen/native/vulkan/api/Common.h>
#include <ATen/native/vulkan/api/Descriptor.h>
#include <ATen/native/vulkan/api/Pipeline.h>
#include <ATen/native/vulkan/api/QueryPool.h>
#include <ATen/native/vulkan/api/Resource.h>
#include <ATen/native/vulkan/api/Runtime.h>
#include <ATen/native/vulkan/api/Shader.h>

namespace at {
namespace native {
namespace vulkan {
namespace api {

struct ContextConfig final {
  uint32_t cmdSubmitFrequency;
  CommandPoolConfig cmdPoolConfig;
  DescriptorPoolConfig descriptorPoolConfig;
  QueryPoolConfig queryPoolConfig;
};

//
// Vulkan Context holds onto all relevant Vulkan state as it pertains to our
// use of Vulkan in PyTorch.  A Context is associated with one, and only one,
// Adapter as a precursor to multi-GPU support.  All Vulkan tensors in PyTorch
// are associated with a Context to make tensor <-> device affinity explicit.
// The context is currently a global object, but technically it does not need
// to be if we were to make it explicit to the user.
//

class Context final {
 public:
  explicit Context(size_t adapter_i, const ContextConfig&);

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  Context(Context&&) = delete;
  Context& operator=(Context&&) = delete;

  ~Context();

 private:
  // Config
  ContextConfig config_;
  // Important handles
  Adapter* adapter_p_;
  VkDevice device_;
  Adapter::Queue queue_;
  // Resource Pools
  CommandPool command_pool_;
  DescriptorPool descriptor_pool_;
  FencePool fences_;
  // Diagnostics
  // TODO: remove USE_VULKAN_GPU_DIAGNOSTICS
  bool enable_op_profiling_{false};
#ifdef USE_VULKAN_GPU_DIAGNOSTICS
  QueryPool querypool_;
#endif /* USE_VULKAN_GPU_DIAGNOSTICS */
  // Command buffers submission
  std::mutex cmd_mutex_;
  CommandBuffer cmd_;
  uint32_t submit_count_;
  // Memory Management
  std::mutex buffer_clearlist_mutex_;
  std::vector<VulkanBuffer> buffers_to_clear_;
  std::mutex image_clearlist_mutex_;
  std::vector<VulkanImage> images_to_clear_;

 public:
  // Adapter access

  inline Adapter* adapter_ptr() {
    return adapter_p_;
  }

  inline void enable_op_profiling() {
    enable_op_profiling_ = true;
  }

  inline bool op_profiling_enabled() {
    return enable_op_profiling_;
  }

  inline VkDevice device() {
    return device_;
  }

  inline VkQueue queue() {
    return queue_.handle;
  }

  // Device Caches

  inline ShaderLayoutCache& shader_layout_cache() {
    return adapter_ptr()->shader_layout_cache();
  }

  inline ShaderCache& shader_cache() {
    return adapter_ptr()->shader_cache();
  }

  inline PipelineLayoutCache& pipeline_layout_cache() {
    return adapter_ptr()->pipeline_layout_cache();
  }

  inline ComputePipelineCache& pipeline_cache() {
    return adapter_ptr()->compute_pipeline_cache();
  }

  // Resource Pools

  inline DescriptorPool& descriptor_pool() {
    return descriptor_pool_;
  }

  inline FencePool& fences() {
    return fences_;
  }

  // Diagnostics

#ifdef USE_VULKAN_GPU_DIAGNOSTICS
  inline QueryPool& querypool() {
    return querypool_;
  }

  inline void reset_querypool() {
    set_cmd();
    querypool_.reset(cmd_);
  }
#endif /* USE_VULKAN_GPU_DIAGNOSTICS */

  // Memory Management
  void register_buffer_cleanup(VulkanBuffer& buffer) {
    std::lock_guard<std::mutex> bufferlist_lock(buffer_clearlist_mutex_);
    buffers_to_clear_.emplace_back(std::move(buffer));
  }

  void register_image_cleanup(VulkanImage& image) {
    std::lock_guard<std::mutex> imagelist_lock(image_clearlist_mutex_);
    images_to_clear_.emplace_back(std::move(image));
  }

  // GPU RPC

  inline std::unique_lock<std::mutex> dispatch_lock() {
    return std::unique_lock<std::mutex>(cmd_mutex_);
  }

 private:
  inline void set_cmd() {
    if (!cmd_) {
      cmd_ = command_pool_.get_new_cmd();
      cmd_.begin();
    }
  }

  DescriptorSet submit_compute_prologue(
      CommandBuffer&,
      const ShaderSource&,
      const utils::uvec3&);

  void submit_compute_epilogue(
      CommandBuffer&,
      const DescriptorSet&,
      const PipelineBarrier&,
      const utils::uvec3&);

 public:
  template <class S, class D>
  void submit_copy(
      const PipelineBarrier&,
      const S&,
      const D&,
      const api::utils::uvec3&,
      const api::utils::uvec3&,
      const api::utils::uvec3&,
      const VkFence fence_handle);

  template <typename... Arguments>
  void submit_compute_job(
      const ShaderSource&,
      const PipelineBarrier&,
      const utils::uvec3&,
      const utils::uvec3&,
      const VkFence fence_handle,
      Arguments&&...);

 private:
  void submit_cmd_to_gpu(const VkFence fence_handle = VK_NULL_HANDLE);

 public:
  void flush();
};

class UniformParamsBuffer final {
 private:
  Context* context_p_;
  VulkanBuffer vulkan_buffer_;

 public:
  template <typename Block>
  UniformParamsBuffer(Context* context_p, const Block& block)
      : context_p_(context_p),
        vulkan_buffer_(
            context_p_->adapter_ptr()->vma().create_params_buffer(block)) {}

  UniformParamsBuffer(const UniformParamsBuffer&) = delete;
  UniformParamsBuffer& operator=(const UniformParamsBuffer&) = delete;

  UniformParamsBuffer(UniformParamsBuffer&&) = default;
  UniformParamsBuffer& operator=(UniformParamsBuffer&&) = default;

  ~UniformParamsBuffer() {
    if (vulkan_buffer_) {
      context_p_->register_buffer_cleanup(vulkan_buffer_);
    }
  }

  VulkanBuffer& buffer() {
    return vulkan_buffer_;
  }
};

class StorageBuffer final {
 private:
  Context* context_p_;
  c10::ScalarType dtype_;
  size_t numel_;
  VulkanBuffer vulkan_buffer_;

 public:
  StorageBuffer(
      Context* context_p,
      const c10::ScalarType dtype,
      const size_t numel,
      const bool gpuonly = false)
      : context_p_(context_p),
        dtype_(dtype),
        numel_(numel),
        vulkan_buffer_(context_p_->adapter_ptr()->vma().create_storage_buffer(
            c10::elementSize(dtype_) * numel_,
            gpuonly)) {}

  StorageBuffer(const StorageBuffer&) = delete;
  StorageBuffer& operator=(const StorageBuffer&) = delete;

  StorageBuffer(StorageBuffer&&) = delete;
  StorageBuffer& operator=(StorageBuffer&&) = delete;

  ~StorageBuffer() {
    context_p_->register_buffer_cleanup(vulkan_buffer_);
  }

  VulkanBuffer& buffer() {
    return vulkan_buffer_;
  }
};

bool available();

// The global runtime is retrieved using this function, where it is declared as
// a static local variable.
Context* context();

namespace detail {

template <size_t... Indices, typename... Arguments>
inline void bind(
    DescriptorSet& descriptor_set,
    const std::index_sequence<Indices...>,
    Arguments&&... arguments) {
  C10_UNUSED const int _[]{
      0,
      (descriptor_set.bind(Indices, std::forward<Arguments>(arguments)), 0)...,
  };
}

} // namespace detail

template <class S, class D>
inline void record_copy(
    CommandBuffer& cmd,
    const S& source,
    const D& destination,
    const api::utils::uvec3& copy_range,
    const api::utils::uvec3& src_offset,
    const api::utils::uvec3& dst_offset) = delete;

template <>
inline void record_copy<VulkanBuffer, VulkanBuffer>(
    CommandBuffer& cmd,
    const VulkanBuffer& source,
    const VulkanBuffer& destination,
    const api::utils::uvec3& copy_range,
    const api::utils::uvec3& src_offset,
    const api::utils::uvec3& dst_offset) {
  cmd.copy_buffer_to_buffer(
      source, destination, copy_range, src_offset, dst_offset);
}

template <>
inline void record_copy<VulkanImage, VulkanImage>(
    CommandBuffer& cmd,
    const VulkanImage& source,
    const VulkanImage& destination,
    const api::utils::uvec3& copy_range,
    const api::utils::uvec3& src_offset,
    const api::utils::uvec3& dst_offset) {
  cmd.copy_texture_to_texture(
      source, destination, copy_range, src_offset, dst_offset);
}

template <>
inline void record_copy<VulkanImage, VulkanBuffer>(
    CommandBuffer& cmd,
    const VulkanImage& source,
    const VulkanBuffer& destination,
    const api::utils::uvec3& copy_range,
    const api::utils::uvec3& src_offset,
    const api::utils::uvec3& dst_offset) {
  cmd.copy_texture_to_buffer(
      source, destination, copy_range, src_offset, dst_offset);
}

template <>
inline void record_copy<VulkanBuffer, VulkanImage>(
    CommandBuffer& cmd,
    const VulkanBuffer& source,
    const VulkanImage& destination,
    const api::utils::uvec3& copy_range,
    const api::utils::uvec3& src_offset,
    const api::utils::uvec3& dst_offset) {
  cmd.copy_buffer_to_texture(
      source, destination, copy_range, src_offset, dst_offset);
}

template <class S, class D>
inline void Context::submit_copy(
    const PipelineBarrier& pipeline_barrier,
    const S& source,
    const D& destination,
    const api::utils::uvec3& copy_range,
    const api::utils::uvec3& src_offset,
    const api::utils::uvec3& dst_offset,
    const VkFence fence_handle) {
  // Serialize recording to the shared command buffer. Do not initialize with a
  // mutex just yet, since in some cases it will be externally managed.
  std::unique_lock<std::mutex> cmd_lock;
  // Refer to comments in submit_compute_job for explanation.
  if (fence_handle == VK_NULL_HANDLE) {
    cmd_lock = std::unique_lock<std::mutex>(cmd_mutex_);
  }

  set_cmd();

#ifdef USE_VULKAN_GPU_DIAGNOSTICS
  uint32_t log_idx = UINT32_MAX;
  if (enable_op_profiling_) {
    std::string label = "cmd_copy";
    log_idx = querypool_.shader_profile_begin(
        cmd_, label, create_extent3d({0, 0, 0}), create_extent3d({0, 0, 0}));
  }
#endif /* USE_VULKAN_GPU_DIAGNOSTICS */

  cmd_.insert_barrier(pipeline_barrier);

  record_copy(cmd_, source, destination, copy_range, src_offset, dst_offset);

#ifdef USE_VULKAN_GPU_DIAGNOSTICS
  if (enable_op_profiling_) {
    querypool_.shader_profile_end(cmd_, log_idx);
  }
#endif /* USE_VULKAN_GPU_DIAGNOSTICS */

  submit_count_++;
  if (fence_handle != VK_NULL_HANDLE ||
      submit_count_ >= config_.cmdSubmitFrequency) {
    submit_cmd_to_gpu(fence_handle);
  }
}

template <typename... Arguments>
inline void Context::submit_compute_job(
    const ShaderSource& shader,
    const PipelineBarrier& pipeline_barrier,
    const utils::uvec3& global_work_group,
    const utils::uvec3& local_work_group_size,
    const VkFence fence_handle,
    Arguments&&... arguments) {
  // Serialize recording to the shared command buffer. Do not initialize with a
  // mutex just yet, since in some cases it will be externally managed.
  std::unique_lock<std::mutex> cmd_lock;
  // If a fence was passed, then assume that the host intends to sync with
  // the GPU, implying there will be imminent calls to fence.wait() and flush().
  // We therefore assume the mutex is externally managed in this case, and the
  // calling thread has already locked the mutex prior to calling the function,
  // and will release the mutex manually after calling flush(). This will
  // prevent more dispatches from being recorded until we have flushed the
  // Context.
  if (fence_handle == VK_NULL_HANDLE) {
    cmd_lock = std::unique_lock<std::mutex>(cmd_mutex_);
  }

  set_cmd();

#ifdef USE_VULKAN_GPU_DIAGNOSTICS
  uint32_t log_idx = UINT32_MAX;
  if (enable_op_profiling_) {
    log_idx = querypool_.shader_profile_begin(
        cmd_,
        shader.kernel_name,
        create_extent3d(global_work_group),
        create_extent3d(local_work_group_size));
  }
#endif /* USE_VULKAN_GPU_DIAGNOSTICS */

  // Factor out template parameter independent code to minimize code bloat.
  DescriptorSet descriptor_set =
      submit_compute_prologue(cmd_, shader, local_work_group_size);

  detail::bind(
      descriptor_set,
      std::index_sequence_for<Arguments...>{},
      std::forward<Arguments>(arguments)...);

  // Adjust the global workgroup size based on the output tile size
  const utils::uvec3 effective_global_wg = {
      utils::div_up(global_work_group.data[0u], shader.out_tile_size.data[0u]),
      utils::div_up(global_work_group.data[1u], shader.out_tile_size.data[1u]),
      utils::div_up(global_work_group.data[2u], shader.out_tile_size.data[2u]),
  };

  // Factor out template parameter independent code to minimize code bloat.
  submit_compute_epilogue(
      cmd_, descriptor_set, pipeline_barrier, effective_global_wg);

#ifdef USE_VULKAN_GPU_DIAGNOSTICS
  if (enable_op_profiling_) {
    querypool_.shader_profile_end(cmd_, log_idx);
  }
#endif /* USE_VULKAN_GPU_DIAGNOSTICS */

  submit_count_++;
  if (fence_handle != VK_NULL_HANDLE ||
      submit_count_ >= config_.cmdSubmitFrequency) {
    submit_cmd_to_gpu(fence_handle);
  }
}

} // namespace api
} // namespace vulkan
} // namespace native
} // namespace at

#endif /* USE_VULKAN_API */

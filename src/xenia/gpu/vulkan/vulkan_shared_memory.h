/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_
#define XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/gpu/shared_memory.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, Memory& memory,
                     TraceWriter& trace_writer,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  void CompletedSubmissionUpdated();
  void EndSubmission();

  enum class Usage {
    // Index buffer, vfetch, compute read, transfer source.
    kRead,
    // Index buffer, vfetch, memexport.
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  // Inserts a pipeline barrier for the target usage, also ensuring consecutive
  // read-write accesses are ordered with each other.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {});
  // For a guest draw, given as (start, length) ranges it reads (vertex and
  // index data) and writes (memory export). Orders it after earlier draws only
  // where they conflict: reading what one wrote, or writing what one read or
  // wrote. With vulkan_order_draws_by_guest_sync, only accesses from before
  // the last guest wait or event (OnGuestSync) count, as on the console:
  // export writes bypass its vertex cache, so a title has to synchronize to
  // use them. Export streams and vertex buffers name whole buffers, so draws
  // using parts of one look like they conflict. A barrier ends the render
  // pass, and Use orders a draw after any draw that wrote, so the 160 or so
  // export draws Halo 3 skins its characters with every frame, each updating
  // its own slice of one vertex buffer, would otherwise take one each.
  void UseForDraw(const std::pair<uint32_t, uint32_t>* reads, size_t read_count,
                  const std::pair<uint32_t, uint32_t>* writes,
                  size_t write_count);
  // At a guest wait or event packet.
  void OnGuestSync() {
    if (!last_use_was_draw_) {
      return;
    }
    synced_draw_reads_.Add(interval_draw_reads_);
    synced_draw_writes_.Add(interval_draw_writes_);
    interval_draw_reads_.clear();
    interval_draw_writes_.clear();
  }
  // Barriers UseForDraw placed and skipped since the last call.
  std::pair<uint64_t, uint64_t> TakeDrawBarrierCounts() {
    std::pair<uint64_t, uint64_t> counts(draw_barriers_placed_,
                                         draw_barriers_skipped_);
    draw_barriers_placed_ = 0;
    draw_barriers_skipped_ = 0;
    return counts;
  }

  VkBuffer buffer() const { return buffer_; }

  // A host-imported (guest RAM) copy of the buffer, or VK_NULL_HANDLE if
  // unavailable. Bound instead of buffer() for memexport-touching draws, and
  // the destination of resolve readback, so their output is coherent with the
  // CPU (no clobber) - it aliases guest RAM.
  VkBuffer host_buffer() const { return host_buffer_; }

  // True when the buffer aliases guest RAM directly
  // (VK_EXT_external_memory_host import). Uploads and memexport readback copies
  // are then unnecessary.
  bool is_zero_copy() const { return zero_copy_; }

  // Returns true if any downloads were submitted to the command processor.
  bool InitializeTraceSubmitDownloads();
  void InitializeTraceCompleteDownloads();

  // Upload batches recorded and bytes copied into the buffer, cumulative, for
  // the log_fps report.
  uint64_t upload_batches() const { return upload_batches_; }
  uint64_t upload_bytes() const { return upload_bytes_; }
  uint64_t hoisted_upload_batches() const { return hoisted_upload_batches_; }

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_ranges) override;

 private:
  uint64_t upload_batches_ = 0;
  uint64_t upload_bytes_ = 0;
  uint64_t hoisted_upload_batches_ = 0;
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;

  VulkanCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // Creates a non-sparse buffer bound to imported guest RAM
  // (VK_EXT_external_memory_host). Returns true and fills out_buffer/out_memory
  // on success; false (nothing to clean up) otherwise.
  bool CreateImportedGuestRamBuffer(VkBuffer& out_buffer,
                                    VkDeviceMemory& out_memory);
  // Attempts to alias guest RAM as the whole buffer (full zero-copy). Returns
  // true on success (buffer_/buffer_memory_ populated, zero_copy_ set); false
  // to fall back to the normal device-local path.
  bool TryInitializeZeroCopy();
  // Attempts to create host_buffer_ for the hybrid two-buffer path. No-op when
  // enable_host_buffer is off or on failure (host_buffer_ stays null and
  // memexport and resolve readback go through staging copies instead).
  void TryInitializeHostBuffer();

  VkBuffer buffer_ = VK_NULL_HANDLE;
  uint32_t buffer_memory_type_;
  // Single for non-sparse, every allocation so far for sparse.
  std::vector<VkDeviceMemory> buffer_memory_;
  // Buffer memory is imported guest RAM - no uploads or readback copies needed.
  bool zero_copy_ = false;

  // Second buffer aliasing guest RAM, bound for memexport-touching draws and
  // written by resolve readback.
  VkBuffer host_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory host_buffer_memory_ = VK_NULL_HANDLE;

  Usage last_usage_;
  std::pair<uint32_t, uint32_t> last_written_range_;

  // Disjoint, sorted [start, end) ranges, merged as they're added.
  class RangeSet {
   public:
    bool Overlaps(uint32_t start, uint32_t end) const {
      auto it = std::upper_bound(
          ranges_.cbegin(), ranges_.cend(), start,
          [](uint32_t value, const std::pair<uint32_t, uint32_t>& range) {
            return value < range.second;
          });
      return it != ranges_.cend() && it->first < end;
    }
    void Add(uint32_t start, uint32_t end) {
      auto first =
          std::lower_bound(ranges_.begin(), ranges_.end(), start,
                           [](const std::pair<uint32_t, uint32_t>& range,
                              uint32_t value) { return range.second < value; });
      auto last = first;
      for (; last != ranges_.end() && last->first <= end; ++last) {
        start = std::min(start, last->first);
        end = std::max(end, last->second);
      }
      ranges_.insert(ranges_.erase(first, last), std::make_pair(start, end));
    }
    bool empty() const { return ranges_.empty(); }
    size_t size() const { return ranges_.size(); }
    std::pair<uint32_t, uint32_t> extent() const {
      return std::make_pair(ranges_.front().first, ranges_.back().second);
    }
    void clear() { ranges_.clear(); }
    void Add(const RangeSet& other) {
      for (const std::pair<uint32_t, uint32_t>& range : other.ranges_) {
        Add(range.first, range.second);
      }
    }

   private:
    std::vector<std::pair<uint32_t, uint32_t>> ranges_;
  };
  // Past this many ranges, a conflict is assumed rather than searched for.
  static constexpr size_t kMaxDrawRanges = 1024;
  // What the draws since the last barrier read and wrote, while the last use
  // was a draw: since the last guest sync, and before it.
  bool last_use_was_draw_ = false;
  RangeSet interval_draw_reads_;
  RangeSet interval_draw_writes_;
  RangeSet synced_draw_reads_;
  RangeSet synced_draw_writes_;
  void ClearDrawAccesses() {
    interval_draw_reads_.clear();
    interval_draw_writes_.clear();
    synced_draw_reads_.clear();
    synced_draw_writes_.clear();
  }
  uint64_t draw_barriers_placed_ = 0;
  uint64_t draw_barriers_skipped_ = 0;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;
  // Upload ranges left after the memexport ones are refreshed on the GPU.
  std::vector<std::pair<uint32_t, uint32_t>> cpu_upload_ranges_;

  // Created temporarily, only for downloading.
  VkBuffer trace_download_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory trace_download_buffer_memory_ = VK_NULL_HANDLE;
  void ResetTraceDownload();
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_

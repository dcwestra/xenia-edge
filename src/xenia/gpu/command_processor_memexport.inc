/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Two-buffer memexport page tracking, shared by the D3D12 and Vulkan command
// processors. This is #included into each backend's class body (like the pm4
// command processor sections) so each backend gets its own inlinable copy.
// TakeMemexportSignalSubmission, EndSubmissionForHeldSignals and
// AwaitSubmissionForHeldSignals implement CommandProcessor's virtuals. The
// include site must already provide
// SharedMemory, Shader, RegisterFile, xenos, xe::bit_scan_forward, <array>,
// <atomic> and <vector>.
// No include guard: this is injected into a class body, not a standalone
// header.
//
// It must also define, before including this:
//   void FlushMemexportStagingReadback()
//     Copies export output the backend staged for readback into guest RAM, now
//     that the wait has made it available. Empty where output already lands
//     there, which is zero-copy and the host buffer.
//   void AwaitMemexportSubmission(uint64_t submission)
//     Waits for a submission to complete, ending it first if it is the one
//     still being recorded.
//   bool CanHoldSignalsForMemexport() const
//     Whether export output reaches guest RAM by itself once its submission
//     completes so a fence can be held back rather than wait for it.
//   void EndSubmissionForMemexport()
//     Ends the submission being recorded, without waiting for it.

// Records the guest pages any memexport draw wrote so geometry draws consuming
// that output route to the host-imported buffer. Marking rounds outward. Fences
// account for all of it, coherency requests only for awaited output
// (IsMemexportAwaited).
void MarkMemexportPagesWritten(uint32_t base_bytes, uint32_t size_bytes,
                               bool awaited) {
  if (!size_bytes || base_bytes >= SharedMemory::kBufferSize) {
    return;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  uint32_t first_page = base_bytes >> 12;
  uint32_t last_page = (base_bytes + size_bytes - 1) >> 12;
  if (memexport_page_submissions_.empty()) {
    memexport_page_submissions_.resize(kMemexportPageCount);
  }
  uint64_t submission = GetCurrentSubmission();
  for (uint32_t page = first_page; page <= last_page; ++page) {
    uint64_t page_bit = uint64_t(1) << (page & 63);
    memexport_written_pages_[page >> 6] |= page_bit;
    // Fresh output, whatever the CPU wrote there before.
    memexport_cpu_written_pages_[page >> 6].fetch_and(
        ~page_bit, std::memory_order_relaxed);
    memexport_page_submissions_[page] = submission;
  }
  any_memexport_pages_written_ = true;
  memexport_last_submission_ = submission;
  memexport_fence_pending_ = true;
  if (awaited) {
    memexport_await_pending_ = true;
    memexport_awaited_submission_ = memexport_last_submission_;
  }
  memexport_written_page_min_ =
      std::min(memexport_written_page_min_, first_page);
  memexport_written_page_max_ =
      std::max(memexport_written_page_max_, last_page);
}

// At a fence, interrupt or wait for idle, after which the guest may read export
// output or reuse its memory. Awaited output is waited for, as the guest reads
// it right after. The rest holds the signal back until the returned submission
// completes, or is waited for where CanHoldSignalsForMemexport is false. That
// submission may still be the one being recorded, which
// EndSubmissionForHeldSignals ends once anything could be waiting for the
// signal. Ring buffer read-pointer writeback is deliberately not a signal, as
// it reports parse progress rather than execution.
uint64_t TakeMemexportSignalSubmission() override {
  if (!memexport_fence_pending_) {
    return 0;
  }
  memexport_fence_pending_ = false;
  if (memexport_await_pending_) {
    memexport_await_pending_ = false;
    AwaitMemexportSubmission(memexport_awaited_submission_);
    FlushMemexportStagingReadback();
  }
  if (memexport_last_submission_ <= GetCompletedSubmission()) {
    return 0;
  }
  if (CanHoldSignalsForMemexport()) {
    return memexport_last_submission_;
  }
  AwaitMemexportSubmission(memexport_last_submission_);
  FlushMemexportStagingReadback();
  return 0;
}

void EndSubmissionForHeldSignals(uint64_t submission) override {
  if (submission >= GetCurrentSubmission()) {
    EndSubmissionForMemexport();
  }
}

void AwaitSubmissionForHeldSignals(uint64_t submission) override {
  AwaitMemexportSubmission(submission);
}

// For the guest asking the GPU to flush a range it is about to read
// (COHER_BASE_HOST/COHER_SIZE_HOST): waits for awaited output to reach guest
// RAM. Gated on the range actually holding export output, unlike fences which
// cannot know what they refer to.
void AwaitMemexportForCoherency(uint32_t base_bytes, uint32_t size_bytes) {
  if (!memexport_await_pending_ || !IsMemexportRange(base_bytes, size_bytes)) {
    return;
  }
  memexport_await_pending_ = false;
  AwaitMemexportSubmission(memexport_awaited_submission_);
  FlushMemexportStagingReadback();
}

// Clears memexport marks for a range a resolve just wrote, so its device-buffer
// output is no longer treated as memexport (host buffer) data. Clearing rounds
// inward: a page straddling the edge holds both this range's data and
// neighbouring memexport output, so it keeps its mark. The window is left
// as-is, it only needs to bound the set bits.
void ClearMemexportPages(uint32_t base_bytes, uint32_t size_bytes) {
  if (!any_memexport_pages_written_ || !size_bytes ||
      base_bytes >= SharedMemory::kBufferSize) {
    return;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  uint32_t first_page = (base_bytes + 0xFFF) >> 12;
  uint32_t end_page = (base_bytes + size_bytes) >> 12;
  for (uint32_t page = first_page; page < end_page; ++page) {
    memexport_written_pages_[page >> 6] &= ~(uint64_t(1) << (page & 63));
  }
}

bool IsMemexportRange(uint32_t base_bytes, uint32_t size_bytes) const {
  if (!any_memexport_pages_written_ || !size_bytes ||
      base_bytes >= SharedMemory::kBufferSize) {
    return false;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  // Clamp the scan to the written memexport window so a range entirely outside
  // it (most textures and geometry) scans nothing.
  uint32_t first_page = std::max(base_bytes >> 12, memexport_written_page_min_);
  uint32_t last_page = std::min((base_bytes + size_bytes - 1) >> 12,
                                memexport_written_page_max_);
  for (uint32_t page = first_page; page <= last_page; ++page) {
    if (memexport_written_pages_[page >> 6] & (uint64_t(1) << (page & 63))) {
      return true;
    }
  }
  return false;
}

bool VertexFetchInMemexportRange(const RegisterFile& regs,
                                 const Shader& vertex_shader) const {
  const Shader::ConstantRegisterMap& constant_map =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (xe::bit_scan_forward(vfetch_bits, &j)) {
      vfetch_bits = xe::clear_lowest_bit(vfetch_bits);
      xenos::xe_gpu_vertex_fetch_t vfetch_constant =
          regs.GetVertexFetch(i * 32 + j);
      if (vfetch_constant.type != xenos::FetchConstantType::kVertex) {
        continue;
      }
      if (IsMemexportRange(vfetch_constant.address << 2,
                           vfetch_constant.size << 2)) {
        return true;
      }
    }
  }
  return false;
}

// Fills memexport_copy_runs_ with the parts of a range on pages memexport
// wrote, or all of it with whole_range, as other pages may hold GPU output only
// the device buffer has. False if memexport wrote none of it.
bool GatherMemexportCopyRuns(uint32_t base_bytes, uint32_t size_bytes,
                             bool whole_range) {
  memexport_copy_runs_.clear();
  if (!IsMemexportRange(base_bytes, size_bytes)) {
    return false;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  uint32_t end = base_bytes + size_bytes;
  if (whole_range) {
    memexport_copy_runs_.emplace_back(base_bytes, size_bytes);
    return true;
  }
  uint32_t first_page = std::max(base_bytes >> 12, memexport_written_page_min_);
  uint32_t last_page = std::min((end - 1) >> 12, memexport_written_page_max_);
  for (uint32_t page = first_page; page <= last_page; ++page) {
    if (!(memexport_written_pages_[page >> 6] & (uint64_t(1) << (page & 63)))) {
      continue;
    }
    uint32_t run_start = std::max(base_bytes, page << 12);
    uint32_t run_end = std::min(end, (page + 1) << 12);
    if (!memexport_copy_runs_.empty()) {
      std::pair<uint32_t, uint32_t>& last_run = memexport_copy_runs_.back();
      if (last_run.first + last_run.second == run_start) {
        last_run.second += run_end - run_start;
        continue;
      }
    }
    memexport_copy_runs_.emplace_back(run_start, run_end - run_start);
  }
  return true;
}

// Pages stay marked until a resolve overwrites them, so once the game reuses
// the memory for CPU-written data - its per-frame geometry, say - draws
// fetching that data still route to the host buffer and refresh copies still
// read it on the GPU timeline. The GPU runs behind the guest's fences, which
// signal when the CP passes them, so by then the guest may have written the
// next frame's data over it, and objects flash for a frame. This drops the
// marks in a range that the CPU has written since their export, once that
// export has finished: guest RAM then holds the newest data, which the usual
// CP-time upload takes. Called on the command processor thread only.
void ForgetCpuRewrittenMemexportPages(uint32_t base_bytes,
                                      uint32_t size_bytes) {
  if (!any_memexport_pages_written_ || !size_bytes ||
      base_bytes >= SharedMemory::kBufferSize) {
    return;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  uint32_t first_page = std::max(base_bytes >> 12, memexport_written_page_min_);
  uint32_t last_page = std::min((base_bytes + size_bytes - 1) >> 12,
                                memexport_written_page_max_);
  if (first_page > last_page) {
    return;
  }
  uint64_t completed_submission = GetCompletedSubmission();
  for (uint32_t block = first_page >> 6; block <= last_page >> 6; ++block) {
    uint64_t pages =
        memexport_written_pages_[block] &
        memexport_cpu_written_pages_[block].load(std::memory_order_relaxed);
    if (block == first_page >> 6) {
      pages &= UINT64_MAX << (first_page & 63);
    }
    if (block == last_page >> 6) {
      pages &= UINT64_MAX >> (63 - (last_page & 63));
    }
    uint32_t page_in_block;
    while (xe::bit_scan_forward(pages, &page_in_block)) {
      uint64_t page_bit = uint64_t(1) << page_in_block;
      pages &= ~page_bit;
      if (memexport_page_submissions_[(block << 6) | page_in_block] >
          completed_submission) {
        // The export may still land, after the CPU write.
        continue;
      }
      memexport_written_pages_[block] &= ~page_bit;
      memexport_cpu_written_pages_[block].fetch_and(~page_bit,
                                                    std::memory_order_relaxed);
      ++memexport_pages_forgotten_;
    }
  }
}

// Draws reading guest RAM through the host buffer, and the copies refreshing
// the device buffer from it, read it when the GPU executes them. The next fence
// or interrupt is held back until then, as for export output: the guest reuses
// memory once those signal, possibly before the GPU got to it.
void HoldSignalsForHostBufferRead() {
  memexport_last_submission_ = GetCurrentSubmission();
  memexport_fence_pending_ = true;
}

void ForgetCpuRewrittenMemexportPagesInVertexFetch(
    const RegisterFile& regs, const Shader& vertex_shader) {
  const Shader::ConstantRegisterMap& constant_map =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (xe::bit_scan_forward(vfetch_bits, &j)) {
      vfetch_bits = xe::clear_lowest_bit(vfetch_bits);
      xenos::xe_gpu_vertex_fetch_t vfetch_constant =
          regs.GetVertexFetch(i * 32 + j);
      if (vfetch_constant.type == xenos::FetchConstantType::kVertex) {
        ForgetCpuRewrittenMemexportPages(vfetch_constant.address << 2,
                                         vfetch_constant.size << 2);
      }
    }
  }
}

// On the guest thread that wrote, within the global critical region, so only
// the atomic bitmap is touched.
void NoteMemexportCpuWrite(uint32_t address_first, uint32_t address_last) {
  if (address_first > address_last ||
      address_first >= SharedMemory::kBufferSize) {
    return;
  }
  address_last = std::min(address_last, SharedMemory::kBufferSize - 1);
  uint32_t first_page = address_first >> 12;
  uint32_t last_page = address_last >> 12;
  for (uint32_t block = first_page >> 6; block <= last_page >> 6; ++block) {
    uint64_t pages = UINT64_MAX;
    if (block == first_page >> 6) {
      pages &= UINT64_MAX << (first_page & 63);
    }
    if (block == last_page >> 6) {
      pages &= UINT64_MAX >> (63 - (last_page & 63));
    }
    memexport_cpu_written_pages_[block].fetch_or(pages,
                                                 std::memory_order_relaxed);
  }
}

// After the shared memory is created, and before it is destroyed.
void RegisterMemexportCpuWriteWatch() {
  using Self = std::remove_pointer_t<decltype(this)>;
  memexport_cpu_write_watch_ = shared_memory_->RegisterGlobalWatch(
      [](const global_unique_lock_type& global_lock, void* context,
         uint32_t address_first, uint32_t address_last,
         bool invalidated_by_gpu) {
        if (!invalidated_by_gpu) {
          static_cast<Self*>(context)->NoteMemexportCpuWrite(address_first,
                                                             address_last);
        }
      },
      this);
}

void UnregisterMemexportCpuWriteWatch() {
  if (memexport_cpu_write_watch_) {
    shared_memory_->UnregisterGlobalWatch(memexport_cpu_write_watch_);
    memexport_cpu_write_watch_ = nullptr;
  }
}

// Resets all tracking, called from each backend's ShutdownContext.
void ResetMemexportPages() {
  memexport_written_pages_.fill(0);
  for (std::atomic<uint64_t>& pages : memexport_cpu_written_pages_) {
    pages.store(0, std::memory_order_relaxed);
  }
  memexport_page_submissions_.clear();
  any_memexport_pages_written_ = false;
  memexport_fence_pending_ = false;
  memexport_await_pending_ = false;
  memexport_last_submission_ = 0;
  memexport_awaited_submission_ = 0;
  memexport_written_page_min_ = kMemexportPageCount;
  memexport_written_page_max_ = 0;
}

// Guest pages (4 KB) any memexport draw has written, at host buffer offsets.
static constexpr uint32_t kMemexportPageCount = SharedMemory::kBufferSize >> 12;
std::array<uint64_t, kMemexportPageCount / 64> memexport_written_pages_{};
// Pages the CPU has written since an export last did, set from a shared memory
// global watch on the writing thread.
std::array<std::atomic<uint64_t>, kMemexportPageCount / 64>
    memexport_cpu_written_pages_{};
SharedMemory::GlobalWatchHandle memexport_cpu_write_watch_ = nullptr;
// Submission each page was last exported in, allocated with the first export.
std::vector<uint64_t> memexport_page_submissions_;
// Marks ForgetCpuRewrittenMemexportPages dropped, for the log_fps report.
uint64_t memexport_pages_forgotten_ = 0;
// Cleared until the first memexport draw, so consumer-side routing is skipped
// entirely for titles that never memexport.
bool any_memexport_pages_written_ = false;
// Export output has been recorded that no fence has accounted for yet.
bool memexport_fence_pending_ = false;
// Same, of awaited output, which coherency requests also wait for.
bool memexport_await_pending_ = false;
// Submission the last export draw, or host buffer read, was recorded in.
uint64_t memexport_last_submission_ = 0;
// Same, of the last export draw that is awaited.
uint64_t memexport_awaited_submission_ = 0;
// Inclusive min/max written page, so IsMemexportRange rejects a range outside
// the memexport region in O(1). Empty (min > max) until the first memexport.
uint32_t memexport_written_page_min_ = kMemexportPageCount;
uint32_t memexport_written_page_max_ = 0;
// GatherMemexportCopyRuns output, as (base, size).
std::vector<std::pair<uint32_t, uint32_t>> memexport_copy_runs_;

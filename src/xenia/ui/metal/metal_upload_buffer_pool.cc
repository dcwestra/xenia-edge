/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/metal/metal_upload_buffer_pool.h"

#include "xenia/base/logging.h"

namespace xe {
namespace ui {
namespace metal {

MetalUploadBufferPool::MetalUploadBufferPool(MTL::Device* device,
                                             const char* label,
                                             size_t page_size)
    : GraphicsUploadBufferPool(page_size), device_(device), label_(label) {}

uint8_t* MetalUploadBufferPool::Request(uint64_t submission_index, size_t size,
                                        size_t alignment,
                                        MTL::Buffer*& buffer_out,
                                        size_t& offset_out) {
  size_t offset;
  const MetalPage* page =
      static_cast<const MetalPage*>(GraphicsUploadBufferPool::Request(
          submission_index, size, alignment, offset));
  if (!page) {
    return nullptr;
  }
  buffer_out = page->buffer_;
  offset_out = offset;
  return reinterpret_cast<uint8_t*>(page->mapping_) + offset;
}

GraphicsUploadBufferPool::Page*
MetalUploadBufferPool::CreatePageImplementation() {
  MTL::Buffer* buffer =
      device_->newBuffer(page_size_, MTL::ResourceStorageModeShared);
  if (!buffer) {
    XELOGE("Failed to create a Metal upload buffer with {} bytes", page_size_);
    return nullptr;
  }
  buffer->setLabel(NS::String::string(label_, NS::UTF8StringEncoding));
  return new MetalPage(buffer);
}

MetalUploadBufferPool::MetalPage::~MetalPage() { buffer_->release(); }

}  // namespace metal
}  // namespace ui
}  // namespace xe

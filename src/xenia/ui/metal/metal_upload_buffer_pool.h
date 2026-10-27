/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_METAL_METAL_UPLOAD_BUFFER_POOL_H_
#define XENIA_UI_METAL_METAL_UPLOAD_BUFFER_POOL_H_

#include "xenia/ui/graphics_upload_buffer_pool.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace ui {
namespace metal {

class MetalUploadBufferPool : public GraphicsUploadBufferPool {
 public:
  // The label must outlive the pool.
  MetalUploadBufferPool(MTL::Device* device, const char* label,
                        size_t page_size = kDefaultPageSize);

  uint8_t* Request(uint64_t submission_index, size_t size, size_t alignment,
                   MTL::Buffer*& buffer_out, size_t& offset_out);

 protected:
  Page* CreatePageImplementation() override;

 private:
  struct MetalPage : public Page {
    // Takes ownership of the buffer.
    explicit MetalPage(MTL::Buffer* buffer)
        : buffer_(buffer), mapping_(buffer->contents()) {}
    ~MetalPage() override;
    MTL::Buffer* buffer_;
    void* mapping_;
  };

  MTL::Device* device_;
  const char* label_;
};

}  // namespace metal
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_METAL_METAL_UPLOAD_BUFFER_POOL_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/kinect_camera.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

DEFINE_bool(kinect_mirror, false,
            "Flip the Kinect's depth and color left to right. Titles' trackers "
            "take the sensor's own orientation: flipped, a left arm reads as a "
            "right one.",
            "Kernel");

DEFINE_path(kinect_playback_path, "",
            "Play a recording made with kinect_record_path to Kinect titles, "
            "over and over, instead of streaming from a Kinect.",
            "Kernel");

#if XE_PLATFORM_LINUX
#include <dlfcn.h>
#include <libusb-1.0/libusb.h>
#endif

namespace xe {
namespace kernel {
namespace nui {

// Each frame of a recording: a header, then the frame as the sensor sent it.
struct KinectRecordingFrame {
  uint32_t stream;   // 0 for depth, 1 for color.
  uint32_t time_ms;  // Since the recording started.
  uint32_t size;
};

KinectRecorder::KinectRecorder(const std::filesystem::path& path) {
  file_ = xe::filesystem::OpenFile(path, "wb");
  if (file_) {
    XELOGI("Kinect: recording to {}", xe::strip_pii(xe::path_to_utf8(path)));
  } else {
    XELOGW("Kinect: can't record to {}", xe::strip_pii(xe::path_to_utf8(path)));
  }
  start_ = std::chrono::steady_clock::now();
}

KinectRecorder::~KinectRecorder() {
  if (file_) {
    fclose(file_);
  }
}

void KinectRecorder::AddFrame(bool color, const uint8_t* data, size_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_) {
    return;
  }
  KinectRecordingFrame header;
  header.stream = color ? 1 : 0;
  header.time_ms =
      uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start_)
                   .count());
  header.size = uint32_t(size);
  fwrite(&header, sizeof(header), 1, file_);
  fwrite(data, 1, size, file_);
}

namespace {

// Plays a recording back, at the pace it was made, looping at its end.
class KinectCameraPlayback : public KinectCamera {
 public:
  ~KinectCameraPlayback() override {
    Stop();
    if (file_) {
      fclose(file_);
    }
  }

  bool Initialize(const std::filesystem::path& path) {
    file_ = xe::filesystem::OpenFile(path, "rb");
    if (!file_) {
      XELOGW("Kinect: can't open the recording {}",
             xe::strip_pii(xe::path_to_utf8(path)));
      return false;
    }
    XELOGI("Kinect: playing the recording {}",
           xe::strip_pii(xe::path_to_utf8(path)));
    return true;
  }

  bool Start(FrameCallback depth_callback,
             FrameCallback color_callback) override {
    if (running_) {
      return true;
    }
    depth_callback_ = std::move(depth_callback);
    color_callback_ = std::move(color_callback);
    running_ = true;
    thread_ = std::thread([this]() { PlayerMain(); });
    return true;
  }

  bool IsColorStreaming() const override { return bool(color_callback_); }

  void Stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void PlayerMain() {
    using clock = std::chrono::steady_clock;
    std::vector<uint8_t> frame;
    auto loop_start = clock::now();
    while (running_) {
      KinectRecordingFrame header;
      if (fread(&header, sizeof(header), 1, file_) != 1 ||
          header.size > 4 * 1024 * 1024) {
        if (std::ftell(file_) <= long(sizeof(header))) {
          XELOGW("Kinect: the recording has no frames");
          return;
        }
        std::fseek(file_, 0, SEEK_SET);
        loop_start = clock::now();
        continue;
      }
      frame.resize(header.size);
      if (fread(frame.data(), 1, frame.size(), file_) != frame.size()) {
        std::fseek(file_, 0, SEEK_SET);
        loop_start = clock::now();
        continue;
      }
      const auto due = loop_start + std::chrono::milliseconds(header.time_ms);
      while (running_ && clock::now() < due) {
        std::this_thread::sleep_for(std::min<clock::duration>(
            due - clock::now(), std::chrono::milliseconds(50)));
      }
      const FrameCallback& callback =
          header.stream ? color_callback_ : depth_callback_;
      if (running_ && callback) {
        callback(frame.data(), frame.size(), header.time_ms);
      }
    }
  }

  FILE* file_ = nullptr;
  FrameCallback depth_callback_;
  FrameCallback color_callback_;
  std::atomic<bool> running_ = false;
  std::thread thread_;
};

// With a recording playing, a motor that reaches any tilt it's given about
// as fast as the real one, so titles' tilting runs as it would.
class KinectMotorSimulated : public KinectMotor {
 public:
  bool SetLed(Led led) override { return true; }

  bool SetTilt(int degrees) override {
    std::lock_guard<std::mutex> lock(mutex_);
    from_degrees_ = CurrentDegrees();
    to_degrees_ = float(std::clamp(degrees, -kMaxTiltDegrees, kMaxTiltDegrees));
    move_start_ = std::chrono::steady_clock::now();
    return true;
  }

  bool GetState(State& state) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const float degrees = CurrentDegrees();
    const float radians = degrees * 3.14159265f / 180.0f;
    state.accel[0] = 0;
    state.accel[1] = int16_t(std::lround(kCountsPerG * std::cos(radians)));
    state.accel[2] = int16_t(std::lround(kCountsPerG * std::sin(radians)));
    state.tilt_half_degrees = int8_t(std::lround(degrees * 2.0f));
    state.status =
        degrees == to_degrees_ ? TiltStatus::kStopped : TiltStatus::kMoving;
    return true;
  }

 private:
  static constexpr float kDegreesPerSecond = 15.0f;

  float CurrentDegrees() const {
    const float elapsed = std::chrono::duration<float>(
                              std::chrono::steady_clock::now() - move_start_)
                              .count();
    const float distance = to_degrees_ - from_degrees_;
    const float moved = elapsed * kDegreesPerSecond;
    if (moved >= std::abs(distance)) {
      return to_degrees_;
    }
    return from_degrees_ + std::copysign(moved, distance);
  }

  std::mutex mutex_;
  float from_degrees_ = 0.0f;
  float to_degrees_ = 0.0f;
  std::chrono::steady_clock::time_point move_start_;
};

}  // namespace

uint16_t KinectRawDepthToMillimeters(uint32_t raw) {
  // 2047 is no reading. The tangent passes 90 degrees just past 1084.
  if (raw >= 1084) {
    return 0;
  }
  const double meters = 0.1236 * std::tan(double(raw) / 2842.5 + 1.1863);
  if (meters <= 0.0 || meters * 1000.0 > 8191.0) {
    return 0;
  }
  return uint16_t(meters * 1000.0 + 0.5);
}

#if XE_PLATFORM_LINUX
namespace {

#define XE_LIBUSB_FUNCTIONS(F)              \
  F(libusb_init)                            \
  F(libusb_exit)                            \
  F(libusb_open_device_with_vid_pid)        \
  F(libusb_close)                           \
  F(libusb_kernel_driver_active)            \
  F(libusb_detach_kernel_driver)            \
  F(libusb_attach_kernel_driver)            \
  F(libusb_claim_interface)                 \
  F(libusb_release_interface)               \
  F(libusb_control_transfer)                \
  F(libusb_alloc_transfer)                  \
  F(libusb_free_transfer)                   \
  F(libusb_submit_transfer)                 \
  F(libusb_cancel_transfer)                 \
  F(libusb_handle_events_timeout_completed) \
  F(libusb_error_name)

struct Libusb {
#define XE_LIBUSB_DECLARE(name) decltype(&::name) name = nullptr;
  XE_LIBUSB_FUNCTIONS(XE_LIBUSB_DECLARE)
#undef XE_LIBUSB_DECLARE

  bool Load() {
    void* library = dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      return false;
    }
#define XE_LIBUSB_RESOLVE(fn)                               \
  fn = reinterpret_cast<decltype(fn)>(dlsym(library, #fn)); \
  if (!fn) {                                                \
    XELOGW("Kinect: libusb-1.0 has no {}", #fn);            \
    return false;                                           \
  }
    XE_LIBUSB_FUNCTIONS(XE_LIBUSB_RESOLVE)
#undef XE_LIBUSB_RESOLVE
    return true;
  }
};

constexpr uint16_t kVendorMicrosoft = 0x045E;
constexpr uint16_t kProductKinectCamera = 0x02AE;
constexpr uint16_t kProductKinectMotor = 0x02B0;
// Isochronous packets: a 12-byte "RB" header, then frame data. The header's
// flag is the stream's base ORed with 1 for the start of a frame, 2 for the
// middle and 5 for the end.
constexpr unsigned char kDepthEndpoint = 0x82;
constexpr int kDepthPacketSize = 1760;
constexpr uint8_t kDepthFlagBase = 0x70;
constexpr unsigned char kColorEndpoint = 0x81;
constexpr int kColorPacketSize = 1920;
constexpr uint8_t kColorFlagBase = 0x80;
constexpr int kPacketsPerTransfer = 16;
constexpr int kTransferCount = 16;
constexpr unsigned int kControlTimeoutMs = 1600;

uint16_t LoadLe16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t LoadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
void StoreLe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}

class KinectCameraLinux : public KinectCamera {
 public:
  ~KinectCameraLinux() override {
    Stop();
    if (handle_) {
      usb_.libusb_release_interface(handle_, 0);
      if (kernel_driver_detached_) {
        usb_.libusb_attach_kernel_driver(handle_, 0);
      }
      usb_.libusb_close(handle_);
    }
    if (context_) {
      usb_.libusb_exit(context_);
    }
  }

  bool Initialize() {
    if (!usb_.Load()) {
      XELOGW("Kinect: libusb-1.0 isn't available, so the Kinect can't be used");
      return false;
    }
    if (usb_.libusb_init(&context_) < 0) {
      context_ = nullptr;
      return false;
    }
    handle_ = usb_.libusb_open_device_with_vid_pid(context_, kVendorMicrosoft,
                                                   kProductKinectCamera);
    if (!handle_) {
      XELOGW(
          "Kinect: no Kinect for Xbox 360 camera found, or no permission to "
          "open it (USB {:04x}:{:04x} needs a udev rule granting access)",
          kVendorMicrosoft, kProductKinectCamera);
      return false;
    }
    // The kernel's gspca_kinect webcam driver claims the camera.
    if (usb_.libusb_kernel_driver_active(handle_, 0) == 1) {
      if (usb_.libusb_detach_kernel_driver(handle_, 0) == 0) {
        kernel_driver_detached_ = true;
      }
    }
    int result = usb_.libusb_claim_interface(handle_, 0);
    if (result < 0) {
      XELOGW("Kinect: can't claim the camera: {}",
             usb_.libusb_error_name(result));
      return false;
    }
    // Replies a session that ended mid-command left unread.
    uint8_t stale[0x200];
    for (int i = 0; i < 16 && usb_.libusb_control_transfer(
                                  handle_, 0xC0, 0, 0, 0, stale, sizeof(stale),
                                  kControlTimeoutMs) > 0;
         ++i) {
    }
    XELOGI("Kinect: camera opened");
    return true;
  }

  bool Start(FrameCallback depth_callback,
             FrameCallback color_callback) override {
    if (running_) {
      return true;
    }
    // Every control transfer comes before the streams start: with transfers
    // in flight, libusb may run their callbacks inside a synchronous one, on
    // the caller's thread, which may hold the lock the callbacks take.
    const uint16_t mirror = cvars::kinect_mirror ? 0x01 : 0x00;
    // 11-bit packed depth, 640x480, 30 frames a second, as the sensor sees it,
    // which is what titles' trackers take.
    const bool depth_configured =
        WriteRegister(0x105, 0x00) &&  // Projector auto-cycle off.
        WriteRegister(0x06, 0x00) &&   // Depth stream off.
        WriteRegister(0x12, 0x03) &&   // 11-bit packed.
        WriteRegister(0x13, 0x01) &&   // 640x480.
        WriteRegister(0x14, 0x1E) &&   // 30 fps.
        WriteRegister(0x06, 0x02) &&   // Depth stream on.
        WriteRegister(0x17, mirror);
    if (!depth_configured) {
      XELOGW("Kinect: the camera didn't accept the depth stream settings");
      return false;
    }
    // 8-bit Bayer, 640x480, 30 frames a second.
    const bool color_configured =
        color_callback && WriteRegister(0x05, 0x00) &&  // Color stream off.
        WriteRegister(0x0C, 0x00) &&                    // Bayer.
        WriteRegister(0x0D, 0x01) &&                    // 640x480.
        WriteRegister(0x0E, 0x1E) &&                    // 30 fps.
        WriteRegister(0x05, 0x01) &&                    // Color stream on.
        WriteRegister(0x47, mirror);
    running_ = true;
    if (!StartStream(depth_, kDepthEndpoint, kDepthPacketSize, kDepthFlagBase,
                     kDepthFrameBytes, std::move(depth_callback))) {
      XELOGW("Kinect: couldn't start reading the depth stream");
      running_ = false;
      return false;
    }
    if (color_configured &&
        StartStream(color_, kColorEndpoint, kColorPacketSize, kColorFlagBase,
                    kColorFrameBytes, std::move(color_callback))) {
      color_streaming_ = true;
    } else if (color_callback) {
      XELOGW("Kinect: couldn't start the color stream");
    }
    event_thread_ = std::thread([this]() {
      while (running_ || active_transfers_ > 0) {
        timeval timeout = {0, 100000};
        usb_.libusb_handle_events_timeout_completed(context_, &timeout,
                                                    nullptr);
      }
    });
    XELOGI("Kinect: depth{} stream started",
           color_streaming_ ? " and color" : "");
    return true;
  }

  bool IsColorStreaming() const override { return color_streaming_; }

  void Stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    for (Stream* stream : {&depth_, &color_}) {
      for (libusb_transfer* transfer : stream->transfers) {
        usb_.libusb_cancel_transfer(transfer);
      }
    }
    if (event_thread_.joinable()) {
      event_thread_.join();
    }
    for (Stream* stream : {&depth_, &color_}) {
      for (libusb_transfer* transfer : stream->transfers) {
        usb_.libusb_free_transfer(transfer);
      }
      stream->transfers.clear();
      stream->buffers.clear();
    }
    WriteRegister(0x06, 0x00);
    if (color_streaming_) {
      WriteRegister(0x05, 0x00);
      color_streaming_ = false;
    }
  }

 private:
  // A camera command: a "GM" header, then the payload, answered by an "RB"
  // header and the reply.
  int SendCommand(uint16_t command, const uint8_t* payload,
                  uint16_t payload_size, uint8_t* reply, int reply_capacity) {
    uint8_t out[8 + 0x200];
    if (payload_size > sizeof(out) - 8) {
      return LIBUSB_ERROR_INVALID_PARAM;
    }
    out[0] = 'G';
    out[1] = 'M';
    StoreLe16(out + 2, payload_size / 2);
    StoreLe16(out + 4, command);
    StoreLe16(out + 6, tag_);
    std::memcpy(out + 8, payload, payload_size);
    int result = usb_.libusb_control_transfer(handle_, 0x40, 0, 0, 0, out,
                                              uint16_t(8 + payload_size),
                                              kControlTimeoutMs);
    if (result < 0) {
      return result;
    }
    const uint16_t tag = tag_++;
    uint8_t in[0x200];
    int length = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
      length = usb_.libusb_control_transfer(handle_, 0xC0, 0, 0, 0, in,
                                            sizeof(in), kControlTimeoutMs);
      if (length < 0) {
        return length;
      }
      if (length >= 8 && in[0] == 'R' && in[1] == 'B' &&
          LoadLe16(in + 4) == command && LoadLe16(in + 6) == tag) {
        break;
      }
      // Nothing yet, or a reply left unread by an earlier session.
      length = 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (length < 8) {
      return LIBUSB_ERROR_TIMEOUT;
    }
    const int reply_size = length - 8;
    std::memcpy(reply, in + 8, std::min(reply_size, reply_capacity));
    return reply_size;
  }

  // Now and then the camera doesn't answer a command (at the first depth or
  // color setting, which left that stream off for the whole session), so a
  // write is tried again. Writing a register twice is harmless, and a late
  // reply to the first try has an older tag, which SendCommand skips.
  bool WriteRegister(uint16_t reg, uint16_t value) {
    uint8_t payload[4];
    StoreLe16(payload, reg);
    StoreLe16(payload + 2, value);
    uint8_t reply[4];
    int result = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      result =
          SendCommand(0x03, payload, sizeof(payload), reply, sizeof(reply));
      if (result >= 0) {
        if (attempt) {
          XELOGI("Kinect: writing camera register {:04X} took {} tries", reg,
                 attempt + 1);
        }
        return true;
      }
      if (result == LIBUSB_ERROR_NO_DEVICE) {
        break;
      }
    }
    XELOGW("Kinect: writing camera register {:04X} failed: {}", reg,
           usb_.libusb_error_name(result));
    return false;
  }

  struct Stream {
    KinectCameraLinux* camera = nullptr;
    uint8_t flag_base = 0;
    FrameCallback callback;
    std::vector<libusb_transfer*> transfers;
    std::vector<std::vector<uint8_t>> buffers;
    // Touched only on the event thread.
    std::vector<uint8_t> frame;
    size_t frame_size = 0;
    bool frame_damaged = false;
    bool synced = false;
    uint8_t expected_sequence = 0;
  };

  bool StartStream(Stream& stream, unsigned char endpoint, int packet_size,
                   uint8_t flag_base, size_t frame_bytes,
                   FrameCallback callback) {
    stream.camera = this;
    stream.flag_base = flag_base;
    stream.callback = std::move(callback);
    stream.frame.resize(frame_bytes);
    int submitted = 0;
    for (int i = 0; i < kTransferCount; ++i) {
      libusb_transfer* transfer =
          usb_.libusb_alloc_transfer(kPacketsPerTransfer);
      if (!transfer) {
        break;
      }
      stream.buffers.emplace_back(packet_size * kPacketsPerTransfer);
      libusb_fill_iso_transfer(transfer, handle_, endpoint,
                               stream.buffers.back().data(),
                               int(stream.buffers.back().size()),
                               kPacketsPerTransfer, OnTransfer, &stream, 0);
      libusb_set_iso_packet_lengths(transfer, packet_size);
      stream.transfers.push_back(transfer);
      if (usb_.libusb_submit_transfer(transfer) == 0) {
        ++active_transfers_;
        ++submitted;
      }
    }
    return submitted != 0;
  }

  static void LIBUSB_CALL OnTransfer(libusb_transfer* transfer) {
    auto* stream = static_cast<Stream*>(transfer->user_data);
    KinectCameraLinux* self = stream->camera;
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
      for (int i = 0; i < transfer->num_iso_packets; ++i) {
        const libusb_iso_packet_descriptor& packet =
            transfer->iso_packet_desc[i];
        if (packet.status == LIBUSB_TRANSFER_COMPLETED &&
            packet.actual_length) {
          ProcessPacket(*stream,
                        libusb_get_iso_packet_buffer_simple(transfer, i),
                        int(packet.actual_length));
        }
      }
    }
    if (self->running_ && transfer->status != LIBUSB_TRANSFER_CANCELLED &&
        transfer->status != LIBUSB_TRANSFER_NO_DEVICE &&
        self->usb_.libusb_submit_transfer(transfer) == 0) {
      return;
    }
    --self->active_transfers_;
  }

  // Reassembles frames, dropping any that lost a packet.
  static void ProcessPacket(Stream& stream, const uint8_t* packet, int length) {
    if (length < 12 || packet[0] != 'R' || packet[1] != 'B') {
      return;
    }
    const uint8_t flag = packet[3];
    const uint8_t sequence = packet[5];
    if (flag == (stream.flag_base | 0x1)) {
      stream.frame_size = 0;
      stream.frame_damaged = false;
      stream.synced = true;
    } else if (!stream.synced || (flag != (stream.flag_base | 0x2) &&
                                  flag != (stream.flag_base | 0x5))) {
      return;
    } else if (sequence != stream.expected_sequence) {
      stream.frame_damaged = true;
    }
    stream.expected_sequence = uint8_t(sequence + 1);
    size_t data_size = size_t(length - 12);
    if (stream.frame_size + data_size > stream.frame.size()) {
      stream.frame_damaged = true;
      data_size = stream.frame.size() - stream.frame_size;
    }
    std::memcpy(stream.frame.data() + stream.frame_size, packet + 12,
                data_size);
    stream.frame_size += data_size;
    if (flag == (stream.flag_base | 0x5)) {
      if (!stream.frame_damaged && stream.frame_size == stream.frame.size()) {
        stream.callback(stream.frame.data(), stream.frame_size,
                        LoadLe32(packet + 8));
      }
      stream.synced = false;
    }
  }

  Libusb usb_;
  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  bool kernel_driver_detached_ = false;
  uint16_t tag_ = 0;
  std::atomic<bool> running_ = false;
  std::atomic<int> active_transfers_ = 0;
  bool color_streaming_ = false;
  Stream depth_;
  Stream color_;
  std::thread event_thread_;
};

// The motor device: the tilt motor, its accelerometer and the LED.
class KinectMotorLinux : public KinectMotor {
 public:
  ~KinectMotorLinux() override {
    if (handle_) {
      usb_.libusb_close(handle_);
    }
    if (context_) {
      usb_.libusb_exit(context_);
    }
  }

  bool Initialize() {
    if (!usb_.Load()) {
      return false;
    }
    if (usb_.libusb_init(&context_) < 0) {
      context_ = nullptr;
      return false;
    }
    handle_ = usb_.libusb_open_device_with_vid_pid(context_, kVendorMicrosoft,
                                                   kProductKinectMotor);
    if (!handle_) {
      XELOGW(
          "Kinect: no Kinect for Xbox 360 motor found, or no permission to "
          "open it (USB {:04x}:{:04x}), so it won't tilt",
          kVendorMicrosoft, kProductKinectMotor);
      return false;
    }
    return true;
  }

  bool SetLed(Led led) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return usb_.libusb_control_transfer(handle_, 0x40, 0x06, uint16_t(led), 0,
                                        nullptr, 0, kControlTimeoutMs) >= 0;
  }

  bool SetTilt(int degrees) override {
    degrees = std::clamp(degrees, -kMaxTiltDegrees, kMaxTiltDegrees);
    std::lock_guard<std::mutex> lock(mutex_);
    // In half degrees, relative to gravity.
    return usb_.libusb_control_transfer(handle_, 0x40, 0x31,
                                        uint16_t(int16_t(degrees * 2)), 0,
                                        nullptr, 0, kControlTimeoutMs) >= 0;
  }

  bool GetState(State& state) override {
    uint8_t reply[10];
    std::lock_guard<std::mutex> lock(mutex_);
    if (usb_.libusb_control_transfer(handle_, 0xC0, 0x32, 0, 0, reply,
                                     sizeof(reply),
                                     kControlTimeoutMs) != sizeof(reply)) {
      return false;
    }
    for (int i = 0; i < 3; ++i) {
      state.accel[i] = int16_t(reply[2 + i * 2] << 8 | reply[3 + i * 2]);
    }
    state.tilt_half_degrees = int8_t(reply[8]);
    state.status = TiltStatus(reply[9]);
    return true;
  }

 private:
  Libusb usb_;
  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  std::mutex mutex_;
};

}  // namespace

std::unique_ptr<KinectMotor> KinectMotor::Open() {
  if (!cvars::kinect_playback_path.empty()) {
    return std::make_unique<KinectMotorSimulated>();
  }
  auto motor = std::make_unique<KinectMotorLinux>();
  if (!motor->Initialize()) {
    return nullptr;
  }
  return motor;
}

std::unique_ptr<KinectCamera> KinectCamera::Open() {
  if (!cvars::kinect_playback_path.empty()) {
    auto playback = std::make_unique<KinectCameraPlayback>();
    if (!playback->Initialize(cvars::kinect_playback_path)) {
      return nullptr;
    }
    return playback;
  }
  auto camera = std::make_unique<KinectCameraLinux>();
  if (!camera->Initialize()) {
    return nullptr;
  }
  return camera;
}
#else
std::unique_ptr<KinectMotor> KinectMotor::Open() {
  if (!cvars::kinect_playback_path.empty()) {
    return std::make_unique<KinectMotorSimulated>();
  }
  return nullptr;
}

std::unique_ptr<KinectCamera> KinectCamera::Open() {
  if (!cvars::kinect_playback_path.empty()) {
    auto playback = std::make_unique<KinectCameraPlayback>();
    if (!playback->Initialize(cvars::kinect_playback_path)) {
      return nullptr;
    }
    return playback;
  }
  return nullptr;
}
#endif

}  // namespace nui
}  // namespace kernel
}  // namespace xe

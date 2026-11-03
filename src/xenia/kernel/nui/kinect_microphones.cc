/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/kinect_microphones.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <thread>
#include <vector>

#include "xenia/apu/audio_system.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/kernel/nui/kinect_libusb.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/xbox.h"

DEFINE_string(kinect_microphone, "auto",
              "What Kinect titles hear through the Kinect's microphone array: "
              "\"auto\" for the Kinect's own microphones, or the host's "
              "recording device if they can't be used; \"kinect\" or \"host\" "
              "for only one of them; \"off\" for none.",
              "Kernel");
DEFINE_string(kinect_host_microphone, "",
              "The host recording device that stands in for the Kinect's "
              "microphones, by part of its name. Empty for the default one.",
              "Kernel");
DEFINE_path(kinect_audio_firmware_path, "",
            "The Kinect's audio firmware, which its microphones need: "
            "audios.bin, or the system package FFFE07DF00000001 it's in, from "
            "the $SystemUpdate folder of an Xbox 360 system update. Without "
            "it, that package installed with File > Install Content is used.",
            "Kernel");
DEFINE_int32(kinect_microphone_gain, 8,
             "Multiply the Kinect's microphone samples by this: they come off "
             "USB quieter than titles expect (kixenia found Nike+ Kinect "
             "Training hears \"Xbox\" at 1 but the command after it only at "
             "8). A host recording device is used as it is.",
             "Kernel");

namespace xe {
namespace kernel {
namespace nui {

void MicrophoneWindowQueue::Push(const MicrophoneWindow& window) {
  std::lock_guard<std::mutex> lock(mutex_);
  windows_[written_ % kCapacity] = window;
  ++written_;
  // Behind by more than a burst of arrivals would put it, as a recording
  // clock a little faster than the title's would leave it: catch up.
  if (written_ - read_ > kMaxBacklog) {
    read_ = written_ - kPrime;
  }
}

bool MicrophoneWindowQueue::Pop(MicrophoneWindow& window) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!primed_) {
    if (written_ - read_ < kPrime) {
      return false;
    }
    primed_ = true;
  }
  if (read_ == written_) {
    primed_ = false;
    return false;
  }
  window = windows_[read_ % kCapacity];
  ++read_;
  return true;
}

namespace {

constexpr uint32_t kFirmwareMagic = 0xCA77F00D;

uint32_t LoadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}

bool IsAudioFirmware(const std::vector<uint8_t>& data) {
  return data.size() >= 28 && LoadLe32(data.data()) == kFirmwareMagic;
}

bool ReadHostFile(const std::filesystem::path& path,
                  std::vector<uint8_t>& data) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  data.clear();
  uint8_t buffer[0x10000];
  size_t read;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.insert(data.end(), buffer, buffer + read);
  }
  fclose(file);
  return true;
}

// audios.bin from the system package FFFE07DF00000001.
bool ReadFirmwareFromPackage(const std::filesystem::path& path,
                             std::vector<uint8_t>& data) {
  auto device = vfs::XContentContainerDevice::CreateContentDevice("", path);
  if (!device || !device->Initialize()) {
    return false;
  }
  // Public on the base class.
  vfs::Entry* entry =
      static_cast<vfs::Device*>(device.get())->ResolvePath("audios.bin");
  vfs::File* file = nullptr;
  if (!entry || XFAILED(entry->Open(vfs::FileAccess::kFileReadData, &file))) {
    return false;
  }
  data.resize(entry->size());
  size_t bytes_read = 0;
  const X_STATUS status = file->ReadSync(data, 0, &bytes_read);
  file->Destroy();
  return XSUCCEEDED(status) && bytes_read == data.size();
}

std::vector<uint8_t> FindAudioFirmware(
    const std::filesystem::path& content_root) {
  std::vector<uint8_t> data;
  if (!cvars::kinect_audio_firmware_path.empty()) {
    const std::filesystem::path& path = cvars::kinect_audio_firmware_path;
    if ((ReadHostFile(path, data) && IsAudioFirmware(data)) ||
        (ReadFirmwareFromPackage(path, data) && IsAudioFirmware(data))) {
      return data;
    }
    XELOGW(
        "Kinect: {} is neither the Kinect's audio firmware nor a package "
        "with it",
        xe::strip_pii(xe::path_to_utf8(path)));
    return {};
  }
  // Where File > Install Content puts the system package's files.
  const std::filesystem::path installed =
      content_root / "0000000000000000" / "FFFE07DF" / "00008000";
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(installed, ec)) {
    if (entry.is_directory(ec) &&
        ReadHostFile(entry.path() / "audios.bin", data) &&
        IsAudioFirmware(data)) {
      return data;
    }
  }
  return {};
}

// A host recording device standing in for the array: its one channel is the
// processed mix and every microphone.
class HostMicrophones : public KinectMicrophones {
 public:
  explicit HostMicrophones(std::unique_ptr<apu::AudioCapture> capture)
      : capture_(std::move(capture)) {}

  std::string name() const override {
    return "host recording device \"" + capture_->device_name() + "\"";
  }

  bool ReadWindow(MicrophoneWindow& window) override {
    // Whatever has arrived goes into windows first, so the queue's priming
    // and its limit on the backlog apply.
    int16_t samples[MicrophoneWindow::kSamples * 4];
    size_t count;
    while ((count = capture_->Read(samples, xe::countof(samples))) > 0) {
      pending_.insert(pending_.end(), samples, samples + count);
    }
    size_t offset = 0;
    while (pending_.size() - offset >= MicrophoneWindow::kSamples) {
      MicrophoneWindow filled;
      for (uint32_t i = 0; i < MicrophoneWindow::kSamples; ++i) {
        const int16_t sample = pending_[offset + i];
        filled.processed[i] = sample;
        for (uint32_t m = 0; m < MicrophoneWindow::kMicrophones; ++m) {
          filled.microphones[m][i] = int32_t(sample) * 65536;
        }
      }
      queue_.Push(filled);
      offset += MicrophoneWindow::kSamples;
    }
    pending_.erase(pending_.begin(), pending_.begin() + offset);
    return queue_.Pop(window);
  }

 private:
  std::unique_ptr<apu::AudioCapture> capture_;
  std::vector<int16_t> pending_;
  MicrophoneWindowQueue queue_;
};

std::unique_ptr<KinectMicrophones> OpenHostMicrophones(
    apu::AudioSystem* audio_system) {
  if (!audio_system) {
    return nullptr;
  }
  auto capture = audio_system->OpenCapture(cvars::kinect_host_microphone,
                                           KinectMicrophones::kSampleRate);
  if (!capture) {
    return nullptr;
  }
  return std::make_unique<HostMicrophones>(std::move(capture));
}

#if XE_PLATFORM_LINUX

constexpr uint16_t kVendorMicrosoft = 0x045E;
constexpr uint16_t kProductKinectAudio = 0x02AD;
// The audio device's boot loader, which is all it runs after power-up, takes
// commands on 0x01 (a header, then the data in 512-byte packets) and answers
// each on 0x81. The firmware written in pages and started, the device leaves
// the bus and comes back with its audio interfaces. From libfreenect and
// kixenia's notes.
constexpr unsigned char kCommandEndpoint = 0x01;
constexpr unsigned char kReplyEndpoint = 0x81;
constexpr uint32_t kCommandMagic = 0x06022009;
constexpr uint32_t kReplyMagic = 0x0A6FE000;
constexpr uint32_t kCommandWrite = 0x03;
constexpr uint32_t kCommandExecute = 0x04;
constexpr uint32_t kFirmwarePageBytes = 0x4000;
constexpr uint32_t kBulkPacketBytes = 512;
constexpr unsigned int kBulkTimeoutMs = 1000;
constexpr auto kReenumerateTimeout = std::chrono::seconds(10);
constexpr auto kReenumeratePollInterval = std::chrono::milliseconds(250);
// The firmware streams each 16 ms window as 524-byte isochronous packets on
// 0x82: a 12-byte header (magic, channel, window number), then 512 bytes.
// Channel 1 is the processed mix (256 16-bit samples), and channels 2 to 9
// the halves of the four microphones (128 32-bit samples each), all
// little-endian. It expects a stream to its speaker endpoint 0x02 alongside,
// the reference for its echo cancellation.
constexpr unsigned char kMicrophoneEndpoint = 0x82;
constexpr unsigned char kSpeakerEndpoint = 0x02;
constexpr int kMicrophonePacketBytes = 524;
constexpr int kMicrophoneHeaderBytes = 12;
constexpr uint32_t kMicrophonePacketMagic = 0x80000080;
constexpr int kSpeakerPacketBytes = 76;
constexpr int kTransferCount = 8;
constexpr int kPacketsPerTransfer = 16;
// Channels 2 to 9 as bits 1 to 8.
constexpr uint32_t kRawChannels = 0x1FE;

class KinectMicrophonesLinux : public KinectMicrophones {
 public:
  ~KinectMicrophonesLinux() override {
    Stop();
    if (handle_) {
      for (int interface_number : claimed_) {
        usb_.libusb_release_interface(handle_, interface_number);
      }
      usb_.libusb_close(handle_);
    }
    if (context_) {
      usb_.libusb_exit(context_);
    }
  }

  bool Initialize(const std::filesystem::path& content_root) {
    if (!usb_.Load() || usb_.libusb_init(&context_) < 0) {
      context_ = nullptr;
      return false;
    }
    handle_ = usb_.libusb_open_device_with_vid_pid(context_, kVendorMicrosoft,
                                                   kProductKinectAudio);
    if (!handle_) {
      XELOGW(
          "Kinect: no Kinect for Xbox 360 audio device found, or no "
          "permission to open it (USB {:04x}:{:04x})",
          kVendorMicrosoft, kProductKinectAudio);
      return false;
    }
    if (InterfaceCount() < 2) {
      // Still the boot loader: the firmware is lost whenever the sensor loses
      // power.
      std::vector<uint8_t> firmware = FindAudioFirmware(content_root);
      if (firmware.empty()) {
        XELOGW(
            "Kinect: the microphones need the Kinect's audio firmware; set "
            "kinect_audio_firmware_path to audios.bin, or to the package "
            "FFFE07DF00000001 from an Xbox 360 system update");
        return false;
      }
      if (!UploadFirmware(firmware) || !Reopen()) {
        return false;
      }
    }
    for (int interface_number = 0; interface_number < 2; ++interface_number) {
      if (usb_.libusb_kernel_driver_active(handle_, interface_number) == 1) {
        usb_.libusb_detach_kernel_driver(handle_, interface_number);
      }
      const int result = usb_.libusb_claim_interface(handle_, interface_number);
      if (result < 0) {
        XELOGW("Kinect: can't claim audio interface {} ({})", interface_number,
               usb_.libusb_error_name(result));
        return false;
      }
      claimed_.push_back(interface_number);
    }
    return Start();
  }

  std::string name() const override { return "Kinect microphone array"; }

  bool ReadWindow(MicrophoneWindow& window) override {
    return queue_.Pop(window);
  }

 private:
  int InterfaceCount() {
    libusb_config_descriptor* config = nullptr;
    if (usb_.libusb_get_active_config_descriptor(
            usb_.libusb_get_device(handle_), &config) < 0) {
      return 0;
    }
    const int count = config->bNumInterfaces;
    usb_.libusb_free_config_descriptor(config);
    return count;
  }

  bool Bulk(unsigned char endpoint, uint8_t* data, int bytes,
            int* transferred_out = nullptr) {
    int transferred = 0;
    const int result = usb_.libusb_bulk_transfer(handle_, endpoint, data, bytes,
                                                 &transferred, kBulkTimeoutMs);
    if (transferred_out) {
      *transferred_out = transferred;
      return result >= 0;
    }
    if (result < 0 || transferred != bytes) {
      XELOGW("Kinect: audio boot loader transfer failed ({}, {} of {} bytes)",
             usb_.libusb_error_name(result), transferred, bytes);
      return false;
    }
    return true;
  }

  // A boot loader command and its reply, all little-endian.
  bool BootCommand(uint32_t command, uint32_t address, const uint8_t* data,
                   uint32_t bytes) {
    uint8_t header[24] = {};
    const uint32_t words[] = {kCommandMagic, tag_, bytes, command, address, 0};
    for (size_t i = 0; i < xe::countof(words); ++i) {
      for (int b = 0; b < 4; ++b) {
        header[i * 4 + b] = uint8_t(words[i] >> (b * 8));
      }
    }
    if (!Bulk(kCommandEndpoint, header, sizeof(header))) {
      return false;
    }
    for (uint32_t sent = 0; sent < bytes; sent += kBulkPacketBytes) {
      if (!Bulk(kCommandEndpoint, const_cast<uint8_t*>(data + sent),
                int(std::min(kBulkPacketBytes, bytes - sent)))) {
        return false;
      }
    }
    uint8_t reply[kBulkPacketBytes];
    int transferred = 0;
    if (!Bulk(kReplyEndpoint, reply, sizeof(reply), &transferred) ||
        transferred != 12 || LoadLe32(reply) != kReplyMagic ||
        LoadLe32(reply + 4) != tag_) {
      XELOGW("Kinect: no reply from the audio boot loader to command {}",
             command);
      return false;
    }
    ++tag_;
    return true;
  }

  // The firmware's header: magic, version, the address it's loaded at, its
  // size and its entry point. It's written from its start, header included.
  bool UploadFirmware(const std::vector<uint8_t>& firmware) {
    const uint32_t base = LoadLe32(firmware.data() + 12);
    const uint32_t size = LoadLe32(firmware.data() + 16);
    const uint32_t entry = LoadLe32(firmware.data() + 20);
    if (size > firmware.size()) {
      XELOGW("Kinect: the audio firmware is cut short");
      return false;
    }
    XELOGI("Kinect: uploading the audio firmware ({} bytes)", size);
    if (usb_.libusb_claim_interface(handle_, 0) < 0) {
      XELOGW("Kinect: can't claim the audio boot loader");
      return false;
    }
    bool ok = true;
    for (uint32_t offset = 0; ok && offset < size;
         offset += kFirmwarePageBytes) {
      ok = BootCommand(kCommandWrite, base + offset, firmware.data() + offset,
                       std::min(kFirmwarePageBytes, size - offset));
    }
    ok = ok && BootCommand(kCommandExecute, entry, nullptr, 0);
    usb_.libusb_release_interface(handle_, 0);
    return ok;
  }

  // Waits for the device to come back running the firmware.
  bool Reopen() {
    usb_.libusb_close(handle_);
    handle_ = nullptr;
    const auto deadline =
        std::chrono::steady_clock::now() + kReenumerateTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(kReenumeratePollInterval);
      handle_ = usb_.libusb_open_device_with_vid_pid(context_, kVendorMicrosoft,
                                                     kProductKinectAudio);
      if (handle_ && InterfaceCount() >= 2) {
        XELOGI("Kinect: the audio firmware is running");
        return true;
      }
      if (handle_) {
        usb_.libusb_close(handle_);
        handle_ = nullptr;
      }
    }
    XELOGW("Kinect: the audio device didn't come back after its firmware");
    return false;
  }

  bool Start() {
    running_ = true;
    // All submitted before the event thread, which owns the speaker packets'
    // numbering from then on.
    bool ok = true;
    for (int i = 0; ok && i < kTransferCount; ++i) {
      ok = Submit(kMicrophoneEndpoint, kMicrophonePacketBytes) &&
           Submit(kSpeakerEndpoint, kSpeakerPacketBytes);
    }
    event_thread_ = std::thread([this]() {
      timeval timeout = {0, 50000};
      while (running_ || in_flight_) {
        usb_.libusb_handle_events_timeout_completed(context_, &timeout,
                                                    nullptr);
      }
    });
    if (!ok) {
      XELOGW("Kinect: can't start the microphone stream");
      return false;
    }
    XELOGI("Kinect: microphones streaming");
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) {
      return;
    }
    // The completions see running_ off and don't resubmit.
    for (libusb_transfer* transfer : transfers_) {
      usb_.libusb_cancel_transfer(transfer);
    }
    if (event_thread_.joinable()) {
      event_thread_.join();
    }
    for (libusb_transfer* transfer : transfers_) {
      usb_.libusb_free_transfer(transfer);
    }
    transfers_.clear();
  }

  bool Submit(unsigned char endpoint, int packet_bytes) {
    libusb_transfer* transfer = usb_.libusb_alloc_transfer(kPacketsPerTransfer);
    if (!transfer) {
      return false;
    }
    buffers_.emplace_back(size_t(packet_bytes) * kPacketsPerTransfer);
    libusb_fill_iso_transfer(transfer, handle_, endpoint,
                             buffers_.back().data(),
                             packet_bytes * kPacketsPerTransfer,
                             kPacketsPerTransfer, OnTransfer, this, 0);
    libusb_set_iso_packet_lengths(transfer, unsigned(packet_bytes));
    if (endpoint == kSpeakerEndpoint) {
      FillSpeakerPackets(transfer);
    }
    transfers_.push_back(transfer);
    if (usb_.libusb_submit_transfer(transfer) < 0) {
      return false;
    }
    ++in_flight_;
    return true;
  }

  static void LIBUSB_CALL OnTransfer(libusb_transfer* transfer) {
    auto* self = static_cast<KinectMicrophonesLinux*>(transfer->user_data);
    const bool input = transfer->endpoint == kMicrophoneEndpoint;
    if (input && transfer->status == LIBUSB_TRANSFER_COMPLETED) {
      const uint8_t* packet = transfer->buffer;
      for (int i = 0; i < transfer->num_iso_packets; ++i) {
        const libusb_iso_packet_descriptor& descriptor =
            transfer->iso_packet_desc[i];
        if (descriptor.status == LIBUSB_TRANSFER_COMPLETED &&
            descriptor.actual_length == unsigned(kMicrophonePacketBytes)) {
          self->AddPacket(packet);
        }
        packet += descriptor.length;
      }
    }
    if (!input) {
      self->FillSpeakerPackets(transfer);
    }
    if (!self->running_ || transfer->status == LIBUSB_TRANSFER_NO_DEVICE ||
        self->usb_.libusb_submit_transfer(transfer) < 0) {
      if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        XELOGW("Kinect: the audio device went away");
      }
      --self->in_flight_;
    }
  }

  // Silence, with the window and sequence numbers libfreenect found the
  // firmware needs.
  void FillSpeakerPackets(libusb_transfer* transfer) {
    uint8_t* packet = transfer->buffer;
    for (int i = 0; i < transfer->num_iso_packets; ++i) {
      std::memset(packet, 0, kSpeakerPacketBytes);
      packet[0] = uint8_t(speaker_window_);
      packet[1] = uint8_t(speaker_window_ >> 8);
      packet[2] = speaker_sequence_;
      uint8_t marker = 0x01;
      if (speaker_counter_ < 4) {
        marker = uint8_t(
            ((speaker_time_ >> (speaker_counter_ * 4)) & 0xF) << 4 | 0x05);
      } else if (speaker_parity_ == 0 && speaker_counter_ < 8) {
        marker = uint8_t(
            (((speaker_time_ + 23) >> ((speaker_counter_ - 4) * 4)) & 0xF)
                << 4 |
            0x05);
      }
      packet[3] = marker;
      ++speaker_sequence_;
      ++speaker_counter_;
      speaker_time_ += speaker_parity_ == 1 ? 6 : 5;
      if (speaker_sequence_ == 0x80 || speaker_sequence_ == 0x2B ||
          speaker_sequence_ == 0x56) {
        if (speaker_sequence_ == 0x80) {
          speaker_sequence_ = 0;
        }
        speaker_counter_ = 0;
        ++speaker_window_;
        if (++speaker_parity_ == 3) {
          speaker_parity_ = 0;
        }
      }
      packet += transfer->iso_packet_desc[i].length;
    }
  }

  void AddPacket(const uint8_t* packet) {
    const uint32_t magic = LoadLe32(packet);
    const uint16_t channel = uint16_t(packet[4] | packet[5] << 8);
    const uint16_t window = uint16_t(packet[8] | packet[9] << 8);
    if (magic != kMicrophonePacketMagic || channel < 1 || channel > 9) {
      return;
    }
    if (window != assembling_window_) {
      FinishWindow();
      assembling_window_ = window;
      assembling_channels_ = 0;
    }
    // The processed mix (channel 1) is left out, see FinishWindow.
    if (channel == 1) {
      return;
    }
    const uint8_t* samples = packet + kMicrophoneHeaderBytes;
    const uint32_t half_samples = MicrophoneWindow::kSamples / 2;
    int32_t* out = assembling_.microphones[(channel - 2) / 2] +
                   (channel - 2) % 2 * half_samples;
    const int64_t gain = cvars::kinect_microphone_gain;
    for (uint32_t i = 0; i < half_samples; ++i) {
      out[i] = int32_t(std::clamp<int64_t>(
          int64_t(int32_t(LoadLe32(samples + i * 4))) * gain, INT32_MIN,
          INT32_MAX));
    }
    assembling_channels_ |= 1u << (channel - 1);
  }

  void FinishWindow() {
    if ((assembling_channels_ & kRawChannels) != kRawChannels) {
      return;
    }
    // Without the array calibration a console makes in its Kinect audio
    // tuning, the firmware stops sending its processed mix a few windows
    // after starting, so the microphones' average stands in for all of it.
    for (uint32_t i = 0; i < MicrophoneWindow::kSamples; ++i) {
      int64_t sum = 0;
      for (uint32_t m = 0; m < MicrophoneWindow::kMicrophones; ++m) {
        sum += assembling_.microphones[m][i];
      }
      assembling_.processed[i] = int16_t(std::clamp<int64_t>(
          sum / MicrophoneWindow::kMicrophones / 65536, INT16_MIN, INT16_MAX));
    }
    queue_.Push(assembling_);
  }

  Libusb usb_;
  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  std::vector<int> claimed_;
  uint32_t tag_ = 1;

  std::atomic<bool> running_ = false;
  std::atomic<int> in_flight_ = 0;
  std::thread event_thread_;
  std::vector<libusb_transfer*> transfers_;
  std::vector<std::vector<uint8_t>> buffers_;

  // On the event thread only.
  uint16_t speaker_window_ = 0;
  uint8_t speaker_sequence_ = 0;
  uint32_t speaker_counter_ = 0;
  uint32_t speaker_parity_ = 0;
  uint32_t speaker_time_ = 0;
  MicrophoneWindow assembling_ = {};
  uint16_t assembling_window_ = 0;
  uint32_t assembling_channels_ = 0;

  MicrophoneWindowQueue queue_;
};

std::unique_ptr<KinectMicrophones> OpenKinectMicrophones(
    const std::filesystem::path& content_root) {
  auto microphones = std::make_unique<KinectMicrophonesLinux>();
  if (!microphones->Initialize(content_root)) {
    return nullptr;
  }
  return microphones;
}

#else

std::unique_ptr<KinectMicrophones> OpenKinectMicrophones(
    const std::filesystem::path& content_root) {
  return nullptr;
}

#endif  // XE_PLATFORM_LINUX

}  // namespace

std::unique_ptr<KinectMicrophones> KinectMicrophones::Open(
    apu::AudioSystem* audio_system, const std::filesystem::path& content_root) {
  const std::string& mode = cvars::kinect_microphone;
  std::unique_ptr<KinectMicrophones> microphones;
  if (mode == "auto" || mode == "kinect") {
    microphones = OpenKinectMicrophones(content_root);
  }
  if (!microphones && (mode == "auto" || mode == "host")) {
    microphones = OpenHostMicrophones(audio_system);
  }
  if (microphones) {
    XELOGI("Kinect: titles hear the {}", microphones->name());
  } else if (mode != "off") {
    XELOGW("Kinect: nothing to record from, so titles hear no microphones");
  }
  return microphones;
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe

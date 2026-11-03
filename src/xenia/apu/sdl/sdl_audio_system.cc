/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sdl/sdl_audio_system.h"

#include <SDL3/SDL.h>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/sdl/sdl_audio_driver.h"
#include "xenia/base/logging.h"

namespace xe {
namespace apu {
namespace sdl {

std::unique_ptr<AudioSystem> SDLAudioSystem::Create(cpu::Processor* processor) {
  return std::make_unique<SDLAudioSystem>(processor);
}

SDLAudioSystem::SDLAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}

SDLAudioSystem::~SDLAudioSystem() {}

void SDLAudioSystem::Initialize() { AudioSystem::Initialize(); }

X_STATUS SDLAudioSystem::CreateDriver(size_t index,
                                      xe::threading::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = std::make_unique<SDLAudioDriver>(semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    return X_STATUS_UNSUCCESSFUL;
  }

  *out_driver = driver.release();
  return X_STATUS_SUCCESS;
}

AudioDriver* SDLAudioSystem::CreateDriver(xe::threading::Semaphore* semaphore,
                                          uint32_t frequency, uint32_t channels,
                                          bool need_format_conversion) {
  return new SDLAudioDriver(semaphore, frequency, channels,
                            need_format_conversion);
}

namespace {

class SDLAudioCapture : public AudioCapture {
 public:
  ~SDLAudioCapture() override {
    if (stream_) {
      SDL_DestroyAudioStream(stream_);
    }
    if (initialized_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
  }

  bool Open(const std::string& device, uint32_t frequency) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      XELOGW("SDL: can't start audio for recording: {}", SDL_GetError());
      return false;
    }
    initialized_ = true;
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!device.empty()) {
      int count = 0;
      SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
      for (int i = 0; ids && i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        if (name && std::string(name).find(device) != std::string::npos) {
          id = ids[i];
          break;
        }
      }
      SDL_free(ids);
      if (id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING) {
        XELOGW("SDL: no recording device named like \"{}\"; using the default",
               device);
      }
    }
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, int(frequency)};
    stream_ = SDL_OpenAudioDeviceStream(id, &spec, nullptr, nullptr);
    if (!stream_) {
      XELOGW("SDL: can't open a recording device: {}", SDL_GetError());
      return false;
    }
    const char* name =
        SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
    device_name_ = name ? name : "default";
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
  }

  std::string device_name() const override { return device_name_; }

  size_t Read(int16_t* samples, size_t count) override {
    const int bytes =
        SDL_GetAudioStreamData(stream_, samples, int(count * sizeof(int16_t)));
    return bytes > 0 ? size_t(bytes) / sizeof(int16_t) : 0;
  }

 private:
  bool initialized_ = false;
  SDL_AudioStream* stream_ = nullptr;
  std::string device_name_;
};

}  // namespace

std::unique_ptr<AudioCapture> SDLAudioSystem::OpenCapture(
    const std::string& device, uint32_t frequency) {
  auto capture = std::make_unique<SDLAudioCapture>();
  if (!capture->Open(device, frequency)) {
    return nullptr;
  }
  return capture;
}

}  // namespace sdl
}  // namespace apu
}  // namespace xe

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/webcam_kinect.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/helper/sdl/sdl_helper.h"
#include "xenia/kernel/nui/body_renderer.h"
#include "xenia/kernel/nui/kinect_camera.h"
#include "xenia/kernel/nui/kinect_view.h"
#include "xenia/kernel/nui/pose_estimator.h"

#if XE_PLATFORM_LINUX
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <fstream>
#endif

DEFINE_string(kinect_webcam, "",
              "The webcam that stands in for a Kinect without one, by part "
              "of its name. Empty uses the first. Controllers > Kinect Webcam "
              "chooses one.",
              "Kernel");
DEFINE_double(kinect_webcam_fov, 70.0,
              "The webcam's horizontal field of view in degrees, all of its "
              "picture across, for placing the player from it. A narrower mode "
              "cut from it is allowed for.",
              "Kernel");
DEFINE_int32(kinect_webcam_rotation, 0,
             "Turn the webcam's picture this many degrees clockwise (0, 90, "
             "180 or 270) before using it, for a webcam that isn't upright. "
             "On its side, a webcam sees a standing player head to toe from "
             "much nearer: its long side is then up and down. The Kinect view "
             "(F9) shows the picture as it's used, the right way up when this "
             "is right. kinect_webcam_fov is still the webcam's long side's.",
             "Kernel");
DEFINE_double(kinect_webcam_distance, 2.6,
              "How far from the Kinect a webcam player stands at the least, "
              "in meters. One who settles nearer the webcam stands this far "
              "from the Kinect, stepping toward or away from it moving them "
              "from there; one further back stands as far from the Kinect as "
              "from the webcam (up to 3.6 m), with as much room above their "
              "head as a real Kinect would leave. Titles aim the Kinect for "
              "players about this far: nearer, Kinect Adventures has their "
              "feet out of view.",
              "Kernel");
DEFINE_double(kinect_webcam_player_height, 1.70,
              "The webcam player's height in meters, for the body Kinect "
              "titles see.",
              "Kernel");
DEFINE_double(kinect_webcam_depth_gain, 1.5,
              "How much deeper than the pose found in the webcam's picture "
              "the arms and legs reach toward or away from it: the picture "
              "shows depth least, and the pose comes out flattened, so a "
              "rein pulled back or an arm swung forward moves less than the "
              "player's. 1 takes the pose as found.",
              "Kernel");
DEFINE_bool(kinect_webcam_outline, true,
            "Give the body Kinect titles see the webcam player's outline, as "
            "the webcam's picture shows it, rather than only the simple shapes "
            "it's drawn from: titles that show players their bodies, such as "
            "Your Shape: Fitness Evolved's figure, show theirs, and the color "
            "picture fits it.",
            "Kernel");
DEFINE_path(kinect_webcam_models_path, "",
            "The folder with the webcam Kinect's pose models "
            "(pose_detector.onnx and pose_landmarks_detector.onnx). Empty is "
            "the kinect folder in Xenia's storage folder.",
            "Kernel");
DEFINE_path(kinect_onnxruntime_path, "",
            "ONNX Runtime's library (libonnxruntime.so.1), which the webcam "
            "Kinect needs. Empty looks beside the pose models, then in the "
            "system's libraries.",
            "Kernel");
DEFINE_path(kinect_webcam_test_recording, "",
            "For testing: a recording made with kinect_record_path whose "
            "color frames stand in for the webcam's picture.",
            "Kernel");

DECLARE_bool(kinect_mirror);
DECLARE_bool(log_kinect);

namespace xe {
namespace kernel {
namespace nui {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kWidth = BodyRenderer::kWidth;
constexpr int kHeight = BodyRenderer::kHeight;
// The virtual Kinect's height above the floor, about a TV's.
constexpr float kCameraHeight = 0.85f;
// Where the player stands at rest is taken once they've stood within
// kSettleRange for kSettleSeconds, or after kMaxSettleSeconds however they
// move; after kForgetSeconds out of view they settle afresh. Their height at
// rest follows them over kHeightFollowSeconds, for jumps.
constexpr double kSettleSeconds = 1.0;
constexpr float kSettleRange = 0.08f;
constexpr double kMaxSettleSeconds = 3.0;
constexpr double kForgetSeconds = 3.0;
// Nearer the webcam than this, as when aiming or moving it, the player isn't
// playing: they settle afresh once they're back.
constexpr float kAtWebcamDistance = 0.8f;
constexpr float kHeightFollowSeconds = 4.0f;
// MediaPipe has a player standing upright in front of a level webcam lean
// toward it, about 13 degrees, and a webcam aimed up or down adds its pitch.
// The lean their torso has while they stand still, the median of the last
// kLeanSamples since they settled, is taken out of the pose, updated each
// kLeanSamplesPerUpdate and eased in over kLeanFollowSeconds.
constexpr size_t kLeanSamples = 1800;
constexpr size_t kLeanSamplesPerUpdate = 30;
constexpr float kLeanFollowSeconds = 0.5f;
// How slowly the scale of MediaPipe's body to the drawn one follows it.
constexpr float kScaleFollowSeconds = 5.0f;
constexpr float kMinVisibility = 0.5f;
// A leg's bones follow the webcam's picture once it shows the joint at each
// one's end (in the picture, at kLegShownVisibility), until it hasn't for
// kLegHoldSeconds (out of the picture, or below kLegHiddenVisibility):
// MediaPipe's visibility drops while one leg passes behind the other, and its
// estimate of the hidden one holds up. Otherwise the leg stands straight. The
// bones blend between the two over kLegBlendSeconds.
constexpr float kLegShownVisibility = 0.5f;
constexpr float kLegHiddenVisibility = 0.15f;
constexpr double kLegHoldSeconds = 0.3;
constexpr float kLegBlendSeconds = 0.25f;
// The drawn body's bones, in meters for a player 1.70 m tall, scaled to
// kinect_webcam_player_height: the proportions of the bodies titles' trackers
// were tested with (tools' kbodyrender.py). The hips are 0.49 m below the
// shoulders' center. The arms are as long as Kinect Sports' and Your Shape's
// trackers measure a 1.90 m player's on a real Kinect, scaled.
constexpr float kHeadLength = 0.22f;  // Shoulders' center to the head's.
constexpr float kShoulderHalfWidth = 0.19f;
constexpr float kUpperArmLength = 0.313f;
constexpr float kForearmLength = 0.242f;
constexpr float kHandLength = 0.08f;  // Wrist to the hand's center.
constexpr float kHipHalfWidth = 0.09f;
constexpr float kThighLength = 0.42f;
constexpr float kShinLength = 0.41f;
constexpr float kFootLength = 0.14f;  // Ankle to toes.
// The players' range in front of the virtual Kinect, short of its room's back
// wall.
constexpr float kMinDistance = 1.5f;
constexpr float kMaxDistance = 3.6f;
constexpr int kPoseThreads = 2;
// A recorded webcam picture's header (KinectRecorder::Stream::kWebcam).
constexpr size_t kWebcamHeaderBytes = 8;

using L = PoseLandmarks;

// The webcam chosen with ChooseWebcam, which the camera thread reads in place
// of kinect_webcam so it never reads the cvar while the UI writes it.
struct WebcamChoice {
  std::mutex mutex;
  bool chosen = false;
  std::string part_of_name;
  // Bumped by each choice, for a streaming webcam to switch.
  std::atomic<uint32_t> generation{0};
};
WebcamChoice& webcam_choice() {
  static WebcamChoice choice;
  return choice;
}

// The One Euro filter: smooth while still, little lag while moving.
class OneEuroFilter {
 public:
  OneEuroFilter(float min_cutoff, float beta)
      : min_cutoff_(min_cutoff), beta_(beta) {}
  float Filter(float x, double time) {
    if (!started_ || time <= time_) {
      started_ = true;
      x_ = x;
      dx_ = 0.0f;
      time_ = time;
      return x;
    }
    const float dt = float(time - time_);
    time_ = time;
    dx_ += Alpha(1.0f, dt) * ((x - x_) / dt - dx_);
    x_ += Alpha(min_cutoff_ + beta_ * std::abs(dx_), dt) * (x - x_);
    return x_;
  }
  void Reset() { started_ = false; }

 private:
  static float Alpha(float cutoff, float dt) {
    const float tau = 1.0f / (2.0f * kPi * cutoff);
    return 1.0f / (1.0f + tau / dt);
  }
  float min_cutoff_, beta_;
  bool started_ = false;
  float x_ = 0.0f, dx_ = 0.0f;
  double time_ = 0.0;
};

// An RGB frame, 3 bytes a pixel, tightly packed.
struct RgbFrame {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;
};

// Webcams may halve their frame rate in dim light to expose for longer
// (V4L2's exposure_auto_priority), leaving titles' trackers half the frames
// and the player's movements late. While it's in use, the webcam keeps its
// frame rate instead, and gets its setting back after.
class KeepFrameRate {
 public:
  ~KeepFrameRate() {
#if XE_PLATFORM_LINUX
    for (const Changed& changed : changed_) {
      v4l2_control control = {V4L2_CID_EXPOSURE_AUTO_PRIORITY,
                              changed.old_value};
      ioctl(changed.fd, VIDIOC_S_CTRL, &control);
      close(changed.fd);
    }
#endif
  }

  // For the video devices with the camera's name, as V4L2 gives it.
  void Apply(const std::string& camera_name) {
#if XE_PLATFORM_LINUX
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator("/sys/class/video4linux", error)) {
      std::ifstream name_file(entry.path() / "name");
      std::string name;
      std::getline(name_file, name);
      if (name != camera_name) {
        continue;
      }
      const std::string device = "/dev/" + entry.path().filename().string();
      const int fd = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0) {
        continue;
      }
      v4l2_control control = {V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0};
      if (ioctl(fd, VIDIOC_G_CTRL, &control) == 0 && control.value) {
        const int32_t old_value = control.value;
        control.value = 0;
        if (ioctl(fd, VIDIOC_S_CTRL, &control) == 0) {
          XELOGI(
              "Kinect: the webcam keeps its frame rate in dim light while in "
              "use");
          changed_.push_back({fd, old_value});
          continue;
        }
      }
      close(fd);
    }
#endif
  }

 private:
  struct Changed {
    int fd;
    int32_t old_value;
  };
  std::vector<Changed> changed_;
};

// Where frames come from: the webcam, or for testing a recording's color.
class FrameSource {
 public:
  virtual ~FrameSource() = default;
  // The next frame, waiting a little for it. False if none came.
  virtual bool Next(RgbFrame& frame) = 0;
  virtual std::string name() const = 0;
  // How much of the webcam's full view across the frames show, as the
  // tangent of half of it: 0.75 for a 4:3 mode cut from a 16:9 sensor.
  // kinect_webcam_fov is the full view's.
  virtual float field_fraction() const { return 1.0f; }
  // How long ago the last frame was taken, as far as the source knows, in
  // milliseconds (relative to how old its first was).
  virtual double frame_age_ms() const { return 0.0; }
};

class SdlCameraSource : public FrameSource {
 public:
  ~SdlCameraSource() override {
    if (camera_) {
      SDL_CloseCamera(camera_);
    }
    if (initialized_) {
      SDL_QuitSubSystem(SDL_INIT_CAMERA);
    }
  }

  bool Initialize() {
    if (!xe::helper::sdl::SDLHelper::Prepare() ||
        !SDL_InitSubSystem(SDL_INIT_CAMERA)) {
      XELOGW("Kinect: no camera support: {}", SDL_GetError());
      return false;
    }
    initialized_ = true;
    int count = 0;
    SDL_CameraID* ids = SDL_GetCameras(&count);
    SDL_CameraID chosen = 0;
    const std::string wanted_name = ChosenWebcam();
    const std::string wanted = xe::utf8::lower_ascii(wanted_name);
    std::string all_names;
    for (int i = 0; i < count; ++i) {
      const char* name = SDL_GetCameraName(ids[i]);
      const std::string shown = name ? name : "camera";
      all_names += (i ? ", " : "") + shown;
      if (!chosen && (wanted.empty() || xe::utf8::lower_ascii(shown).find(
                                            wanted) != std::string::npos)) {
        chosen = ids[i];
        name_ = shown;
      }
    }
    SDL_free(ids);
    if (count > 1) {
      XELOGI("Kinect: webcams: {}", all_names);
    }
    if (!chosen) {
      XELOGW(
          "Kinect: no webcam{} (SDL camera driver: {})",
          wanted.empty() ? "" : " named " + wanted_name,
          SDL_GetCurrentCameraDriver() ? SDL_GetCurrentCameraDriver() : "none");
      return false;
    }
    // Webcams often make modes of another shape than their sensor's by
    // cutting its sides or top and bottom off, so the mode of the shape of
    // the largest one, which is the whole sensor, sees most of the player.
    // Of those, the one nearest 640x480 at 25 frames a second or more,
    // uncompressed if there's a choice; SDL converts it to RGB.
    SDL_CameraSpec spec = {};
    int formats = 0;
    SDL_CameraSpec** specs = SDL_GetCameraSupportedFormats(chosen, &formats);
    float sensor_aspect = 0.0f;
    int sensor_area = 0;
    for (int i = 0; specs && i < formats; ++i) {
      const SDL_CameraSpec& s = *specs[i];
      if (s.width * s.height > sensor_area && s.height > 0) {
        sensor_area = s.width * s.height;
        sensor_aspect = float(s.width) / float(s.height);
      }
    }
    int best_score = INT32_MAX;
    for (int i = 0; specs && i < formats; ++i) {
      const SDL_CameraSpec& s = *specs[i];
      if (s.width <= 0 || s.height <= 0) {
        continue;
      }
      const float fps = s.framerate_denominator ? float(s.framerate_numerator) /
                                                      s.framerate_denominator
                                                : 0.0f;
      const float aspect = float(s.width) / float(s.height);
      const bool whole_sensor =
          std::abs(aspect - sensor_aspect) < 0.02f * sensor_aspect;
      int score = std::abs(s.width * s.height - kWidth * kHeight) / 1000 +
                  (s.format == SDL_PIXELFORMAT_MJPG ? 50 : 0) +
                  (whole_sensor ? 0 : 1000) + (fps < 25.0f ? 10000 : 0);
      if (score < best_score) {
        best_score = score;
        spec = s;
      }
    }
    SDL_free(specs);
    const bool chose_spec = best_score != INT32_MAX;
    if (chose_spec) {
      // A mode narrower than the sensor has its sides cut off.
      const float aspect = float(spec.width) / float(spec.height);
      field_fraction_ =
          sensor_aspect > 0.0f ? std::min(1.0f, aspect / sensor_aspect) : 1.0f;
      spec.format = SDL_PIXELFORMAT_RGB24;
      spec.colorspace = SDL_COLORSPACE_SRGB;
    }
    camera_ = SDL_OpenCamera(chosen, chose_spec ? &spec : nullptr);
    if (!camera_) {
      XELOGW("Kinect: can't open the webcam {}: {}", name_, SDL_GetError());
      return false;
    }
    if (chose_spec) {
      XELOGI(
          "Kinect: a webcam stands in for the Kinect: {}, {}x{}, {:.0f}% of "
          "its view across",
          name_, spec.width, spec.height, field_fraction_ * 100.0f);
    } else {
      XELOGI("Kinect: a webcam stands in for the Kinect: {}", name_);
    }
    keep_frame_rate_.Apply(name_);
    return true;
  }

  float field_fraction() const override { return field_fraction_; }
  double frame_age_ms() const override { return frame_age_ms_; }

  bool Next(RgbFrame& frame) override {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(100);
    while (clock::now() < deadline) {
      if (SDL_GetCameraPermissionState(camera_) ==
          SDL_CAMERA_PERMISSION_STATE_DENIED) {
        return false;
      }
      // The newest frame, dropping any older ones waiting.
      SDL_Surface* newest = nullptr;
      Uint64 timestamp = 0;
      while (SDL_Surface* surface =
                 SDL_AcquireCameraFrame(camera_, &timestamp)) {
        if (newest) {
          SDL_ReleaseCameraFrame(camera_, newest);
        }
        newest = surface;
      }
      if (!newest) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      frame_age_ms_ =
          timestamp ? double(SDL_GetTicksNS() - timestamp) / 1e6 : 0.0;
      bool ok = true;
      SDL_Surface* rgb = newest;
      if (newest->format != SDL_PIXELFORMAT_RGB24) {
        rgb = SDL_ConvertSurface(newest, SDL_PIXELFORMAT_RGB24);
        ok = rgb != nullptr;
      }
      if (ok) {
        frame.width = rgb->w;
        frame.height = rgb->h;
        frame.pixels.resize(size_t(rgb->w) * rgb->h * 3);
        for (int y = 0; y < rgb->h; ++y) {
          std::memcpy(&frame.pixels[size_t(y) * rgb->w * 3],
                      static_cast<const uint8_t*>(rgb->pixels) + y * rgb->pitch,
                      size_t(rgb->w) * 3);
        }
        if (rgb != newest) {
          SDL_DestroySurface(rgb);
        }
      }
      SDL_ReleaseCameraFrame(camera_, newest);
      return ok;
    }
    return false;
  }

  std::string name() const override { return name_; }

 private:
  bool initialized_ = false;
  SDL_Camera* camera_ = nullptr;
  std::string name_;
  float field_fraction_ = 1.0f;
  double frame_age_ms_ = 0.0;
  KeepFrameRate keep_frame_rate_;
};

// A Kinect recording's color frames, demosaiced, at their own pace, looping;
// or, if it has them, the webcam's own pictures a webcam stand-in recorded.
class RecordingSource : public FrameSource {
 public:
  ~RecordingSource() override {
    if (file_) {
      fclose(file_);
    }
  }

  bool Initialize(const std::filesystem::path& path) {
    file_ = xe::filesystem::OpenFile(path, "rb");
    if (!file_) {
      XELOGW("Kinect: can't open the test recording {}",
             xe::strip_pii(xe::path_to_utf8(path)));
      return false;
    }
    XELOGI("Kinect: a recording's color stands in for the webcam: {}",
           xe::strip_pii(xe::path_to_utf8(path)));
    return true;
  }

  bool Next(RgbFrame& frame) override {
    using clock = std::chrono::steady_clock;
    struct Header {
      uint32_t stream, time_ms, size;
    } header;
    std::vector<uint8_t> bayer;
    while (true) {
      if (fread(&header, sizeof(header), 1, file_) != 1 ||
          header.size > 4 * 1024 * 1024) {
        if (std::ftell(file_) <= long(sizeof(header))) {
          return false;
        }
        std::fseek(file_, 0, SEEK_SET);
        loop_start_ = clock::now();
        continue;
      }
      if (header.stream == uint32_t(KinectRecorder::Stream::kWebcam) &&
          header.size > kWebcamHeaderBytes) {
        webcam_pictures_ = true;
      } else if (webcam_pictures_ ||
                 (header.stream != 1 && header.stream != 2) ||
                 header.size != uint32_t(kWidth * kHeight)) {
        std::fseek(file_, header.size, SEEK_CUR);
        continue;
      }
      bayer.resize(header.size);
      if (fread(bayer.data(), 1, bayer.size(), file_) != bayer.size()) {
        continue;
      }
      break;
    }
    if (!started_) {
      started_ = true;
      loop_start_ = clock::now();
    }
    std::this_thread::sleep_until(loop_start_ +
                                  std::chrono::milliseconds(header.time_ms));
    if (header.stream == uint32_t(KinectRecorder::Stream::kWebcam)) {
      uint16_t size[2];
      std::memcpy(size, bayer.data(), sizeof(size));
      std::memcpy(&field_fraction_, bayer.data() + 4, sizeof(float));
      const size_t pixels = size_t(size[0]) * size[1] * 3;
      if (bayer.size() < kWebcamHeaderBytes + pixels) {
        return false;
      }
      frame.width = size[0];
      frame.height = size[1];
      frame.pixels.assign(bayer.begin() + kWebcamHeaderBytes,
                          bayer.begin() + kWebcamHeaderBytes + pixels);
      return true;
    }
    // GRBG at half the resolution, then doubled.
    frame.width = kWidth;
    frame.height = kHeight;
    frame.pixels.resize(size_t(kWidth) * kHeight * 3);
    for (int y = 0; y < kHeight; ++y) {
      const int by = std::min(y / 2, kHeight / 2 - 1) * 2;
      for (int x = 0; x < kWidth; ++x) {
        const int bx = std::min(x / 2, kWidth / 2 - 1) * 2;
        const uint8_t* top = &bayer[size_t(by) * kWidth + bx];
        const uint8_t* bottom = top + kWidth;
        uint8_t* out = &frame.pixels[(size_t(y) * kWidth + x) * 3];
        out[0] = top[1];
        out[1] = uint8_t((top[0] + bottom[1]) / 2);
        out[2] = bottom[0];
      }
    }
    return true;
  }

  std::string name() const override { return "a recording"; }
  float field_fraction() const override { return field_fraction_; }

 private:
  FILE* file_ = nullptr;
  bool webcam_pictures_ = false;
  float field_fraction_ = 1.0f;
  bool started_ = false;
  std::chrono::steady_clock::time_point loop_start_;
};

class KinectCameraWebcam : public KinectCamera {
 public:
  ~KinectCameraWebcam() override { Stop(); }

  bool Initialize(const std::filesystem::path& storage_root) {
    const std::filesystem::path models =
        cvars::kinect_webcam_models_path.empty()
            ? storage_root / "kinect"
            : cvars::kinect_webcam_models_path;
    pose_estimator_ = PoseEstimator::Create(
        models, cvars::kinect_onnxruntime_path, kPoseThreads);
    if (!pose_estimator_) {
      XELOGW(
          "Kinect: a webcam can't stand in for a Kinect without ONNX Runtime "
          "and the pose models in {} (see tools/kinect/)",
          xe::strip_pii(xe::path_to_utf8(models)));
      return false;
    }
    if (!cvars::kinect_webcam_test_recording.empty()) {
      auto source = std::make_unique<RecordingSource>();
      if (!source->Initialize(cvars::kinect_webcam_test_recording)) {
        return false;
      }
      source_ = std::move(source);
    } else {
      webcam_generation_ = webcam_choice().generation;
      auto source = std::make_unique<SdlCameraSource>();
      if (!source->Initialize()) {
        return false;
      }
      source_ = std::move(source);
    }
    renderer_.SetCamera(kCameraHeight, 0.0f);
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
    thread_ = std::thread([this]() { ThreadMain(); });
    return true;
  }

  bool IsColorStreaming() const override { return bool(color_callback_); }
  bool IsColorRegistered() const override { return true; }
  bool NeedsSimulatedMotor() const override { return true; }
  void SetRecorder(KinectRecorder* recorder) override { recorder_ = recorder; }

  void Stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  // Where the player is and how they stand, from what the webcam saw.
  struct Placement {
    // MediaPipe's origin, between the hips: from the webcam (meters, y up),
    // and where it is in the room.
    float t[3];
    Vec3 anchor;
    float webcam_focal;
    bool legs_seen;
    // From the Kinect, in the room titles see.
    float play_distance;
    // Whether the picture shows each joint where it's drawn.
    std::array<bool, size_t(BodyJoint::kCount)> shown;
    // How far out from between MediaPipe's hips the drawn hip joints are,
    // as a fraction of the way to its.
    float hip_fraction;
    // How much larger the drawn body is than MediaPipe's, and the player
    // than t has them.
    float mediapipe_scale;
  };

  // Whether a leg's bone follows the picture (see kLegShownVisibility).
  struct LegBone {
    bool following = false;
    double hidden_since = -1.0;
    // 0 standing straight to 1 following.
    float weight = 0.0f;
    double time = 0.0;

    void Update(bool shown, bool hidden, double now) {
      if (!following) {
        following = shown;
        hidden_since = -1.0;
      } else if (!hidden) {
        hidden_since = -1.0;
      } else if (hidden_since < 0.0) {
        hidden_since = now;
      } else if (now - hidden_since > kLegHoldSeconds) {
        following = false;
      }
      const float dt =
          float(std::clamp(now - time, 0.0, 0.1)) / kLegBlendSeconds;
      time = now;
      weight =
          following ? std::min(weight + dt, 1.0f) : std::max(weight - dt, 0.0f);
    }
  };

  void ThreadMain() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    RgbFrame frame;
    std::vector<uint16_t> raw(size_t(kWidth) * kHeight);
    std::vector<uint8_t> depth(kDepthFrameBytes);
    std::vector<uint8_t> color(kColorFrameBytes);
    uint32_t frames = 0, found = 0;
    double pose_seconds = 0.0;
    auto report = start;
    while (running_) {
      if (cvars::kinect_webcam_test_recording.empty() &&
          webcam_generation_ != webcam_choice().generation) {
        SwitchWebcam();
      }
      if (!source_) {
        // The chosen webcam couldn't be opened; another may be chosen.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      if (!source_->Next(frame)) {
        continue;
      }
      const auto now = clock::now();
      const double time = std::chrono::duration<double>(now - start).count();
      LogKinectTiming("capture", source_->frame_age_ms());
      if (recorder_) {
        RecordPicture(frame);
      }
      RotateFrame(frame);
      // The view as titles have tilted the (simulated) sensor: they find the
      // floor and frame their players by tilting it.
      renderer_.SetCamera(kCameraHeight, KinectMotor::SimulatedTiltDegrees());
      PoseLandmarks pose;
      const bool seen =
          pose_estimator_->Process(frame.pixels.data(), frame.width,
                                   frame.height, size_t(frame.width) * 3, pose);
      pose_seconds += std::chrono::duration<double>(clock::now() - now).count();
      Body body;
      Placement placement;
      const bool placed = seen && PlaceBody(pose, frame.width, frame.height,
                                            time, body, placement);
      if (!seen) {
        ResetTracking();
      }
      found += seen;
      if (placed) {
        // The hands' heights the title is shown, to time its skeletons by.
        LogKinectTiming("hand_left", body[BodyJoint::kHandLeft].y);
        LogKinectTiming("hand_right", body[BodyJoint::kHandRight].y);
      }
      BodyRenderer::Outline outline;
      const PoseMask& mask = pose_estimator_->mask();
      const bool outlined = placed && cvars::kinect_webcam_outline &&
                            mask.valid() &&
                            MakeOutline(pose, body, placement, time, outline);
      if (outlined) {
        outline.mask = [&mask](float x, float y) { return mask.At(x, y); };
      } else {
        outline_scale_filter_.Reset();
      }
      renderer_.Render(&body, placed ? 1 : 0,
                       float(cvars::kinect_webcam_player_height), raw.data(),
                       outlined ? &outline : nullptr);
      if (cvars::kinect_mirror) {
        for (int y = 0; y < kHeight; ++y) {
          std::reverse(raw.begin() + y * kWidth,
                       raw.begin() + (y + 1) * kWidth);
        }
      }
      PackKinectDepth(raw.data(), depth.data());
      const uint32_t timestamp = uint32_t(time * 1000.0);
      depth_callback_(depth.data(), depth.size(), timestamp);
      if (color_callback_) {
        MakeColor(frame, placed ? &body : nullptr, placement,
                  outlined ? &outline : nullptr, outlined ? &mask : nullptr,
                  color.data());
        color_callback_(color.data(), color.size(), timestamp);
      }
      ++frames;
      if (KinectView::Get().shown()) {
        PublishView(frame, seen ? &pose : nullptr,
                    placed ? &placement : nullptr, time,
                    std::chrono::duration<double>(clock::now() - now).count());
      }
      if (cvars::log_kinect && now - report > std::chrono::seconds(10)) {
        const double seconds =
            std::chrono::duration<double>(now - report).count();
        XELOGI(
            "Kinect: webcam {:.1f} frames a second, the player seen in {}%, "
            "the pose taking {:.1f} ms",
            frames / seconds, frames ? found * 100 / frames : 0,
            frames ? pose_seconds * 1000.0 / frames : 0.0);
        report = now;
        frames = found = 0;
        pose_seconds = 0.0;
      }
    }
  }

  void RecordPicture(const RgbFrame& frame) {
    const uint16_t size[2] = {uint16_t(frame.width), uint16_t(frame.height)};
    const float field = source_->field_fraction();
    picture_.resize(kWebcamHeaderBytes + frame.pixels.size());
    std::memcpy(picture_.data(), size, sizeof(size));
    std::memcpy(picture_.data() + 4, &field, sizeof(field));
    std::memcpy(picture_.data() + kWebcamHeaderBytes, frame.pixels.data(),
                frame.pixels.size());
    recorder_->AddFrame(KinectRecorder::Stream::kWebcam, picture_.data(),
                        picture_.size());
  }

  // For the Kinect view: the picture, what was found in it, and how fast.
  void PublishView(const RgbFrame& frame, const PoseLandmarks* pose,
                   const Placement* placement, double time,
                   double pose_seconds) {
    const float dt = float(time - view_time_);
    view_time_ = time;
    if (dt > 0.0f && dt < 1.0f) {
      view_fps_ += (1.0f / dt - view_fps_) * 0.1f;
    }
    view_pose_ms_ += (float(pose_seconds) * 1000.0f - view_pose_ms_) * 0.1f;
    KinectView::Webcam webcam;
    webcam.name = source_->name();
    webcam.frames_per_second = view_fps_;
    webcam.pose_ms = view_pose_ms_;
    webcam.seen = pose && placement;
    webcam.settled = settled_;
    webcam.tilt_degrees = KinectMotor::SimulatedTiltDegrees();
    if (webcam.seen) {
      webcam.legs_seen = placement->legs_seen;
      webcam.distance = placement->t[2] * placement->mediapipe_scale;
      webcam.play_distance = placement->play_distance;
      for (int i = 0; i < L::kCount; ++i) {
        webcam.landmarks[i][0] = pose->image[i][0] / frame.width;
        webcam.landmarks[i][1] = pose->image[i][1] / frame.height;
        webcam.visibility[i] = pose->visibility[i];
      }
    }
    KinectView::Get().PublishWebcam(frame.pixels.data(), frame.width,
                                    frame.height, webcam);
  }

  void ResetTracking() {
    for (OneEuroFilter& filter : landmark_filters_) {
      filter.Reset();
    }
    for (OneEuroFilter& filter : position_filters_) {
      filter.Reset();
    }
    for (LegBone& bone : leg_bones_) {
      bone = LegBone();
    }
    outline_scale_filter_.Reset();
    for (OneEuroFilter& filter : image_filters_) {
      filter.Reset();
    }
    mediapipe_scale_ = 0.0f;
  }

  // Streams from the webcam ChooseWebcam chose instead. A webcam can only be
  // opened once, so the one streaming is closed first. The player settles
  // afresh in front of the new one.
  void SwitchWebcam() {
    webcam_generation_ = webcam_choice().generation;
    source_.reset();
    auto source = std::make_unique<SdlCameraSource>();
    if (source->Initialize()) {
      source_ = std::move(source);
    }
    ResetTracking();
    have_rest_ = false;
  }

  // The hip center's position from the webcam, by least squares over the
  // visible landmarks: each one's picture position is its world position
  // (relative to the hips) plus this, seen through the webcam's lens.
  static bool SolvePosition(const PoseLandmarks& pose, int width, int height,
                            float focal, float t[3]) {
    static const int kSolve[] = {
        L::kNose,          L::kEarLeft,   L::kEarRight,   L::kShoulderLeft,
        L::kShoulderRight, L::kElbowLeft, L::kElbowRight, L::kWristLeft,
        L::kWristRight,    L::kHipLeft,   L::kHipRight,   L::kKneeLeft,
        L::kKneeRight,     L::kAnkleLeft, L::kAnkleRight};
    const float cx = width * 0.5f;
    const float cy = height * 0.5f;
    double ata[3][3] = {};
    double atb[3] = {};
    int rows = 0;
    auto add_row = [&](const float a[3], float b, float weight) {
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          ata[i][j] += weight * a[i] * a[j];
        }
        atb[i] += weight * a[i] * b;
      }
    };
    for (int i : kSolve) {
      const float w = pose.visibility[i];
      if (w < kMinVisibility) {
        continue;
      }
      // y up, as t is.
      const float x = pose.world[i][0];
      const float y = -pose.world[i][1];
      const float z = pose.world[i][2];
      const float du = pose.image[i][0] - cx;
      const float dv = pose.image[i][1] - cy;
      const float row_u[3] = {focal, 0.0f, -du};
      add_row(row_u, du * z - focal * x, w);
      const float row_v[3] = {0.0f, focal, dv};
      add_row(row_v, -dv * z - focal * y, w);
      rows += 2;
    }
    if (rows < 8) {
      return false;
    }
    // Cramer's rule.
    auto det3 = [](const double m[3][3]) {
      return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double det = det3(ata);
    if (std::abs(det) < 1e-9) {
      return false;
    }
    for (int k = 0; k < 3; ++k) {
      double m[3][3];
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          m[i][j] = j == k ? atb[i] : ata[i][j];
        }
      }
      t[k] = float(det3(m) / det);
    }
    return t[2] > 0.3f;
  }

  // The webcam's focal length in pixels, for its frames this size: its long
  // side is kinect_webcam_fov's, whichever way the picture is turned.
  float WebcamFocal(int width, int height) const {
    return std::max(width, height) * 0.5f /
           (std::tan(float(cvars::kinect_webcam_fov) * kPi / 360.0f) *
            source_->field_fraction());
  }

  // Turns the webcam's picture upright (kinect_webcam_rotation).
  void RotateFrame(RgbFrame& frame) {
    const int turns = ((cvars::kinect_webcam_rotation / 90) % 4 + 4) % 4;
    if (!turns) {
      return;
    }
    const int w = frame.width, h = frame.height;
    turned_.width = turns & 1 ? h : w;
    turned_.height = turns & 1 ? w : h;
    turned_.pixels.resize(frame.pixels.size());
    for (int y = 0; y < turned_.height; ++y) {
      for (int x = 0; x < turned_.width; ++x) {
        // The picture's pixel that lands here.
        int sx, sy;
        switch (turns) {
          case 1:  // A quarter turn clockwise.
            sx = y;
            sy = h - 1 - x;
            break;
          case 2:
            sx = w - 1 - x;
            sy = h - 1 - y;
            break;
          default:  // Three quarters clockwise.
            sx = w - 1 - y;
            sy = x;
            break;
        }
        std::memcpy(&turned_.pixels[(size_t(y) * turned_.width + x) * 3],
                    &frame.pixels[(size_t(sy) * w + sx) * 3], 3);
      }
    }
    std::swap(frame, turned_);
  }

  bool PlaceBody(const PoseLandmarks& raw_pose, int width, int height,
                 double time, Body& body, Placement& placement) {
    const float focal = WebcamFocal(width, height);
    float t[3];
    if (!SolvePosition(raw_pose, width, height, focal, t)) {
      return false;
    }
    PoseLandmarks pose = raw_pose;
    for (int i = 0; i < L::kCount; ++i) {
      for (int k = 0; k < 3; ++k) {
        pose.world[i][k] =
            landmark_filters_[i * 3 + k].Filter(pose.world[i][k], time);
      }
    }
    for (int k = 0; k < 3; ++k) {
      t[k] = position_filters_[k].Filter(t[k], time);
    }
    const float lean = TorsoLean(pose);
    StraightenPose(pose, time);
    // In the room's axes (y up), from MediaPipe's origin between the hips.
    auto world = [&](int i) {
      return Vec3(pose.world[i][0], -pose.world[i][1], pose.world[i][2]);
    };
    auto mid = [&](int a, int b) { return (world(a) + world(b)) * 0.5f; };
    auto visible = [&](int i) { return pose.visibility[i] >= kMinVisibility; };
    using J = BodyJoint;
    const float s = float(cvars::kinect_webcam_player_height) / 1.70f;
    const Vec3 shoulders = mid(L::kShoulderLeft, L::kShoulderRight);
    Vec3 hips = mid(L::kHipLeft, L::kHipRight);
    Vec3 hip_left = world(L::kHipLeft);
    Vec3 hip_right = world(L::kHipRight);
    const bool hips_seen = visible(L::kHipLeft) && visible(L::kHipRight);
    // How much larger the drawn body is than MediaPipe's, whose comes out
    // smaller than the player's: by the shoulders, and the torso if the hips
    // are seen.
    const float shoulder_width =
        (world(L::kShoulderLeft) - world(L::kShoulderRight)).Length();
    const float measured_scale =
        hips_seen
            ? (2.0f * kShoulderHalfWidth + 0.49f) * s /
                  std::max(shoulder_width + (shoulders - hips).Length(), 0.1f)
            : 2.0f * kShoulderHalfWidth * s / std::max(shoulder_width, 0.05f);
    // A constant of the player's, which MediaPipe's shoulders shift a little
    // with their arms: followed slowly.
    if (mediapipe_scale_ <= 0.0f) {
      mediapipe_scale_ = measured_scale;
    } else {
      mediapipe_scale_ +=
          (measured_scale - mediapipe_scale_) *
          (1.0f - std::exp(-float(std::clamp(time - scale_time_, 0.0, 1.0)) /
                           kScaleFollowSeconds));
    }
    scale_time_ = time;
    const float mediapipe_scale = mediapipe_scale_;
    if (!hips_seen) {
      // Out of view, as they are close to the webcam, the hips MediaPipe
      // guesses are too high, shortening the body: a torso of the usual
      // length under the shoulders instead.
      const Vec3 up = (shoulders - hips).Normalized();
      const Vec3 left =
          (world(L::kShoulderLeft) - world(L::kShoulderRight)).Normalized();
      hips = shoulders - up * (0.49f * s);
      hip_left = hips + left * (0.12f * s);
      hip_right = hips - left * (0.12f * s);
    }
    // Around the drawn hips: MediaPipe's bones' directions, at the lengths of
    // a body of the player's height. MediaPipe's own lengths come out about a
    // fifth short, its limbs most (a forward swing then doesn't reach as far
    // as titles look for), and they change with how much of the player the
    // webcam sees.
    // Limbs reach deeper toward or away from the webcam than found
    // (kinect_webcam_depth_gain).
    const float depth_gain = float(cvars::kinect_webcam_depth_gain);
    auto direction = [&](const Vec3& from, const Vec3& to) {
      Vec3 v = to - from;
      v.z *= depth_gain;
      return v.Normalized();
    };
    auto bone = [&](const Vec3& from, const Vec3& to, float length) {
      return direction(from, to) * (length * s);
    };
    // Perpendicular to the torso, toward the player's left.
    auto across = [&](const Vec3& left_side, const Vec3& up) {
      return (left_side - up * left_side.Dot(up)).Normalized();
    };
    const Vec3 up = (shoulders - hips).Normalized();
    const Vec3 shoulder_side =
        across(world(L::kShoulderLeft) - world(L::kShoulderRight), up);
    const Vec3 hip_side = across(hip_left - hip_right, up);
    std::array<bool, size_t(J::kCount)>& shown = placement.shown;
    shown.fill(false);
    const bool shoulders_seen =
        visible(L::kShoulderLeft) && visible(L::kShoulderRight);
    body[J::kHipCenter] = Vec3();
    body[J::kShoulderCenter] = up * (0.49f * s);
    body[J::kSpine] = body[J::kShoulderCenter] * 0.2f;
    body[J::kHead] = body[J::kShoulderCenter] +
                     (mid(L::kEarLeft, L::kEarRight) - shoulders).Normalized() *
                         (kHeadLength * s);
    shown[size_t(J::kHipCenter)] = hips_seen;
    shown[size_t(J::kShoulderCenter)] = shoulders_seen;
    shown[size_t(J::kSpine)] = hips_seen && shoulders_seen;
    shown[size_t(J::kHead)] = visible(L::kEarLeft) || visible(L::kEarRight);
    for (auto [shoulder, elbow, wrist, hand, sign, mp_shoulder, mp_elbow,
               mp_wrist, mp_pinky, mp_index] :
         {std::tuple{J::kShoulderLeft, J::kElbowLeft, J::kWristLeft,
                     J::kHandLeft, 1.0f, L::kShoulderLeft, L::kElbowLeft,
                     L::kWristLeft, L::kPinkyLeft, L::kIndexLeft},
          std::tuple{J::kShoulderRight, J::kElbowRight, J::kWristRight,
                     J::kHandRight, -1.0f, L::kShoulderRight, L::kElbowRight,
                     L::kWristRight, L::kPinkyRight, L::kIndexRight}}) {
      body[shoulder] = body[J::kShoulderCenter] +
                       shoulder_side * (sign * kShoulderHalfWidth * s) -
                       up * (0.03f * s);
      body[elbow] = body[shoulder] +
                    bone(world(mp_shoulder), world(mp_elbow), kUpperArmLength);
      body[wrist] =
          body[elbow] + bone(world(mp_elbow), world(mp_wrist), kForearmLength);
      body[hand] = body[wrist] +
                   bone(world(mp_wrist), mid(mp_pinky, mp_index), kHandLength);
      shown[size_t(shoulder)] = visible(mp_shoulder);
      shown[size_t(elbow)] = visible(mp_elbow);
      shown[size_t(wrist)] = visible(mp_wrist);
      shown[size_t(hand)] = visible(mp_wrist);
    }
    body[J::kHipLeft] = hip_side * (kHipHalfWidth * s) - up * (0.05f * s);
    body[J::kHipRight] = hip_side * (-kHipHalfWidth * s) - up * (0.05f * s);
    shown[size_t(J::kHipLeft)] = shown[size_t(J::kHipRight)] = hips_seen;
    // MediaPipe's hips are wider apart than the drawn ones, the joints titles
    // track, so its thighs slope in to the knees more: a thigh aims from the
    // drawn hip at MediaPipe's knee, as far out as MediaPipe has it, or the
    // knees would meet.
    const float mediapipe_hip_half_width =
        0.5f * (hip_left - hip_right).Length() * mediapipe_scale;
    placement.hip_fraction =
        mediapipe_hip_half_width > 0.02f
            ? std::clamp(kHipHalfWidth * s / mediapipe_hip_half_width, 0.3f,
                         1.5f)
            : 1.0f;
    // Each leg's bones follow the picture while it shows them, a bone at a
    // time from the hip (a webcam that sees down to the knees still shows a
    // squat), and otherwise stand straight, as titles' trackers want.
    auto in_picture = [&](int i) {
      return pose.image[i][0] >= 0.0f && pose.image[i][0] < width &&
             pose.image[i][1] >= 0.0f && pose.image[i][1] < height - 1;
    };
    auto joint_shown = [&](int i) {
      return in_picture(i) && pose.presence[i] >= 0.5f &&
             pose.visibility[i] >= kLegShownVisibility;
    };
    auto joint_hidden = [&](int i) {
      return !in_picture(i) || pose.presence[i] < 0.5f ||
             pose.visibility[i] < kLegHiddenVisibility;
    };
    // Straight down from the hip, the knee a little forward and the foot
    // flat.
    const Vec3 standing[3] = {
        Vec3(0.0f, -kThighLength * s, -0.02f).Normalized(),
        Vec3(0.0f, -kShinLength * s, 0.02f).Normalized(),
        Vec3(0.0f, -0.05f, -0.13f).Normalized()};
    const float lengths[3] = {kThighLength, kShinLength, kFootLength};
    bool legs_seen = true;
    for (auto [side, hip, knee, ankle, foot, mp_hip, mp_knee, mp_ankle,
               mp_foot] :
         {std::tuple{0, J::kHipLeft, J::kKneeLeft, J::kAnkleLeft, J::kFootLeft,
                     L::kHipLeft, L::kKneeLeft, L::kAnkleLeft, L::kFootLeft},
          std::tuple{1, J::kHipRight, J::kKneeRight, J::kAnkleRight,
                     J::kFootRight, L::kHipRight, L::kKneeRight, L::kAnkleRight,
                     L::kFootRight}}) {
      const J joints[4] = {hip, knee, ankle, foot};
      const int landmarks[4] = {mp_hip, mp_knee, mp_ankle, mp_foot};
      bool parent_following = hips_seen;
      for (int b = 0; b < 3; ++b) {
        LegBone& leg_bone = leg_bones_[side * 3 + b];
        const int end = landmarks[b + 1];
        leg_bone.Update(parent_following && joint_shown(end),
                        !parent_following || joint_hidden(end), time);
        parent_following = leg_bone.following;
        const float w = leg_bone.weight;
        Vec3 d = standing[b];
        if (w > 0.0f) {
          // The drawn hip where it is in MediaPipe's body.
          const Vec3 from =
              b ? world(landmarks[b])
                : hips + body[joints[0]] * (1.0f / mediapipe_scale);
          const Vec3 followed = direction(from, world(end));
          d = (standing[b] * (1.0f - w) + followed * w).Normalized();
        }
        body[joints[b + 1]] = body[joints[b]] + d * (lengths[b] * s);
        shown[size_t(joints[b + 1])] = w >= 1.0f && shown[size_t(joints[b])];
      }
      legs_seen = legs_seen && leg_bones_[side * 3 + 1].weight >= 1.0f;
    }
    // Resting on whatever of the body is lowest, less the flesh under that
    // joint, with a little room so the floor and the body stay apart: the
    // feet standing, the knees kneeling, the hands and feet in a push-up, the
    // back lying down.
    static constexpr std::pair<J, float> kContacts[] = {
        {J::kFootLeft, 0.0f},       {J::kFootRight, 0.0f},
        {J::kAnkleLeft, 0.05f},     {J::kAnkleRight, 0.05f},
        {J::kKneeLeft, 0.06f},      {J::kKneeRight, 0.06f},
        {J::kHipCenter, 0.13f},     {J::kHandLeft, 0.03f},
        {J::kHandRight, 0.03f},     {J::kElbowLeft, 0.045f},
        {J::kElbowRight, 0.045f},   {J::kShoulderLeft, 0.07f},
        {J::kShoulderRight, 0.07f}, {J::kHead, 0.1f}};
    float floor = std::numeric_limits<float>::infinity();
    for (const auto& [joint, below] : kContacts) {
      floor = std::min(floor, body[joint].y - below * s);
    }
    const float hip_height = 0.03f - floor;
    // How far the hips rise above where the body's pose puts them, as
    // jumping: the webcam's height for them, at the drawn body's scale, less
    // the pose's. Rising from the floor or a crouch moves both alike.
    const float lift = t[1] * mediapipe_scale - hip_height;
    UpdateRest(t, lift, lean, time);
    // As far from the Kinect as from the webcam, pushed back by however much
    // nearer than the play distance they settled, so moved from there by
    // their steps; sideways as they are; and rising above where the pose
    // rests them (jumping). The webcam finds them at the scale of MediaPipe's
    // body, so their distance is scaled up as it is.
    const Vec3 root(
        (t[0] + hips.x) * mediapipe_scale,
        hip_height + std::max(0.0f, lift - rest_[1]),
        std::clamp(t[2] * mediapipe_scale +
                       std::max(0.0f, float(cvars::kinect_webcam_distance) -
                                          rest_[2] * mediapipe_scale),
                   kMinDistance, kMaxDistance));
    for (Vec3& joint : body.joints) {
      joint = joint + root;
    }
    std::memcpy(placement.t, t, sizeof(t));
    placement.mediapipe_scale = mediapipe_scale;
    placement.anchor = root - hips * mediapipe_scale;
    placement.webcam_focal = focal;
    placement.legs_seen = legs_seen;
    placement.play_distance = root.z;
    return true;
  }

  // How far MediaPipe has the torso lean away from the webcam, from the hips'
  // center to the shoulders', in radians (toward it, negative), or NaN
  // without them.
  static float TorsoLean(const PoseLandmarks& pose) {
    for (int i :
         {L::kShoulderLeft, L::kShoulderRight, L::kHipLeft, L::kHipRight}) {
      if (pose.visibility[i] < kMinVisibility) {
        return std::numeric_limits<float>::quiet_NaN();
      }
    }
    auto torso = [&](int k) {
      return (pose.world[L::kShoulderLeft][k] +
              pose.world[L::kShoulderRight][k] - pose.world[L::kHipLeft][k] -
              pose.world[L::kHipRight][k]) *
             0.5f;
    };
    // MediaPipe's y is down.
    const float up = -torso(1);
    if (up <= 0.0f) {
      return std::numeric_limits<float>::quiet_NaN();
    }
    return std::atan2(torso(2), up);
  }

  // Turns the pose about the webcam's x axis to take out the lean the player
  // settled with (kLeanFollowSeconds).
  void StraightenPose(PoseLandmarks& pose, double time) {
    lean_ += (lean_target_ - lean_) *
             (1.0f - std::exp(-float(std::clamp(time - lean_time_, 0.0, 1.0)) /
                              kLeanFollowSeconds));
    lean_time_ = time;
    const float c = std::cos(lean_);
    const float s = std::sin(lean_);
    for (float* world : pose.world) {
      const float y = world[1];
      const float z = world[2];
      world[1] = y * c - z * s;
      world[2] = y * s + z * c;
    }
  }

  // Where the player rests: their distance from the webcam, how high the
  // webcam sees their hips over where their pose puts them (lift), and the
  // lean MediaPipe gives their torso while they stand still (TorsoLean).
  void UpdateRest(const float t[3], float lift, float lean, double time) {
    const double since_seen = time - last_seen_;
    last_seen_ = time;
    if (t[2] < kAtWebcamDistance) {
      // At the webcam, which may be aimed or placed afresh, so where they
      // stood before means nothing now. Until they're back and settled,
      // they stay at the play distance.
      have_rest_ = false;
      settled_ = false;
      leans_.clear();
      lean_count_ = 0;
      std::memcpy(rest_, t, sizeof(rest_));
      rest_[1] = lift;
      return;
    }
    if (!have_rest_ || since_seen > kForgetSeconds) {
      have_rest_ = true;
      settled_ = false;
      leans_.clear();
      lean_count_ = 0;
      settle_start_ = time;
      settle_.clear();
      std::memcpy(rest_, t, sizeof(rest_));
      rest_[1] = lift;
    } else {
      rest_[1] += (lift - rest_[1]) *
                  (1.0f - std::exp(-float(since_seen) / kHeightFollowSeconds));
    }
    settle_.push_back({time, t[2]});
    while (settle_.front().first < time - kSettleSeconds) {
      settle_.pop_front();
    }
    float low = t[2], high = t[2];
    for (const auto& [when, z] : settle_) {
      low = std::min(low, z);
      high = std::max(high, z);
    }
    const bool still =
        time - settle_start_ >= kSettleSeconds && high - low <= kSettleRange;
    auto median = [](std::vector<float>& values) {
      std::nth_element(values.begin(), values.begin() + values.size() / 2,
                       values.end());
      return values[values.size() / 2];
    };
    // Standing still rather than walking, when they lean as they go: the
    // lean then is mostly how they stand, whatever they hold a moment.
    if (still && !std::isnan(lean)) {
      leans_.push_back(lean);
      if (leans_.size() > kLeanSamples) {
        leans_.pop_front();
      }
      if (++lean_count_ % kLeanSamplesPerUpdate == 0) {
        const bool first = lean_count_ == kLeanSamplesPerUpdate;
        std::vector<float> values(leans_.begin(), leans_.end());
        lean_target_ = median(values);
        if (first && cvars::log_kinect) {
          XELOGI(
              "Kinect: the webcam player's pose is straightened by {:.1f} "
              "degrees",
              -lean_target_ * 180.0f / kPi);
        }
      }
    }
    if (settled_) {
      return;
    }
    // Until they settle, wherever they are is the play distance.
    rest_[2] = t[2];
    if (!still && time - settle_start_ < kMaxSettleSeconds) {
      return;
    }
    std::vector<float> distances;
    for (const auto& [when, z] : settle_) {
      distances.push_back(z);
    }
    rest_[2] = median(distances);
    settled_ = true;
    if (cvars::log_kinect) {
      XELOGI("Kinect: the webcam player settled {:.2f} m from it", rest_[2]);
    }
  }

  // The outline of the body drawn: where the webcam's picture shows each
  // joint, and how much larger it shows the player than the view titles
  // see. False if it doesn't show enough of them.
  bool MakeOutline(const PoseLandmarks& pose, const Body& body,
                   const Placement& placement, double time,
                   BodyRenderer::Outline& outline) {
    using J = BodyJoint;
    // The landmarks smoothed a little while still, or the outline shivers
    // with them.
    float image[L::kCount][2];
    for (int i = 0; i < L::kCount; ++i) {
      for (int k = 0; k < 2; ++k) {
        image[i][k] = image_filters_[i * 2 + k].Filter(pose.image[i][k], time);
      }
    }
    auto& joints = outline.joints;
    auto at = [&](J joint, int i) {
      joints[size_t(joint)] = {image[i][0], image[i][1]};
    };
    auto at_mid = [&](J joint, int a, int b) {
      joints[size_t(joint)] = {(image[a][0] + image[b][0]) * 0.5f,
                               (image[a][1] + image[b][1]) * 0.5f};
    };
    at_mid(J::kHipCenter, L::kHipLeft, L::kHipRight);
    at_mid(J::kShoulderCenter, L::kShoulderLeft, L::kShoulderRight);
    const auto& hip = joints[size_t(J::kHipCenter)];
    const auto& neck = joints[size_t(J::kShoulderCenter)];
    joints[size_t(J::kSpine)] = {hip[0] + (neck[0] - hip[0]) * 0.2f,
                                 hip[1] + (neck[1] - hip[1]) * 0.2f};
    at_mid(J::kHead, L::kEarLeft, L::kEarRight);
    at(J::kShoulderLeft, L::kShoulderLeft);
    at(J::kElbowLeft, L::kElbowLeft);
    at(J::kWristLeft, L::kWristLeft);
    at_mid(J::kHandLeft, L::kPinkyLeft, L::kIndexLeft);
    at(J::kShoulderRight, L::kShoulderRight);
    at(J::kElbowRight, L::kElbowRight);
    at(J::kWristRight, L::kWristRight);
    at_mid(J::kHandRight, L::kPinkyRight, L::kIndexRight);
    // The drawn hip joints, nearer together than MediaPipe's.
    for (auto [joint, i] : {std::pair{J::kHipLeft, int(L::kHipLeft)},
                            std::pair{J::kHipRight, int(L::kHipRight)}}) {
      joints[size_t(joint)] = {
          hip[0] + (image[i][0] - hip[0]) * placement.hip_fraction,
          hip[1] + (image[i][1] - hip[1]) * placement.hip_fraction};
    }
    at(J::kKneeLeft, L::kKneeLeft);
    at(J::kAnkleLeft, L::kAnkleLeft);
    at(J::kFootLeft, L::kFootLeft);
    at(J::kKneeRight, L::kKneeRight);
    at(J::kAnkleRight, L::kAnkleRight);
    at(J::kFootRight, L::kFootRight);
    outline.shown = placement.shown;
    // The scale, by the torso, shoulders and head: as long in the picture as
    // in the view.
    float picture = 0.0f, in_view = 0.0f;
    for (auto [a, b] : {std::pair{J::kHipCenter, J::kShoulderCenter},
                        std::pair{J::kShoulderLeft, J::kShoulderRight},
                        std::pair{J::kShoulderCenter, J::kHead}}) {
      if (!outline.shown[size_t(a)] || !outline.shown[size_t(b)]) {
        continue;
      }
      picture += std::hypot(joints[size_t(b)][0] - joints[size_t(a)][0],
                            joints[size_t(b)][1] - joints[size_t(a)][1]);
      float ax, ay, bx, by;
      BodyRenderer::Project(renderer_.ToCamera(body[a]), ax, ay);
      BodyRenderer::Project(renderer_.ToCamera(body[b]), bx, by);
      in_view += std::hypot(bx - ax, by - ay);
    }
    if (in_view < 20.0f || picture < 10.0f) {
      return false;
    }
    outline.scale = outline_scale_filter_.Filter(picture / in_view, time);
    return true;
  }

  // The webcam's picture as the Kinect's color, registered to the depth: the
  // player's hips where the drawn body's are, at its scale. Bayer GRBG. With
  // an outline, the body's pixels come from where it found them, and the
  // room around it, where the picture now shows the player, from where it
  // last showed the room.
  void MakeColor(const RgbFrame& frame, const Body* body,
                 const Placement& placement,
                 const BodyRenderer::Outline* outline, const PoseMask* mask,
                 uint8_t* out) {
    const float focal =
        body ? placement.webcam_focal : WebcamFocal(frame.width, frame.height);
    float pv_x = BodyRenderer::kCenterX, pv_y = BodyRenderer::kCenterY;
    float pw_x = frame.width * 0.5f, pw_y = frame.height * 0.5f;
    float scale = BodyRenderer::kFocalLength / focal;
    if (body) {
      const Vec3 hip = renderer_.ToCamera(placement.anchor);
      pv_x =
          BodyRenderer::kCenterX + BodyRenderer::kFocalLength * hip.x / hip.z;
      pv_y =
          BodyRenderer::kCenterY - BodyRenderer::kFocalLength * hip.y / hip.z;
      const float* t = placement.t;
      pw_x = frame.width * 0.5f + focal * t[0] / t[2];
      pw_y = frame.height * 0.5f - focal * t[1] / t[2];
      scale = (BodyRenderer::kFocalLength / hip.z) / (focal / t[2]);
    }
    const float* source = nullptr;
    if (body && outline && mask && renderer_.outlined()) {
      scale = 1.0f / outline->scale;
      source = renderer_.source().data();
      UpdateBackground(frame, *mask);
    }
    const bool mirror = cvars::kinect_mirror;
    for (int y = 0; y < kHeight; ++y) {
      const float sy = (y - pv_y) / scale + pw_y;
      for (int x = 0; x < kWidth; ++x) {
        const int ox = mirror ? kWidth - 1 - x : x;
        // GRBG: green, red on even rows; blue, green on odd ones.
        const int channel = (y & 1) ? ((x & 1) ? 1 : 2) : ((x & 1) ? 0 : 1);
        float sx = (x - pv_x) / scale + pw_x;
        float ty = sy;
        const RgbFrame* from = &frame;
        if (source) {
          const float* body_source = &source[(size_t(y) * kWidth + x) * 2];
          if (!std::isnan(body_source[0])) {
            sx = body_source[0];
            ty = body_source[1];
          } else if (ShowsPlayer(sx, sy)) {
            from = &background_;
          }
        }
        out[y * kWidth + ox] = Sample(*from, sx, ty, channel);
      }
    }
  }

  // The room as the webcam has seen it around the player lately, and where
  // it shows them now.
  void UpdateBackground(const RgbFrame& frame, const PoseMask& mask) {
    if (background_.width != frame.width ||
        background_.height != frame.height) {
      background_ = frame;
    }
    player_.resize(size_t(frame.width) * frame.height);
    float bx0, by0, bx1, by1;
    mask.Bounds(bx0, by0, bx1, by1);
    const int x0 = std::clamp(int(bx0), 0, frame.width);
    const int x1 = std::clamp(int(bx1) + 1, 0, frame.width);
    const int y0 = std::clamp(int(by0), 0, frame.height);
    const int y1 = std::clamp(int(by1) + 1, 0, frame.height);
    for (int y = 0; y < frame.height; ++y) {
      const size_t row = size_t(y) * frame.width;
      uint8_t* player = &player_[row];
      const uint8_t* in = &frame.pixels[row * 3];
      uint8_t* room = &background_.pixels[row * 3];
      if (y < y0 || y >= y1) {
        std::fill_n(player, frame.width, uint8_t(0));
        std::memcpy(room, in, size_t(frame.width) * 3);
        continue;
      }
      for (int x = 0; x < frame.width; ++x) {
        const float m = x >= x0 && x < x1 ? mask.At(float(x), float(y)) : 0.0f;
        player[x] = m >= 0.5f;
        // Clear of the player's edge, where the mask is unsure, and a part
        // at a time, so a blurred limb the mask missed fades out.
        if (m < 0.25f) {
          for (int c = 0; c < 3; ++c) {
            uint8_t& r = room[x * 3 + c];
            r = uint8_t(r + ((int(in[x * 3 + c]) - r) * 77 >> 8));
          }
        }
      }
    }
  }

  bool ShowsPlayer(float x, float y) const {
    const int xi = int(x + 0.5f);
    const int yi = int(y + 0.5f);
    return xi >= 0 && yi >= 0 && xi < background_.width &&
           yi < background_.height &&
           player_[size_t(yi) * background_.width + xi];
  }

  static uint8_t Sample(const RgbFrame& frame, float x, float y, int channel) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int x0 = int(fx), y0 = int(fy);
    if (x0 < 0 || y0 < 0 || x0 + 1 >= frame.width || y0 + 1 >= frame.height) {
      return 0;
    }
    const float wx = x - fx, wy = y - fy;
    const size_t row = size_t(frame.width) * 3;
    const uint8_t* p = &frame.pixels[y0 * row + x0 * 3 + channel];
    const float top = p[0] + (p[3] - p[0]) * wx;
    const float bottom = p[row] + (p[row + 3] - p[row]) * wx;
    return uint8_t(top + (bottom - top) * wy + 0.5f);
  }

  std::unique_ptr<FrameSource> source_;
  // Records the webcam's pictures (kinect_record_path), and the buffer for
  // one.
  KinectRecorder* recorder_ = nullptr;
  std::vector<uint8_t> picture_;
  // RotateFrame's buffer for the picture turned.
  RgbFrame turned_;
  // webcam_choice().generation when source_ was opened.
  uint32_t webcam_generation_ = 0;
  std::unique_ptr<PoseEstimator> pose_estimator_;
  BodyRenderer renderer_;
  FrameCallback depth_callback_;
  FrameCallback color_callback_;
  std::atomic<bool> running_ = false;
  std::thread thread_;
  std::vector<OneEuroFilter> landmark_filters_ =
      std::vector<OneEuroFilter>(L::kCount * 3, OneEuroFilter(1.5f, 5.0f));
  // Smoother while still than the landmarks: the hips' distance from the
  // webcam, found from how large the player looks, wavers by a centimeter or
  // two.
  std::vector<OneEuroFilter> position_filters_ =
      std::vector<OneEuroFilter>(3, OneEuroFilter(0.4f, 4.0f));
  // Left then right: thigh, shin and foot.
  std::array<LegBone, 6> leg_bones_;
  // The outline's scale, picture pixels per view pixel.
  OneEuroFilter outline_scale_filter_ = OneEuroFilter(1.0f, 0.5f);
  // The landmarks in the picture, for the outline: pixels, so a beta for
  // speeds about 200 times the meters a second of landmark_filters_'.
  std::vector<OneEuroFilter> image_filters_ =
      std::vector<OneEuroFilter>(L::kCount * 2, OneEuroFilter(1.5f, 0.025f));
  // PlaceBody's: how much larger the drawn body is than MediaPipe's.
  float mediapipe_scale_ = 0.0f;
  double scale_time_ = 0.0;
  // UpdateBackground's.
  RgbFrame background_;
  std::vector<uint8_t> player_;
  // Where the player stands at rest (UpdateRest).
  bool have_rest_ = false;
  bool settled_ = false;
  double settle_start_ = 0.0;
  std::deque<std::pair<double, float>> settle_;
  // UpdateRest's: TorsoLean's while they stood still since they settled,
  // and what StraightenPose takes out, easing toward their median.
  std::deque<float> leans_;
  size_t lean_count_ = 0;
  float lean_ = 0.0f;
  float lean_target_ = 0.0f;
  double lean_time_ = 0.0;
  float rest_[3] = {};
  double last_seen_ = 0.0;
  // For the Kinect view.
  double view_time_ = 0.0;
  float view_fps_ = 0.0f;
  float view_pose_ms_ = 0.0f;
};

}  // namespace

std::unique_ptr<KinectCamera> OpenWebcamKinectCamera(
    const std::filesystem::path& storage_root) {
  auto camera = std::make_unique<KinectCameraWebcam>();
  if (!camera->Initialize(storage_root)) {
    return nullptr;
  }
  return camera;
}

std::vector<std::string> ListWebcams() {
  std::vector<std::string> names;
  // Started once and kept: SDL counts its subsystems' users without a lock,
  // and a streaming webcam's thread starts and quits the camera subsystem
  // too, so this doesn't add to or take from the count again.
  static const bool camera_subsystem = xe::helper::sdl::SDLHelper::Prepare() &&
                                       SDL_InitSubSystem(SDL_INIT_CAMERA);
  if (!camera_subsystem) {
    return names;
  }
  int count = 0;
  SDL_CameraID* ids = SDL_GetCameras(&count);
  for (int i = 0; i < count; ++i) {
    const char* name = SDL_GetCameraName(ids[i]);
    names.push_back(name ? name : "camera");
  }
  SDL_free(ids);
  return names;
}

std::string ChosenWebcam() {
  WebcamChoice& choice = webcam_choice();
  std::lock_guard<std::mutex> lock(choice.mutex);
  return choice.chosen ? choice.part_of_name : cvars::kinect_webcam;
}

void ChooseWebcam(const std::string& part_of_name) {
  WebcamChoice& choice = webcam_choice();
  std::lock_guard<std::mutex> lock(choice.mutex);
  choice.chosen = true;
  choice.part_of_name = part_of_name;
  // Under the lock, as ChosenWebcam reads kinect_webcam until now.
  OVERRIDE_PERSIST_string(kinect_webcam, part_of_name);
  ++choice.generation;
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe

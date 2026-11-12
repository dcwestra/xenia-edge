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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
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
DEFINE_double(kinect_webcam_distance, 2.6,
              "How far from the Kinect a webcam player stands, in meters, "
              "however far they are from the webcam: stepping toward or away "
              "from it moves them from there. Titles aim the Kinect for "
              "players about this far: nearer, Kinect Adventures has their "
              "feet out of view.",
              "Kernel");
DEFINE_double(kinect_webcam_player_height, 1.70,
              "The webcam player's height in meters, for the body Kinect "
              "titles see.",
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
constexpr float kMinVisibility = 0.5f;
// The drawn body's bones, in meters for a player 1.70 m tall, scaled to
// kinect_webcam_player_height: the proportions of the bodies titles' trackers
// were tested with (tools' kbodyrender.py). The hips are 0.49 m below the
// shoulders' center.
constexpr float kHeadLength = 0.22f;  // Shoulders' center to the head's.
constexpr float kShoulderHalfWidth = 0.19f;
constexpr float kUpperArmLength = 0.35f;
constexpr float kForearmLength = 0.27f;
constexpr float kHandLength = 0.08f;  // Wrist to the hand's center.
constexpr float kHipHalfWidth = 0.09f;
constexpr float kThighLength = 0.42f;
constexpr float kShinLength = 0.41f;
constexpr float kFootLength = 0.14f;  // Ankle to toes.
// The players' range in front of the virtual Kinect.
constexpr float kMinDistance = 1.5f;
constexpr float kMaxDistance = 3.2f;
constexpr int kPoseThreads = 2;

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
  KeepFrameRate keep_frame_rate_;
};

// A Kinect recording's color frames, demosaiced, at their own pace, looping.
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
      if ((header.stream != 1 && header.stream != 2) ||
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

 private:
  FILE* file_ = nullptr;
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
      renderer_.Render(&body, placed ? 1 : 0,
                       float(cvars::kinect_webcam_player_height), raw.data());
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
        MakeColor(frame, placed ? &body : nullptr, placement, color.data());
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
      webcam.distance = placement->t[2];
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

  // The webcam's focal length in pixels, for its frames this wide.
  float WebcamFocal(int width) const {
    return width * 0.5f /
           (std::tan(float(cvars::kinect_webcam_fov) * kPi / 360.0f) *
            source_->field_fraction());
  }

  bool PlaceBody(const PoseLandmarks& raw_pose, int width, int height,
                 double time, Body& body, Placement& placement) {
    const float focal = WebcamFocal(width);
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
    UpdateRest(t, time);
    // In the room's axes (y up), from MediaPipe's origin between the hips.
    auto world = [&](int i) {
      return Vec3(pose.world[i][0], -pose.world[i][1], pose.world[i][2]);
    };
    auto mid = [&](int a, int b) { return (world(a) + world(b)) * 0.5f; };
    using J = BodyJoint;
    const float s = float(cvars::kinect_webcam_player_height) / 1.70f;
    const Vec3 shoulders = mid(L::kShoulderLeft, L::kShoulderRight);
    Vec3 hips = mid(L::kHipLeft, L::kHipRight);
    Vec3 hip_left = world(L::kHipLeft);
    Vec3 hip_right = world(L::kHipRight);
    if (std::min(pose.visibility[L::kHipLeft], pose.visibility[L::kHipRight]) <
        kMinVisibility) {
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
    auto bone = [&](const Vec3& from, const Vec3& to, float length) {
      return (to - from).Normalized() * (length * s);
    };
    // Perpendicular to the torso, toward the player's left.
    auto across = [&](const Vec3& left_side, const Vec3& up) {
      return (left_side - up * left_side.Dot(up)).Normalized();
    };
    const Vec3 up = (shoulders - hips).Normalized();
    const Vec3 shoulder_side =
        across(world(L::kShoulderLeft) - world(L::kShoulderRight), up);
    const Vec3 hip_side = across(hip_left - hip_right, up);
    body[J::kHipCenter] = Vec3();
    body[J::kShoulderCenter] = up * (0.49f * s);
    body[J::kSpine] = body[J::kShoulderCenter] * 0.2f;
    body[J::kHead] =
        body[J::kShoulderCenter] +
        bone(shoulders, mid(L::kEarLeft, L::kEarRight), kHeadLength);
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
    }
    body[J::kHipLeft] = hip_side * (kHipHalfWidth * s) - up * (0.05f * s);
    body[J::kHipRight] = hip_side * (-kHipHalfWidth * s) - up * (0.05f * s);
    const bool legs_seen =
        std::min({pose.visibility[L::kKneeLeft], pose.visibility[L::kKneeRight],
                  pose.visibility[L::kAnkleLeft],
                  pose.visibility[L::kAnkleRight]}) > 0.6f;
    for (auto [hip, knee, ankle, foot, mp_hip, mp_knee, mp_ankle, mp_foot] :
         {std::tuple{J::kHipLeft, J::kKneeLeft, J::kAnkleLeft, J::kFootLeft,
                     L::kHipLeft, L::kKneeLeft, L::kAnkleLeft, L::kFootLeft},
          std::tuple{J::kHipRight, J::kKneeRight, J::kAnkleRight, J::kFootRight,
                     L::kHipRight, L::kKneeRight, L::kAnkleRight,
                     L::kFootRight}}) {
      if (legs_seen) {
        body[knee] =
            body[hip] + bone(world(mp_hip), world(mp_knee), kThighLength);
        body[ankle] =
            body[knee] + bone(world(mp_knee), world(mp_ankle), kShinLength);
        body[foot] =
            body[ankle] + bone(world(mp_ankle), world(mp_foot), kFootLength);
      } else {
        // Out of the webcam's view, as they are for a seated player: standing
        // legs, which titles' trackers want.
        body[knee] = body[hip] + Vec3(0.0f, -kThighLength * s, -0.02f);
        body[ankle] = body[knee] + Vec3(0.0f, -kShinLength * s, 0.02f);
        body[foot] = body[ankle] + Vec3(0.0f, -0.05f * s, -0.13f * s);
      }
    }
    // Standing on the lower of the feet, the heel just under the ankle, with a
    // little room so the floor and the soles stay apart.
    float floor = 0.0f;
    for (J joint : {J::kFootLeft, J::kFootRight}) {
      floor = std::min(floor, body[joint].y);
    }
    for (J joint : {J::kAnkleLeft, J::kAnkleRight}) {
      floor = std::min(floor, body[joint].y - 0.05f * s);
    }
    const float hip_height = 0.03f - floor;
    // At the play distance, moved by their steps toward the webcam or away
    // from where they settled; sideways as they are; and rising above where
    // they stand (jumping). Crouches come through the legs.
    const Vec3 root(
        t[0] + hips.x, hip_height + std::max(0.0f, t[1] - rest_[1]),
        std::clamp(float(cvars::kinect_webcam_distance) + (t[2] - rest_[2]),
                   kMinDistance, kMaxDistance));
    for (Vec3& joint : body.joints) {
      joint = joint + root;
    }
    std::memcpy(placement.t, t, sizeof(t));
    placement.anchor = root - hips;
    placement.webcam_focal = focal;
    placement.legs_seen = legs_seen;
    placement.play_distance = root.z;
    return true;
  }

  void UpdateRest(const float t[3], double time) {
    const double since_seen = time - last_seen_;
    last_seen_ = time;
    if (t[2] < kAtWebcamDistance) {
      // At the webcam, which may be aimed or placed afresh, so where they
      // stood before means nothing now. Until they're back and settled,
      // they stay at the play distance.
      have_rest_ = false;
      settled_ = false;
      std::memcpy(rest_, t, sizeof(rest_));
      return;
    }
    if (!have_rest_ || since_seen > kForgetSeconds) {
      have_rest_ = true;
      settled_ = false;
      settle_start_ = time;
      settle_.clear();
      std::memcpy(rest_, t, sizeof(rest_));
    } else {
      rest_[1] += (t[1] - rest_[1]) *
                  (1.0f - std::exp(-float(since_seen) / kHeightFollowSeconds));
    }
    if (settled_) {
      return;
    }
    // Until they settle, wherever they are is the play distance.
    rest_[2] = t[2];
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
    if (!still && time - settle_start_ < kMaxSettleSeconds) {
      return;
    }
    std::vector<float> values;
    for (const auto& [when, z] : settle_) {
      values.push_back(z);
    }
    std::nth_element(values.begin(), values.begin() + values.size() / 2,
                     values.end());
    rest_[2] = values[values.size() / 2];
    settled_ = true;
    if (cvars::log_kinect) {
      XELOGI("Kinect: the webcam player settled {:.2f} m from it", rest_[2]);
    }
  }

  // The webcam's picture as the Kinect's color, registered to the depth: the
  // player's hips where the drawn body's are, at its scale. Bayer GRBG.
  void MakeColor(const RgbFrame& frame, const Body* body,
                 const Placement& placement, uint8_t* out) {
    const float focal =
        body ? placement.webcam_focal : WebcamFocal(frame.width);
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
    const bool mirror = cvars::kinect_mirror;
    for (int y = 0; y < kHeight; ++y) {
      const float sy = (y - pv_y) / scale + pw_y;
      for (int x = 0; x < kWidth; ++x) {
        const int ox = mirror ? kWidth - 1 - x : x;
        // GRBG: green, red on even rows; blue, green on odd ones.
        const int channel = (y & 1) ? ((x & 1) ? 1 : 2) : ((x & 1) ? 0 : 1);
        const float sx = (x - pv_x) / scale + pw_x;
        out[y * kWidth + ox] = Sample(frame, sx, sy, channel);
      }
    }
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
  std::vector<OneEuroFilter> position_filters_ =
      std::vector<OneEuroFilter>(3, OneEuroFilter(1.0f, 2.0f));
  // Where the player stands at rest (UpdateRest).
  bool have_rest_ = false;
  bool settled_ = false;
  double settle_start_ = 0.0;
  std::deque<std::pair<double, float>> settle_;
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

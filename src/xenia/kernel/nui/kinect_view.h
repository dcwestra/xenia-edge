/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_KINECT_VIEW_H_
#define XENIA_KERNEL_NUI_KINECT_VIEW_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace kernel {
namespace nui {

// What the Kinect view shows of the camera and the player, which the camera
// code publishes while the view is shown: the depth and color titles get, the
// players titles track, and with a webcam standing in for a Kinect, its
// picture and what was found in it.
class KinectView {
 public:
  static constexpr int kLandmarks = 33;
  static constexpr int kJoints = 20;

  // A player a title tracks: the NUI skeleton's joints, in meters from the
  // sensor, and their states (0 not tracked, 1 inferred, 2 tracked).
  struct Skeleton {
    float joints[kJoints][3];
    uint32_t states[kJoints];
  };

  // Whether the view shows the camera while no title uses it
  // (StartKinectPreview).
  enum class Preview { kOff, kShowing, kNoCamera };

  // With a webcam: what was found in its picture.
  struct Webcam {
    std::string name;
    float frames_per_second = 0.0f;
    float pose_ms = 0.0f;
    bool seen = false;
    // Whether where the player stands at rest has been taken.
    bool settled = false;
    bool legs_seen = false;
    // Meters: the player from the webcam, and from the Kinect in the room
    // titles see.
    float distance = 0.0f;
    float play_distance = 0.0f;
    float tilt_degrees = 0.0f;
    // MediaPipe's pose landmarks in the picture, 0 to 1, and how visible.
    float landmarks[kLandmarks][2] = {};
    float visibility[kLandmarks] = {};
  };

  // A picture as published: RGBA, and a number that changes with it.
  struct Picture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    uint32_t generation = 0;
  };
  // A copy of what's published, for drawing it.
  struct Snapshot {
    Picture depth;
    Picture color;
    Picture webcam;
    Webcam webcam_status;
    // The players the title tracks, if it sent them recently.
    std::vector<Skeleton> skeletons;
    Preview preview = Preview::kOff;
  };

  static KinectView& Get();

  // How to stand or aim the webcam, from where the player is in its picture.
  // waiting_for_title: a title is running and hasn't found them.
  static std::vector<const char*> Advice(const Webcam& webcam,
                                         bool waiting_for_title);

  // Things are published only while something shows them.
  void AddViewer() { ++viewers_; }
  void RemoveViewer() { --viewers_; }
  bool shown() const { return viewers_ > 0; }
  Snapshot Take();

  // The depth titles get: the sensor's 11-bit packing of 640x480 raw values.
  void PublishPackedDepth(const uint8_t* packed);
  // The color titles get: 640x480 Bayer GRBG.
  void PublishColor(const uint8_t* bayer);
  void PublishWebcam(const uint8_t* rgb, int width, int height,
                     const Webcam& webcam);
  // The players a title tracks, from its skeleton frames.
  void PublishSkeletons(const std::vector<Skeleton>& skeletons);
  void SetPreview(Preview preview) { preview_ = preview; }
  // Forgets what was published, as another camera starts.
  void Clear();

 private:
  friend class KinectViewOverlay;
  using Clock = std::chrono::steady_clock;
  // Pictures are published at most this often, each a new texture.
  static constexpr auto kPublishInterval = std::chrono::milliseconds(66);

  // Whether a picture is due, marking it published.
  bool Due(Clock::time_point& last);

  std::atomic<int> viewers_ = 0;
  std::atomic<Preview> preview_ = Preview::kOff;
  std::mutex mutex_;
  Picture depth_;
  Picture color_;
  Picture webcam_;
  Webcam webcam_status_;
  std::vector<Skeleton> skeletons_;
  Clock::time_point depth_time_, color_time_, webcam_time_, skeletons_time_;
};

// The Kinect view in a game: an overlay in its corner showing what the game
// sees and, with a webcam, how to stand or aim it so the player is seen well.
// Only one at a time.
class KinectViewOverlay : public ui::ImGuiOverlay {
 public:
  explicit KinectViewOverlay(ui::ImGuiDrawer* imgui_drawer);
  ~KinectViewOverlay() override;
  void Draw() override;

 private:
  struct Texture {
    std::unique_ptr<ui::ImmediateTexture> texture;
    uint32_t generation = 0;
    int width = 0;
    int height = 0;
  };
  void Update(Texture& texture, const KinectView::Picture& picture);

  ui::ImGuiDrawer* imgui_drawer_;
  Texture depth_;
  Texture color_;
  Texture webcam_;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_KINECT_VIEW_H_

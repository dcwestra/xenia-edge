/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/kinect_view.h"

#include <algorithm>
#include <cmath>

#include "third_party/imgui/imgui.h"
#include "xenia/base/cvar.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui/kinect_camera.h"
#include "xenia/kernel/nui/nui_hud.h"

DEFINE_bool(kinect_view, false,
            "Show the Kinect view from the start (F9 shows and hides it): "
            "what Kinect titles see, and with a webcam standing in for a "
            "Kinect, how to stand or aim it.",
            "Kernel");

namespace xe {
namespace kernel {
namespace nui {

namespace {
constexpr int kSourceWidth = 640;
constexpr int kSourceHeight = 480;
constexpr int kPreviewWidth = 320;
constexpr int kPreviewHeight = 240;
// The sensor's nominal depth view, for drawing titles' skeletons on it.
constexpr float kFocalLength = 571.26f;

// The NUI skeleton's bones, by joint index.
constexpr int kSkeletonBones[][2] = {
    {0, 1},   {1, 2},  {2, 3},   {2, 4},   {4, 5},  {5, 6},   {6, 7},
    {2, 8},   {8, 9},  {9, 10},  {10, 11}, {0, 12}, {12, 13}, {13, 14},
    {14, 15}, {0, 16}, {16, 17}, {17, 18}, {18, 19}};
// MediaPipe's pose bones, by landmark.
constexpr int kPoseBones[][2] = {{11, 12}, {11, 13}, {13, 15}, {12, 14},
                                 {14, 16}, {11, 23}, {12, 24}, {23, 24},
                                 {23, 25}, {25, 27}, {27, 31}, {24, 26},
                                 {26, 28}, {28, 32}, {7, 0},   {0, 8}};
constexpr float kVisible = 0.5f;

const ImU32 kSkeletonColor = IM_COL32(80, 255, 120, 255);
const ImU32 kPoseColor = IM_COL32(80, 200, 255, 255);
const ImVec4 kWarningColor = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
const ImVec4 kGoodColor = ImVec4(0.5f, 1.0f, 0.6f, 1.0f);
}  // namespace

KinectView& KinectView::Get() {
  static KinectView view;
  return view;
}

bool KinectView::Due(Clock::time_point& last) {
  const auto now = Clock::now();
  if (now - last < kPublishInterval) {
    return false;
  }
  last = now;
  return true;
}

void KinectView::PublishPackedDepth(const uint8_t* packed) {
  if (!shown_ || !Due(depth_time_)) {
    return;
  }
  std::vector<uint16_t> raw(size_t(kSourceWidth) * kSourceHeight);
  UnpackKinectDepth(packed, raw.data());
  std::lock_guard<std::mutex> lock(mutex_);
  depth_.width = kPreviewWidth;
  depth_.height = kPreviewHeight;
  depth_.rgba.resize(size_t(kPreviewWidth) * kPreviewHeight * 4);
  uint8_t* out = depth_.rgba.data();
  for (int y = 0; y < kPreviewHeight; ++y) {
    const uint16_t* row = &raw[size_t(y) * 2 * kSourceWidth];
    for (int x = 0; x < kPreviewWidth; ++x, out += 4) {
      // Nearer brighter; no reading dark blue.
      const uint16_t value = row[x * 2];
      if (value >= 1084) {
        out[0] = 10;
        out[1] = 14;
        out[2] = 40;
      } else {
        const uint8_t gray =
            uint8_t(std::clamp(255 - (int(value) - 400) * 220 / 684, 30, 255));
        out[0] = out[1] = out[2] = gray;
      }
      out[3] = 255;
    }
  }
  ++depth_.generation;
}

void KinectView::PublishColor(const uint8_t* bayer) {
  if (!shown_ || !Due(color_time_)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  color_.width = kPreviewWidth;
  color_.height = kPreviewHeight;
  color_.rgba.resize(size_t(kPreviewWidth) * kPreviewHeight * 4);
  uint8_t* out = color_.rgba.data();
  // Each GRBG cell as one pixel.
  for (int y = 0; y < kPreviewHeight; ++y) {
    const uint8_t* top = bayer + size_t(y) * 2 * kSourceWidth;
    const uint8_t* bottom = top + kSourceWidth;
    for (int x = 0; x < kPreviewWidth; ++x, out += 4) {
      out[0] = top[x * 2 + 1];
      out[1] = uint8_t((top[x * 2] + bottom[x * 2 + 1]) / 2);
      out[2] = bottom[x * 2];
      out[3] = 255;
    }
  }
  ++color_.generation;
}

void KinectView::PublishWebcam(const uint8_t* rgb, int width, int height,
                               const Webcam& webcam) {
  if (!shown_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  webcam_status_ = webcam;
  if (!Due(webcam_time_) || width <= 0 || height <= 0) {
    return;
  }
  webcam_.width = kPreviewWidth;
  webcam_.height = std::max(1, kPreviewWidth * height / width);
  webcam_.rgba.resize(size_t(webcam_.width) * webcam_.height * 4);
  uint8_t* out = webcam_.rgba.data();
  for (int y = 0; y < webcam_.height; ++y) {
    const int sy = y * height / webcam_.height;
    for (int x = 0; x < webcam_.width; ++x, out += 4) {
      const uint8_t* p =
          rgb + (size_t(sy) * width + x * width / webcam_.width) * 3;
      out[0] = p[0];
      out[1] = p[1];
      out[2] = p[2];
      out[3] = 255;
    }
  }
  ++webcam_.generation;
}

KinectViewOverlay::KinectViewOverlay(ui::ImGuiDrawer* imgui_drawer,
                                     KernelState* kernel_state)
    : imgui_drawer_(imgui_drawer), kernel_state_(kernel_state) {
  KinectView::Get().shown_ = true;
  imgui_drawer_->AddOverlay(this);
}

KinectViewOverlay::~KinectViewOverlay() {
  KinectView::Get().shown_ = false;
  imgui_drawer_->RemoveOverlay(this);
}

void KinectViewOverlay::Update(Texture& texture,
                               const KinectView::Picture& picture) {
  if (picture.generation == texture.generation || picture.rgba.empty()) {
    return;
  }
  texture.texture = imgui_drawer_->CreateTexture(
      uint32_t(picture.width), uint32_t(picture.height), picture.rgba.data());
  texture.generation = picture.generation;
  texture.width = picture.width;
  texture.height = picture.height;
}

void KinectViewOverlay::Draw() {
  KinectView& view = KinectView::Get();
  KinectView::Webcam webcam;
  bool have_webcam;
  {
    std::lock_guard<std::mutex> lock(view.mutex_);
    Update(depth_, view.depth_);
    Update(color_, view.color_);
    Update(webcam_, view.webcam_);
    webcam = view.webcam_status_;
    have_webcam = view.webcam_.generation != 0;
  }
  std::vector<NuiHud::TrackedSkeleton> skeletons;
  if (kernel_state_ && kernel_state_->nui_hud()) {
    skeletons = kernel_state_->nui_hud()->GetTrackedSkeletons();
  }

  const ImGuiIO& io = ImGui::GetIO();
  const float scale = std::clamp(io.DisplaySize.x / 1920.0f, 0.6f, 1.5f);
  const ImVec2 picture_size(kPreviewWidth * scale, kPreviewHeight * scale);
  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.7f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (!ImGui::Begin("##kinect_view", nullptr, flags)) {
    ImGui::End();
    return;
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // What the game sees, with the players it tracks.
  ImGui::BeginGroup();
  ImGui::TextUnformatted("What the game sees");
  const ImVec2 depth_origin = ImGui::GetCursorScreenPos();
  if (depth_.texture) {
    ImGui::Image(reinterpret_cast<ImTextureID>(depth_.texture.get()),
                 picture_size);
    const float sx = picture_size.x / kSourceWidth;
    const float sy = picture_size.y / kSourceHeight;
    for (const NuiHud::TrackedSkeleton& skeleton : skeletons) {
      auto at = [&](int joint) {
        const float* p = skeleton.joints[joint];
        const float z = std::max(p[2], 0.1f);
        // Skeleton x is mirrored against the sensor's picture.
        return ImVec2(
            depth_origin.x +
                (kSourceWidth * 0.5f - kFocalLength * p[0] / z) * sx,
            depth_origin.y +
                (kSourceHeight * 0.5f - kFocalLength * p[1] / z) * sy);
      };
      for (const auto& bone : kSkeletonBones) {
        if (skeleton.states[bone[0]] && skeleton.states[bone[1]]) {
          draw->AddLine(at(bone[0]), at(bone[1]), kSkeletonColor, 2.0f * scale);
        }
      }
    }
  } else {
    ImGui::Dummy(picture_size);
  }
  ImGui::EndGroup();

  // The webcam's picture with the pose found in it, else the Kinect's color.
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (have_webcam && webcam_.texture) {
    ImGui::TextUnformatted("What the webcam sees");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(picture_size.x,
                      picture_size.x * webcam_.height / webcam_.width);
    ImGui::Image(reinterpret_cast<ImTextureID>(webcam_.texture.get()), size);
    if (webcam.seen) {
      auto at = [&](int i) {
        return ImVec2(origin.x + webcam.landmarks[i][0] * size.x,
                      origin.y + webcam.landmarks[i][1] * size.y);
      };
      for (const auto& bone : kPoseBones) {
        if (webcam.visibility[bone[0]] > kVisible &&
            webcam.visibility[bone[1]] > kVisible) {
          draw->AddLine(at(bone[0]), at(bone[1]), kPoseColor, 2.0f * scale);
        }
      }
    }
  } else if (color_.texture) {
    ImGui::TextUnformatted("What the Kinect's camera sees");
    ImGui::Image(reinterpret_cast<ImTextureID>(color_.texture.get()),
                 picture_size);
  }
  ImGui::EndGroup();

  if (skeletons.empty()) {
    ImGui::TextUnformatted("Game: not tracking anyone");
  } else {
    ImGui::TextColored(kGoodColor, "Game: tracking %d player%s",
                       int(skeletons.size()), skeletons.size() > 1 ? "s" : "");
  }
  if (have_webcam) {
    ImGui::Text("Webcam: %s, %.0f frames a second, pose %.0f ms",
                webcam.name.c_str(), webcam.frames_per_second, webcam.pose_ms);
    if (webcam.seen) {
      ImGui::Text("You: %.2f m from the webcam, %.2f m from the Kinect%s",
                  webcam.distance, webcam.play_distance,
                  webcam.settled ? "" : " (hold still a moment)");
    }
    ImGui::Text("Kinect tilt: %+.0f degrees (simulated)", webcam.tilt_degrees);

    // How to stand or aim the webcam, from where the player is in its
    // picture.
    std::vector<const char*> advice;
    if (!webcam.seen) {
      advice.push_back("Step into the webcam's view.");
    } else {
      const float* l = &webcam.landmarks[0][0];
      auto x = [&](int i) { return l[i * 2]; };
      auto y = [&](int i) { return l[i * 2 + 1]; };
      const float shoulders_x = (x(11) + x(12)) * 0.5f;
      const float shoulders_y = (y(11) + y(12)) * 0.5f;
      const float shoulder_width = std::abs(x(11) - x(12));
      // The top of the head: above the nose by about 0.6 of the nose's
      // height above the shoulders.
      const float crown = y(0) - (shoulders_y - y(0)) * 0.6f;
      if (shoulder_width > 0.5f) {
        advice.push_back("Too close to the webcam: step back.");
      } else if (crown < 0.02f) {
        advice.push_back(
            "Your head is out of view: tilt the webcam up, or step back.");
      } else if (shoulder_width < 0.08f) {
        advice.push_back(
            "Far from the webcam: step closer for steadier tracking.");
      }
      if (shoulders_x < 0.15f || shoulders_x > 0.85f) {
        advice.push_back("Step toward the middle of the webcam's view.");
      }
      if (!webcam.legs_seen) {
        advice.push_back(
            "Your legs are out of view, so standing legs are drawn for you. "
            "To use your own, tilt the webcam down or step back.");
      }
      if (webcam.settled && skeletons.empty()) {
        advice.push_back("The game hasn't found you yet: move a little.");
      }
    }
    for (const char* line : advice) {
      ImGui::TextColored(kWarningColor, "%s", line);
    }
  }
  ImGui::TextDisabled("F9 hides this");
  ImGui::End();
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe

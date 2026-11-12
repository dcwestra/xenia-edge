/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_WEBCAM_KINECT_H_
#define XENIA_KERNEL_NUI_WEBCAM_KINECT_H_

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace nui {

class KinectCamera;

// A webcam standing in for a Kinect for Xbox 360, for players without one:
// the player's pose, found in the webcam's picture, is drawn as the depth a
// Kinect would see of them, and titles' own skeletal trackers take it from
// there. Color is the webcam's picture, lined up with the drawn body. Null,
// having logged why, without a webcam, ONNX Runtime or the pose models
// (kinect_webcam_models_path, by default the kinect folder in storage_root).
std::unique_ptr<KinectCamera> OpenWebcamKinectCamera(
    const std::filesystem::path& storage_root);

// The names of the webcams there are, for choosing one.
std::vector<std::string> ListWebcams();
// The webcam chosen to stand in for a Kinect, by part of its name: empty for
// the first there is. kinect_webcam until ChooseWebcam is called.
std::string ChosenWebcam();
// Chooses the webcam that stands in for a Kinect, switching to it at once if
// one is streaming, and sets kinect_webcam to it for config::SaveConfig.
void ChooseWebcam(const std::string& part_of_name);

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_WEBCAM_KINECT_H_

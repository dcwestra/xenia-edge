#!/bin/sh
# get_webcam_files.sh [XENIA_STORAGE_FOLDER]: sets up what a webcam needs to
# stand in for a Kinect, in the kinect folder of Xenia's storage folder
# (~/.local/share/Xenia by default):
#   - pose_detector.onnx and pose_landmarks_detector.onnx: MediaPipe's pose
#     landmarker (Google, Apache License 2.0), converted to ONNX by
#     convert_pose_models.py in a temporary Python environment;
#   - libonnxruntime.so.1: ONNX Runtime (Microsoft, MIT), only if the system
#     doesn't have it (Fedora: sudo dnf install onnxruntime).
# Needs curl and Python 3.10-3.13 (uv is used if it's installed).
set -e
storage=${1:-$HOME/.local/share/Xenia}
dest=$storage/kinect
here=$(cd "$(dirname "$0")" && pwd)
task_url=https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/latest/pose_landmarker_full.task
ort_version=1.22.0
ort_url=https://github.com/microsoft/onnxruntime/releases/download/v$ort_version/onnxruntime-linux-x64-$ort_version.tgz

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$dest" "$work/download"

echo "Downloading MediaPipe's pose landmarker..."
curl -fL --progress-bar -o "$work/download/pose_landmarker_full.task" "$task_url"

echo "Converting it to ONNX..."
packages="tflite onnx onnxruntime ai-edge-litert numpy"
if command -v uv > /dev/null; then
  uv venv -q --python 3.13 "$work/venv" || uv venv -q "$work/venv"
  uv pip install -q --python "$work/venv/bin/python" $packages
else
  python3 -m venv "$work/venv"
  "$work/venv/bin/pip" install -q $packages
fi
"$work/venv/bin/python" -I -W ignore "$here/convert_pose_models.py" \
  "$work/download/pose_landmarker_full.task" "$dest"

if ldconfig -p 2> /dev/null | grep -q 'libonnxruntime\.so\.1 '; then
  echo "ONNX Runtime is installed on the system."
elif [ -e "$dest/libonnxruntime.so.1" ]; then
  echo "ONNX Runtime is already in $dest."
else
  echo "Downloading ONNX Runtime $ort_version..."
  curl -fL --progress-bar -o "$work/download/ort.tgz" "$ort_url"
  tar -xzf "$work/download/ort.tgz" -C "$work/download"
  cp "$work/download/onnxruntime-linux-x64-$ort_version/lib/libonnxruntime.so.$ort_version" \
    "$dest/libonnxruntime.so.1"
  cp "$work/download/onnxruntime-linux-x64-$ort_version/LICENSE" \
    "$dest/onnxruntime-LICENSE"
fi
echo "Done: a webcam can stand in for a Kinect ($dest)."

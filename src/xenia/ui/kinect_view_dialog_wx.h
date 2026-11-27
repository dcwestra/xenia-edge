/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_KINECT_VIEW_DIALOG_WX_H_
#define XENIA_UI_KINECT_VIEW_DIALOG_WX_H_

#include <cstdint>
#include <functional>

#include <wx/bitmap.h>
#include <wx/frame.h>
#include <wx/timer.h>

#include "xenia/kernel/nui/kinect_view.h"

class wxDC;
class wxPanel;

namespace xe {
namespace ui {

// The Kinect view while no title runs, as a window of its own beside the game
// list: the camera the next game will use (kernel::nui::StartKinectPreview
// streams it), and how to stand or aim it. In a game the view is an ImGui
// overlay, which needs the presenter a title brings. on_close runs when the
// user closes it (its close box, F9 or Escape), and should Destroy it.
class KinectViewDialog : public wxFrame {
 public:
  KinectViewDialog(wxWindow* parent, std::function<void()> on_close);
  ~KinectViewDialog() override;
  bool Destroy() override;

 private:
  using KinectView = kernel::nui::KinectView;

  void OnTimer(wxTimerEvent& event);
  void OnPaint(wxPaintEvent& event);
  void UpdateBitmap(wxBitmap& bitmap, uint32_t& generation,
                    const KinectView::Picture& picture);
  // Draws text wrapped to width, returning how far down it went.
  int DrawWrapped(wxDC& dc, const wxString& text, int x, int y, int width,
                  int line_height);

  wxPanel* panel_;
  wxTimer timer_;
  KinectView::Snapshot snapshot_;
  wxBitmap depth_, color_, webcam_;
  uint32_t depth_generation_ = 0, color_generation_ = 0, webcam_generation_ = 0;
  std::function<void()> on_close_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_KINECT_VIEW_DIALOG_WX_H_

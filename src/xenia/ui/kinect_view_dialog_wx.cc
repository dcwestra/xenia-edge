/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/kinect_view_dialog_wx.h"

#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/tokenzr.h>

namespace xe {
namespace ui {

namespace {
// As the in-game view's pictures, in DIPs.
constexpr int kPictureWidth = 320;
constexpr int kPictureHeight = 240;
constexpr int kMargin = 12;
// Room below the pictures for the webcam's status and advice.
constexpr int kTextLines = 9;
// The view publishes pictures no more often than this.
constexpr int kRefreshMilliseconds = 66;
// MediaPipe's pose bones, by landmark, as the in-game view draws them.
constexpr int kPoseBones[][2] = {{11, 12}, {11, 13}, {13, 15}, {12, 14},
                                 {14, 16}, {11, 23}, {12, 24}, {23, 24},
                                 {23, 25}, {25, 27}, {27, 31}, {24, 26},
                                 {26, 28}, {28, 32}, {7, 0},   {0, 8}};
constexpr float kVisible = 0.5f;
}  // namespace

KinectViewDialog::KinectViewDialog(wxWindow* parent,
                                   std::function<void()> on_close)
    : wxFrame(parent, wxID_ANY, _("Kinect View"), wxDefaultPosition,
              wxDefaultSize,
              (wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX)) |
                  wxFRAME_FLOAT_ON_PARENT),
      timer_(this),
      on_close_(std::move(on_close)) {
  panel_ = new wxPanel(this);
  panel_->SetBackgroundStyle(wxBG_STYLE_PAINT);
  const int line_height = GetCharHeight() + FromDIP(4);
  SetClientSize(
      FromDIP(kPictureWidth * 2 + kMargin * 3),
      FromDIP(kPictureHeight + kMargin * 3) + line_height * (kTextLines + 2));
  panel_->Bind(wxEVT_PAINT, &KinectViewDialog::OnPaint, this);
  Bind(wxEVT_TIMER, &KinectViewDialog::OnTimer, this);
  auto close = [this]() {
    if (on_close_) {
      on_close_();
    } else {
      Destroy();
    }
  };
  Bind(wxEVT_CLOSE_WINDOW, [close](wxCloseEvent&) { close(); });
  Bind(wxEVT_CHAR_HOOK, [close](wxKeyEvent& event) {
    if (event.GetKeyCode() == WXK_F9 || event.GetKeyCode() == WXK_ESCAPE) {
      close();
      return;
    }
    event.Skip();
  });
  KinectView::Get().AddViewer();
  timer_.Start(kRefreshMilliseconds);
}

KinectViewDialog::~KinectViewDialog() {
  timer_.Stop();
  KinectView::Get().RemoveViewer();
}

bool KinectViewDialog::Destroy() {
  // Deleted later, when idle.
  timer_.Stop();
  return wxFrame::Destroy();
}

void KinectViewDialog::UpdateBitmap(wxBitmap& bitmap, uint32_t& generation,
                                    const KinectView::Picture& picture) {
  if (picture.generation == generation) {
    return;
  }
  generation = picture.generation;
  if (picture.rgba.empty() || picture.width <= 0 || picture.height <= 0) {
    bitmap = wxBitmap();
    return;
  }
  wxImage image(picture.width, picture.height, false);
  unsigned char* rgb = image.GetData();
  const size_t pixels = size_t(picture.width) * picture.height;
  for (size_t i = 0; i < pixels; ++i) {
    rgb[i * 3] = picture.rgba[i * 4];
    rgb[i * 3 + 1] = picture.rgba[i * 4 + 1];
    rgb[i * 3 + 2] = picture.rgba[i * 4 + 2];
  }
  const int width = FromDIP(kPictureWidth);
  if (width != picture.width) {
    image.Rescale(width, width * picture.height / picture.width,
                  wxIMAGE_QUALITY_BILINEAR);
  }
  bitmap = wxBitmap(image);
}

void KinectViewDialog::OnTimer(wxTimerEvent& event) {
  snapshot_ = KinectView::Get().Take();
  UpdateBitmap(depth_, depth_generation_, snapshot_.depth);
  UpdateBitmap(color_, color_generation_, snapshot_.color);
  UpdateBitmap(webcam_, webcam_generation_, snapshot_.webcam);
  panel_->Refresh(false);
}

int KinectViewDialog::DrawWrapped(wxDC& dc, const wxString& text, int x, int y,
                                  int width, int line_height) {
  wxString line;
  wxStringTokenizer words(text, " ");
  while (words.HasMoreTokens()) {
    const wxString word = words.GetNextToken();
    const wxString longer = line.empty() ? word : line + " " + word;
    if (!line.empty() && dc.GetTextExtent(longer).GetWidth() > width) {
      dc.DrawText(line, x, y);
      y += line_height;
      line = word;
    } else {
      line = longer;
    }
  }
  if (!line.empty()) {
    dc.DrawText(line, x, y);
    y += line_height;
  }
  return y;
}

void KinectViewDialog::OnPaint(wxPaintEvent& event) {
  wxAutoBufferedPaintDC dc(panel_);
  const wxColour background = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
  const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
  const bool dark = background.GetLuminance() < 0.5;
  const wxColour good = dark ? wxColour(128, 255, 153) : wxColour(0, 115, 40);
  const wxColour warning = dark ? wxColour(255, 217, 77) : wxColour(150, 95, 0);
  dc.SetBackground(wxBrush(background));
  dc.Clear();
  dc.SetFont(GetFont());

  const int margin = FromDIP(kMargin);
  const int line_height = dc.GetCharHeight() + FromDIP(4);
  const int width = panel_->GetClientSize().GetWidth() - margin * 2;
  const int picture_width = FromDIP(kPictureWidth);
  const int picture_height = FromDIP(kPictureHeight);
  int y = margin;

  if (snapshot_.preview == KinectView::Preview::kNoCamera) {
    dc.SetTextForeground(warning);
    DrawWrapped(dc,
                _("No Kinect or webcam to show. xenia.log says why (search "
                  "it for \"Kinect:\")."),
                margin, y, width, line_height);
    return;
  }
  dc.SetTextForeground(good);
  y = DrawWrapped(dc,
                  _("Set up the camera and where you stand here, then start "
                    "a game."),
                  margin, y, width, line_height);

  // What games will see, beside the webcam's picture with the pose found in
  // it, or the Kinect's color.
  const KinectView::Webcam& webcam = snapshot_.webcam_status;
  const bool have_webcam = webcam_.IsOk();
  const wxBitmap& picture = have_webcam ? webcam_ : color_;
  const int right = margin * 2 + picture_width;
  dc.SetTextForeground(text);
  dc.DrawText(_("What games will see"), margin, y);
  dc.DrawText(have_webcam ? _("What the webcam sees")
                          : _("What the Kinect's camera sees"),
              right, y);
  y += line_height;
  const wxColour frame = text.ChangeLightness(dark ? 40 : 160);
  dc.SetPen(wxPen(frame));
  dc.SetBrush(*wxTRANSPARENT_BRUSH);
  if (depth_.IsOk()) {
    dc.DrawBitmap(depth_, margin, y);
  } else {
    dc.DrawRectangle(margin, y, picture_width, picture_height);
  }
  if (picture.IsOk()) {
    dc.DrawBitmap(picture, right, y);
    if (have_webcam && webcam.seen) {
      const int w = picture.GetWidth();
      const int h = picture.GetHeight();
      dc.SetPen(wxPen(wxColour(80, 200, 255), FromDIP(2)));
      for (const auto& bone : kPoseBones) {
        if (webcam.visibility[bone[0]] > kVisible &&
            webcam.visibility[bone[1]] > kVisible) {
          dc.DrawLine(right + int(webcam.landmarks[bone[0]][0] * w),
                      y + int(webcam.landmarks[bone[0]][1] * h),
                      right + int(webcam.landmarks[bone[1]][0] * w),
                      y + int(webcam.landmarks[bone[1]][1] * h));
        }
      }
    }
  } else {
    dc.DrawRectangle(right, y, picture_width, picture_height);
  }
  y += picture_height + margin;

  if (!have_webcam) {
    return;
  }
  dc.SetTextForeground(text);
  y = DrawWrapped(
      dc,
      wxString::Format(_("Webcam: %s, %.0f frames a second, pose %.0f ms"),
                       wxString::FromUTF8(webcam.name),
                       webcam.frames_per_second, webcam.pose_ms),
      margin, y, width, line_height);
  if (webcam.seen) {
    y = DrawWrapped(
        dc,
        wxString::Format(
            _("You: %.2f m from the webcam, %.2f m from the "
              "Kinect%s"),
            webcam.distance, webcam.play_distance,
            webcam.settled ? wxString() : _(" (hold still a moment)")),
        margin, y, width, line_height);
  }
  y = DrawWrapped(dc,
                  wxString::Format(_("Kinect tilt: %+.0f degrees (simulated)"),
                                   webcam.tilt_degrees),
                  margin, y, width, line_height);
  dc.SetTextForeground(warning);
  for (const char* advice : KinectView::Advice(webcam, false)) {
    y = DrawWrapped(dc, wxString::FromUTF8(advice), margin, y, width,
                    line_height);
  }
}

}  // namespace ui
}  // namespace xe

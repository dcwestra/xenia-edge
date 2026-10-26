/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_UI_H_
#define XENIA_KERNEL_XAM_XAM_UI_H_

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_gamepad_dialog.h"

namespace xe {
namespace hid {
class InputSystem;
}  // namespace hid
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

class XamDialog : public xe::ui::ImGuiDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamDialog(xe::ui::ImGuiDrawer* imgui_drawer)
      : xe::ui::ImGuiDialog(imgui_drawer) {}

  virtual ~XamDialog() {};

  void OnClose() override {
    if (close_callback_) {
      close_callback_();
    }
  }

 private:
  std::function<void()> close_callback_ = nullptr;
};

// XamDialog with gamepad support for game-triggered dialogs
class XamGamepadDialog : public xe::ui::ImGuiGamepadDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamGamepadDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                   xe::hid::InputSystem* input_system)
      : xe::ui::ImGuiGamepadDialog(imgui_drawer, input_system, true) {}

  virtual ~XamGamepadDialog() {};

  void OnClose() override {
    if (close_callback_) {
      close_callback_();
    }
  }

 private:
  std::function<void()> close_callback_ = nullptr;
};

// A Kinect player's hand moving a cursor over a dialog's buttons, a hand held
// over one for a moment choosing it, as on a console.
class KinectCursor {
 public:
  void Enable(uint32_t tracking_id) {
    enabled_ = true;
    tracking_id_ = tracking_id;
  }
  bool enabled() const { return enabled_; }

  // Inside the dialog's window, before its buttons.
  void BeginFrame();
  // Right after a button.
  void TrackItem(int32_t id);
  // After the buttons: draws the cursor, and returns the button held long
  // enough, else -1.
  int32_t EndFrame();

 private:
  bool enabled_ = false;
  uint32_t tracking_id_ = 0;
  bool active_ = false;
  float pos_x_ = 0.0f;
  float pos_y_ = 0.0f;
  int32_t hovered_ = -1;
  int32_t held_ = -1;
  std::chrono::steady_clock::time_point hold_start_;
  std::chrono::steady_clock::time_point auto_start_;
  int32_t auto_button_ = -1;
};

class MessageBoxDialog : public XamGamepadDialog {
 public:
  MessageBoxDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                   xe::hid::InputSystem* input_system, const std::string& title,
                   const std::string& description,
                   std::vector<std::string> buttons, uint32_t default_button)
      : XamGamepadDialog(imgui_drawer, input_system),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
  }

  uint32_t chosen_button() const { return chosen_button_; }

  // Lets the hand of the Kinect player with the tracking ID (or any) choose
  // a button by holding a cursor over it, as on a console.
  void EnableKinectCursor(uint32_t tracking_id) {
    kinect_cursor_.Enable(tracking_id);
  }

  void OnDraw(ImGuiIO& io) override;
  virtual ~MessageBoxDialog() {}

 private:
  void Choose(uint32_t button);

  bool has_opened_ = false;
  KinectCursor kinect_cursor_;
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
};

class KeyboardInputDialog : public XamGamepadDialog {
 public:
  KeyboardInputDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                      xe::hid::InputSystem* input_system, std::string& title,
                      std::string& description, std::string& default_text,
                      size_t max_length)
      : XamGamepadDialog(imgui_drawer, input_system),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_length_(max_length),
        text_buffer_() {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    text_buffer_.resize(max_length);
    xe::string_util::copy_truncating(text_buffer_.data(), default_text_,
                                     text_buffer_.size());
  }
  virtual ~KeyboardInputDialog() {}

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override;

 private:
  bool has_opened_ = false;
  bool focus_set_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  size_t max_length_ = 0;
  std::vector<char> text_buffer_;
  std::string text_ = "";
  bool cancelled_ = true;
};

bool xeDrawProfileContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          const uint64_t xuid, const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const xe::ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid);

// Picks an installed indie game, or reports that there are none.
bool xeXamChooseIndieGame(std::string* file_name, uint32_t* device_id,
                          std::string* display_name);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif

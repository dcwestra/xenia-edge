/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_ui.h"

#include <algorithm>

#include "xenia/app/emulator_window.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/utf8.h"
#include "xenia/base/system.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui/nui_hud.h"
#include "xenia/kernel/nui/nui_identity.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_guest_notification.h"

#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/kernel/xam/ui/disc_swap_ui.h"
#include "xenia/kernel/xam/ui/game_achievements_ui.h"
#include "xenia/kernel/xam/ui/gamercard_ui.h"
#include "xenia/kernel/xam/ui/passcode_ui.h"
#include "xenia/kernel/xam/ui/signin_ui.h"
#include "xenia/kernel/xam/ui/title_info_ui.h"

DEFINE_bool(storage_selection_dialog, false,
            "Show storage device selection dialog when the game requests it.",
            "UI");

DEFINE_string(kinect_dialog_auto_buttons, "",
              "For testing with a Kinect recording: the button to press (0 "
              "the first) in each Kinect dialog in turn, two seconds after it "
              "opens, comma-separated. Empty leaves them to the player.",
              "Kernel");

DECLARE_int32(license_mask);

constexpr std::chrono::milliseconds kUIDelayMillis(200);

namespace xe {
namespace kernel {
namespace xam {
// TODO(gibbed): This is all one giant WIP that seems to work better than the
// previous immediate synchronous completion of dialogs.
//
// The deferred execution of dialog handling is done in such a way that there is
// a pre-, peri- (completion), and post- callback steps.
//
// pre();
// result = completion();
// CompleteOverlapped(result);
// post();
//
// There are games that are batshit insane enough to wait for the X_OVERLAPPED
// to be completed (ie not X_ERROR_PENDING) before creating a listener to
// receive a notification, which is why we have distinct pre- and post- steps.
//
// We deliberately delay the XN_SYS_UI = false notification to give games time
// to create a listener (if they're insane enough do this).
template <typename T>
X_RESULT xeXamDispatchDialog(T* dialog,
                             std::function<X_RESULT(T*)> close_callback,
                             uint32_t overlapped) {
  auto pre = []() {
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto run = [dialog, close_callback]() -> X_RESULT {
    X_RESULT result;
    dialog->set_close_callback([&dialog, &result, &close_callback]() {
      result = close_callback(dialog);
    });
    xe::threading::Fence fence;
    xe::ui::WindowedAppContext& app_context =
        kernel_state()->emulator()->display_window()->app_context();
    if (app_context.CallInUIThreadSynchronous(
            [&dialog, &fence]() { dialog->Then(&fence); })) {
      GuestScheduler::WaitOnFence(fence);
      kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    std::jthread t([] {
      xe::threading::Sleep(kUIDelayMillis);
      kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
    });
    t.detach();
  };
  if (!overlapped) {
    pre();
    auto result = run();
    post();
    return result;
  } else {
    kernel_state()->CompleteOverlappedDeferred(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

template <typename T>
X_RESULT xeXamDispatchDialogEx(
    T* dialog, std::function<X_RESULT(T*, uint32_t&, uint32_t&)> close_callback,
    uint32_t overlapped) {
  auto pre = []() {
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto run = [dialog, close_callback](uint32_t& extended_error,
                                      uint32_t& length) -> X_RESULT {
    auto display_window = kernel_state()->emulator()->display_window();
    X_RESULT result;
    dialog->set_close_callback(
        [&dialog, &result, &extended_error, &length, &close_callback]() {
          result = close_callback(dialog, extended_error, length);
        });
    xe::threading::Fence fence;
    if (display_window->app_context().CallInUIThreadSynchronous(
            [&dialog, &fence]() { dialog->Then(&fence); })) {
      GuestScheduler::WaitOnFence(fence);
      kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    xe::threading::Sleep(kUIDelayMillis);
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchWithoutDialog(std::function<X_RESULT()> run_callback,
                                    uint32_t overlapped) {
  auto pre = []() {
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
    xe::threading::Sleep(std::chrono::milliseconds(25));
  };
  auto post = []() {
    std::jthread t([]() {
      xe::threading::Sleep(kUIDelayMillis);
      kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
    });

    t.detach();
  };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    kernel_state()->CompleteOverlappedDeferred(run_callback, overlapped, pre,
                                               post);
    return X_ERROR_IO_PENDING;
  }
}

template <typename T>
X_RESULT xeXamDispatchDialogAsync(T* dialog,
                                  std::function<void(T*)> close_callback,
                                  std::function<void()> finish = nullptr) {
  kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  // Important to pass captured vars by value here since we return from this
  // without waiting for the dialog to close so the original local vars will be
  // destroyed.
  dialog->set_close_callback([dialog, close_callback, finish]() {
    close_callback(dialog);

    // What the system still does after the dialog goes, such as enrolling a
    // Kinect player for Kinect ID, comes before the title hears it's closed.
    if (!finish) {
      kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    }

    auto run = [finish]() -> void {
      if (finish) {
        finish();
        kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
      }
      xe::threading::Sleep(kUIDelayMillis);
      kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
    };

    std::thread thread(run);
    thread.detach();
  });

  return X_ERROR_SUCCESS;
}

void MessageBoxDialog::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
  }
  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Handle keyboard escape or gamepad B/Back to cancel
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      chosen_button_ = default_button_;
      ImGui::CloseCurrentPopup();
      Close();
    }

    if (description_.size()) {
      ImGui::Text("%s", description_.c_str());
    }

    kinect_cursor_.BeginFrame();
    if (kinect_cursor_.enabled()) {
      ImGui::TextDisabled("Hold your hand over a button to choose it.");
    }
    const ImVec2 button_size = kinect_cursor_.enabled()
                                   ? ImVec2(ImGui::GetFontSize() * 9.0f,
                                            ImGui::GetFontSize() * 3.0f)
                                   : ImVec2(0.0f, 0.0f);

    // Buttons - ImGui handles navigation, A button activates via ImGui
    for (size_t i = 0; i < buttons_.size(); ++i) {
      if (ImGui::Button(buttons_[i].c_str(), button_size)) {
        Choose(static_cast<uint32_t>(i));
      }
      kinect_cursor_.TrackItem(int32_t(i));
      // Set default focus on the default button
      if (i == default_button_) {
        ImGui::SetItemDefaultFocus();
      }
      ImGui::SameLine();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    const int32_t held = kinect_cursor_.EndFrame();
    if (held >= 0 && uint32_t(held) < buttons_.size()) {
      Choose(uint32_t(held));
    }
    ImGui::EndPopup();
  } else {
    Close();
  }
}

void KinectCursor::BeginFrame() {
  active_ = false;
  hovered_ = -1;
  if (!enabled_) {
    return;
  }
  // The hand spans a little more than the dialog.
  float x, y;
  if (!kernel_state()->nui_hud()->GetHandCursor(tracking_id_, x, y)) {
    return;
  }
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const ImVec2 window_size = ImGui::GetWindowSize();
  pos_x_ = window_pos.x + (x * 1.4f - 0.2f) * window_size.x;
  pos_y_ = window_pos.y + (y * 1.4f - 0.2f) * window_size.y;
  active_ = true;
}

void KinectCursor::TrackItem(int32_t id) {
  if (!active_) {
    return;
  }
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  if (pos_x_ >= min.x && pos_x_ < max.x && pos_y_ >= min.y &&
      pos_y_ < max.y) {
    hovered_ = id;
  }
}

int32_t KinectCursor::EndFrame() {
  if (!enabled_) {
    return -1;
  }
  const auto now = std::chrono::steady_clock::now();
  // For testing with a Kinect recording, which can't aim at a button.
  if (!cvars::kinect_dialog_auto_buttons.empty()) {
    if (auto_start_ == std::chrono::steady_clock::time_point()) {
      auto_start_ = now;
      static uint32_t dialogs_opened = 0;
      const auto buttons =
          xe::utf8::split(cvars::kinect_dialog_auto_buttons, ",");
      if (dialogs_opened < buttons.size()) {
        auto_button_ = std::atoi(std::string(buttons[dialogs_opened]).c_str());
      }
      ++dialogs_opened;
    } else if (auto_button_ >= 0 && now - auto_start_ > std::chrono::seconds(2)) {
      return auto_button_;
    }
  }
  if (!active_) {
    held_ = -1;
    return -1;
  }
  constexpr float kHoldSeconds = 1.5f;
  if (hovered_ != held_) {
    held_ = hovered_;
    hold_start_ = now;
  }
  float progress = 0.0f;
  if (held_ >= 0) {
    progress = std::chrono::duration<float>(now - hold_start_).count() /
               kHoldSeconds;
  }
  const ImVec2 pos(pos_x_, pos_y_);
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  const float radius = ImGui::GetFontSize() * 1.2f;
  draw_list->AddCircleFilled(pos, radius * 0.45f, IM_COL32(255, 255, 255, 230),
                             24);
  draw_list->AddCircle(pos, radius, IM_COL32(255, 255, 255, 160), 32, 2.0f);
  if (progress > 0.0f) {
    draw_list->PathArcTo(pos, radius, -1.5708f,
                         -1.5708f + std::min(progress, 1.0f) * 6.2832f, 32);
    draw_list->PathStroke(IM_COL32(16, 124, 16, 255), 0, 4.0f);
  }
  return progress >= 1.0f ? held_ : -1;
}

void MessageBoxDialog::Choose(uint32_t button) {
  chosen_button_ = button;
  ImGui::CloseCurrentPopup();
  Close();
}

void KeyboardInputDialog::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
  }

  // Center the window on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  // Size constraints leave an auto-resize popup a pixel short of its contents
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f),
                           ImGuiCond_Always);

  // Style like Xbox - white background, black text, Xbox green highlights
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));

  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Handle keyboard escape or gamepad B/Back to cancel
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      text_ = "";
      cancelled_ = true;
      ImGui::CloseCurrentPopup();
      Close();
    }

    if (description_.size()) {
      ImGui::TextWrapped("%s", description_.c_str());
    }

    ImGui::PushID("input_text");
    // Set focus to InputText on first frame
    if (!focus_set_) {
      ImGui::SetKeyboardFocusHere();
      focus_set_ = true;
    }
    bool input_submitted =
        ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    // Context menu for paste functionality
    if (ImGui::BeginPopupContextItem("input_context_menu")) {
      if (ImGui::MenuItem("Paste")) {
        if (ImGui::GetClipboardText() != nullptr) {
          std::string clipboard_text = ImGui::GetClipboardText();
          xe::string_util::copy_truncating(text_buffer_.data(), clipboard_text,
                                           text_buffer_.size());
        }
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();

    if (input_submitted) {
      text_ = std::string(text_buffer_.data(), text_buffer_.size());
      cancelled_ = false;
      ImGui::CloseCurrentPopup();
      Close();
    }

    // OK button
    if (ImGui::Button("OK")) {
      text_ = std::string(text_buffer_.data(), text_buffer_.size());
      cancelled_ = false;
      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::SetItemDefaultFocus();

    ImGui::SameLine();

    // Cancel button
    if (ImGui::Button("Cancel")) {
      text_ = "";
      cancelled_ = true;
      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::Spacing();
    ImGui::EndPopup();
  } else {
    Close();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(10);
}

// kinect: for the Kinect player with the tracking ID, choosing with their
// hand.
static dword_result_t XamShowMessageBoxUi(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped, bool kinect = false,
    uint32_t kinect_tracking_id = 0) {
  std::string title = title_ptr ? xe::to_utf8(title_ptr.value()) : "";
  std::string text = text_ptr ? xe::to_utf8(text_ptr.value()) : "";

  std::vector<std::string> buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    uint32_t button_ptr = button_ptrs[i];
    auto button = xe::load_and_swap<std::u16string>(
        kernel_state()->memory()->TranslateVirtual(button_ptr));

    if (!button.empty()) {
      buttons.push_back(xe::to_utf8(button));
    }
  }

  if (buttons.empty()) {
    buttons.push_back("OK");
  }

  X_RESULT result;
  switch (flags & 0xF) {
    case XMBox_NOICON: {
    } break;
    case XMBox_ERRORICON: {
    } break;
    case XMBox_WARNINGICON: {
    } break;
    case XMBox_ALERTICON: {
    } break;
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();

  if (flags & XMBox_PASSCODEMODE || flags & XMBox_VERIFYPASSCODEMODE) {
    auto close = [result_ptr,
                  active_button](ui::ProfilePasscodeUI* dialog) -> X_RESULT {
      if (dialog->SelectedSignedIn()) {
        // Logged in
        return X_ERROR_SUCCESS;
      } else {
        return X_ERROR_FUNCTION_FAILED;
      }
    };

    result = xeXamDispatchDialog<ui::ProfilePasscodeUI>(
        new ui::ProfilePasscodeUI(imgui_drawer, title, text, result_ptr), close,
        overlapped);
  } else {
    auto close = [result_ptr](MessageBoxDialog* dialog) -> X_RESULT {
      XELOGI("XamShowMessageBoxUI: button {} chosen", dialog->chosen_button());
      result_ptr->ButtonPressed = dialog->chosen_button();
      kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
      return X_ERROR_SUCCESS;
    };

    auto* dialog =
        new MessageBoxDialog(imgui_drawer, input_system, title, text, buttons,
                             static_cast<uint32_t>(active_button));
    if (kinect) {
      dialog->EnableKinectCursor(kinect_tracking_id);
    }
    result = xeXamDispatchDialog<MessageBoxDialog>(dialog, close, overlapped);
  }

  return result;
}

dword_result_t XamIsUIActive_entry() {
  return kernel_state()->xam_state()->IsUIActive();
}
DECLARE_XAM_EXPORT2(XamIsUIActive, kUI, kImplemented, kHighFrequency);

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
dword_result_t XamShowMessageBoxUI_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUI, kUI, kImplemented);

dword_result_t XamShowMessageBoxUIEx_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, dword_t unknown_unused,
    pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUIEx, kUI, kImplemented);

// XamShowMessageBoxUI for a Kinect player, named by their skeleton tracking
// ID (4D530A1A passes 255 as the user index along with it).
dword_result_t XamShowNuiMessageBoxUI_entry(
    dword_t tracking_id, dword_t user_index, lpu16string_t title_ptr,
    lpu16string_t text_ptr, dword_t button_count, lpdword_t button_ptrs,
    dword_t active_button, dword_t flags,
    pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  std::string buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    if (button_ptrs && button_ptrs[i]) {
      buttons += fmt::format(
          "{}\"{}\"", i ? ", " : "",
          xe::to_utf8(xe::load_and_swap<std::u16string>(
              kernel_state()->memory()->TranslateVirtual(button_ptrs[i]))));
    }
  }
  XELOGI("XamShowNuiMessageBoxUI({:08X}, {}): {} [{}], flags {:X}",
         uint32_t(tracking_id), uint32_t(user_index),
         text_ptr ? xe::to_utf8(text_ptr.value()) : std::string(), buttons,
         uint32_t(flags));
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped, true, tracking_id);
}
DECLARE_XAM_EXPORT1(XamShowNuiMessageBoxUI, kUI, kSketchy);

dword_result_t XNotifyQueueUI_entry(dword_t exnq, dword_t dwUserIndex,
                                    qword_t qwAreas,
                                    lpu16string_t displayText_ptr,
                                    lpvoid_t contextData) {
  std::string displayText = "";
  const uint8_t position_id = static_cast<uint8_t>(qwAreas);

  if (displayText_ptr) {
    displayText = xe::to_utf8(displayText_ptr.value());
  }

  XELOGI("XNotifyQueueUI: {}", displayText);

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  new xe::ui::XNotifyWindow(imgui_drawer, "", displayText, dwUserIndex,
                            position_id);

  // XNotifyQueueUI -> XNotifyQueueUIEx -> XMsgProcessRequest ->
  // XMsgStartIORequestEx & XMsgInProcessCall
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNotifyQueueUI, kUI, kSketchy);

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
dword_result_t XamShowKeyboardUI_entry(
    dword_t user_index, dword_t flags, lpu16string_t default_text,
    lpu16string_t title, lpu16string_t description, lpu16string_t buffer,
    dword_t buffer_length, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  assert_not_null(overlapped);

  auto close = [buffer, buffer_length](KeyboardInputDialog* dialog,
                                       uint32_t& extended_error,
                                       uint32_t& length) -> X_RESULT {
    if (dialog->cancelled()) {
      extended_error = X_ERROR_CANCELLED;
      length = 0;
      return X_ERROR_SUCCESS;
    } else {
      // Zero the output buffer.
      auto text = xe::to_utf16(dialog->text());
      string_util::copy_and_swap_truncating(buffer, text, buffer_length);
      extended_error = X_ERROR_SUCCESS;
      length = 0;
      return X_ERROR_SUCCESS;
    }
  };

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();

  std::string title_str = title ? xe::to_utf8(title.value()) : "";
  std::string desc_str = description ? xe::to_utf8(description.value()) : "";
  std::string def_text_str =
      default_text ? xe::to_utf8(default_text.value()) : "";

  // If no default text provided, use the user's gamertag
  if (def_text_str.empty()) {
    auto profile_manager = kernel_state()->xam_state()->profile_manager();
    if (profile_manager) {
      // Try the specified user_index first, fall back to slot 0
      auto profile =
          profile_manager->GetProfile(static_cast<uint8_t>(user_index));
      if (!profile) {
        profile = profile_manager->GetProfile(static_cast<uint8_t>(0));
      }
      if (profile) {
        def_text_str = profile->name();
      }
    }
  }

  return xeXamDispatchDialogEx<KeyboardInputDialog>(
      new KeyboardInputDialog(imgui_drawer, input_system, title_str, desc_str,
                              def_text_str, buffer_length),
      close, overlapped);
}
DECLARE_XAM_EXPORT1(XamShowKeyboardUI, kUI, kImplemented);

dword_result_t XamShowDeviceSelectorUI_entry(
    dword_t user_index, dword_t content_type, dword_t content_flags,
    qword_t total_requested, lpdword_t device_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!overlapped) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if ((user_index >= XUserMaxUserCount && user_index != XUserIndexAny) ||
      (content_flags & 0x83F00008) != 0 || !device_id_ptr) {
    XOverlappedSetExtendedError(overlapped, X_ERROR_INVALID_PARAMETER);
    return X_ERROR_INVALID_PARAMETER;
  }

  std::vector<const DummyDeviceInfo*> devices = ListStorageDevices();

  if (!cvars::storage_selection_dialog) {
    // Default to the first storage device (HDD).
    return xeXamDispatchWithoutDialog(
        [device_id_ptr, devices]() -> X_RESULT {
          if (devices.empty()) {
            return X_ERROR_CANCELLED;
          }

          const DummyDeviceInfo* device_info = devices.front();
          *device_id_ptr = static_cast<uint32_t>(device_info->device_id);
          return X_ERROR_SUCCESS;
        },
        overlapped);
  }

  auto close = [device_id_ptr, devices](MessageBoxDialog* dialog) -> X_RESULT {
    uint32_t button = dialog->chosen_button();
    if (button >= devices.size()) {
      return X_ERROR_CANCELLED;
    }

    const DummyDeviceInfo* device_info = devices.at(button);
    *device_id_ptr = static_cast<uint32_t>(device_info->device_id);
    return X_ERROR_SUCCESS;
  };

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  std::string title = "Select storage device";
  std::string desc = "";

  cxxopts::OptionNames buttons;
  for (auto& device_info : devices) {
    buttons.push_back(to_utf8(device_info->name));
  }
  buttons.push_back("Cancel");

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  return xeXamDispatchDialog<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, input_system, title, desc, buttons, 0),
      close, overlapped);
}
DECLARE_XAM_EXPORT1(XamShowDeviceSelectorUI, kUI, kImplemented);

// XamShowDeviceSelectorUI for a Kinect player, named by their skeleton tracking
// ID. Kinect titles pick where they save with it.
dword_result_t XamShowNuiDeviceSelectorUI_entry(
    dword_t tracking_id, dword_t user_index, dword_t content_type,
    dword_t content_flags, qword_t total_requested, lpdword_t device_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  XELOGI("XamShowNuiDeviceSelectorUI({:08X}, {}, {:08X}, {:08X}, {})",
         uint32_t(tracking_id), uint32_t(user_index), uint32_t(content_type),
         uint32_t(content_flags), uint64_t(total_requested));
  return XamShowDeviceSelectorUI_entry(user_index, content_type, content_flags,
                                       total_requested, device_id_ptr,
                                       overlapped);
}
DECLARE_XAM_EXPORT1(XamShowNuiDeviceSelectorUI, kUI, kSketchy);

const char kDiscReadErrorTitle[] = "Disc Read Error";
const char kDiscReadErrorDescription[] =
    "There's been an issue reading content from the game disc.\nThis is "
    "likely caused by bad or unimplemented file IO calls.";

void XamShowDirtyDiscErrorUI_entry(dword_t user_index,
                                   const ppc_context_t& context) {
  XELOGE("XamShowDirtyDiscErrorUI: title reported a disc read error, lr {:08X}",
         uint32_t(context->lr));

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  xeXamDispatchDialog<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, input_system, kDiscReadErrorTitle,
                           kDiscReadErrorDescription, {"OK"}, 0),
      [](MessageBoxDialog*) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  // Does not return.
  kernel_state()->ExitToDashboard();
}
DECLARE_XAM_EXPORT1(XamShowDirtyDiscErrorUI, kUI, kImplemented);

void XamShowNuiDirtyDiscErrorUI_entry(dword_t tracking_id) {
  // xeXamNuiHudCheck & XamShowAndWaitForMessageBoxEx, same as
  // XamShowDirtyDiscErrorUI except flag = 0x2001, user index always set to
  // XUserIndexAny
  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  xeXamDispatchDialog<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, input_system, kDiscReadErrorTitle,
                           kDiscReadErrorDescription, {"OK"}, 0),
      [](MessageBoxDialog*) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
}
DECLARE_XAM_EXPORT1(XamShowNuiDirtyDiscErrorUI, kUI, kStub);

dword_result_t XamShowPartyUI_entry(dword_t user_index) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowPartyUI, kUI, kStub);

dword_result_t XamShowCommunitySessionsUI_entry(dword_t user_index,
                                                dword_t social_sessions_flags) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowCommunitySessionsUI, kUI, kStub);

// https://gitlab.com/GlitchyScripts/xlivelessness/-/blob/master/xlivelessness/xlive/xdefs.hpp?ref_type=heads#L1235
dword_result_t XamShowMarketplaceUIEx_entry(dword_t user_index, dword_t ui_type,
                                            qword_t offer_id,
                                            dword_t offer_type,
                                            dword_t content_category,
                                            unknown_t unk6, unknown_t unk7,
                                            dword_t title_id) {
  // ui_type:
  // 0 - view all content for the current title
  // 1 - view content specified by offer id
  // offer_types:
  // filter for content list, usually just -1
  // content_category:
  // filter on item types for games (e.g. cars, maps, weapons, etc)
  if (user_index >= XUserMaxUserCount || ui_type >= MaxEntrypoint) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  bool is_xbla_unlock_offer =
      (offer_id == ((uint64_t(kernel_state()->title_id()) << 32) | 1ull));

  auto close = [ui_type,
                is_xbla_unlock_offer](MessageBoxDialog* dialog) -> void {
    if (ui_type == 1 && is_xbla_unlock_offer) {
      uint32_t button = dialog->chosen_button();
      if (button == 0) {
        cvars::license_mask = 1;

        kernel_state()->BroadcastNotification(
            kXNotificationLiveContentInstalled, 0);
      }
    }
  };

  std::string title = "Xbox Marketplace";
  std::string desc = "";
  cxxopts::OptionNames buttons;

  switch (ui_type) {
    case X_MARKETPLACE_ENTRYPOINT::ContentList:
      desc =
          "Game requested to open marketplace page with all content for the "
          "current title ID.";
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem:
      desc = fmt::format(
          "Game requested to open marketplace page for offer ID 0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::MembershipList:
      desc =
          "Game requested to open marketplace page with all Xbox Live "
          "memberships.";
      break;
    case X_MARKETPLACE_ENTRYPOINT::MembershipItem:
      desc = fmt::format(
          "Game requested to open marketplace page for an Xbox Live "
          "membership offer 0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentList_Background:
      // Used when accessing microsoft points
      desc = fmt::format(
          "Xbox Marketplace requested access to Microsoft Points offer page "
          "0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem_Background:
      // Used when accessing credit card information and calls
      // XamShowCreditCardUI
      desc = fmt::format(
          "Xbox Marketplace requested access to credit card information page "
          "0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV1:
      // Used by XamShowForcedNameChangeUI v1888
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV2:
      // Used by XamShowForcedNameChangeUI NXE and up
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::GamerTagPurchase:
      // Used by dashboard when selecting change gamertag in profile menu
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ActiveDownloads:
      // Used in profile tabs when clicking active downloads
      desc = fmt::format(
          "There are no current plans to download files from Xbox "
          "Marketplace.");
      break;
    default:
      desc = fmt::format("Unknown marketplace op {:d}",
                         static_cast<uint32_t>(ui_type));
      break;
  }

  desc +=
      "\nNote that since Xenia cannot access Xbox Marketplace, any DLC must be "
      "installed manually using File -> Install Content.";

  switch (ui_type) {
    case X_MARKETPLACE_ENTRYPOINT::ContentList:
    default:
      buttons.push_back("OK");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem:
      if (is_xbla_unlock_offer) {
        desc +=
            "\n\nTo start trial games in full mode, set license_mask to 1 in "
            "Xenia config file.\n\nDo you wish to change license_mask to 1 for "
            "*this session*?";
        buttons.push_back("Yes");
        buttons.push_back("No");
      } else {
        buttons.push_back("OK");
      }
      break;
  }

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  return xeXamDispatchDialogAsync<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, input_system, title, desc, buttons, 0),
      close);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUIEx, kUI, kSketchy);

dword_result_t XamShowMarketplaceUI_entry(dword_t user_index, dword_t ui_type,
                                          qword_t offer_id, dword_t offer_type,
                                          dword_t content_category,
                                          dword_t title_id) {
  return XamShowMarketplaceUIEx_entry(user_index, ui_type, offer_id, offer_type,
                                      content_category, 0, 0, title_id);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUI, kUI, kSketchy);

dword_result_t XamShowNuiMarketplaceUI_entry(
    dword_t tracking_id, dword_t user_index, dword_t ui_type, qword_t offer_id,
    dword_t offer_type, dword_t content_category, dword_t title_id) {
  return XamShowMarketplaceUI_entry(user_index, ui_type, offer_id, offer_type,
                                    content_category, title_id);
}
DECLARE_XAM_EXPORT1(XamShowNuiMarketplaceUI, kUI, kSketchy);

dword_result_t XamShowMarketplaceDownloadItemsUI_entry(
    dword_t user_index, dword_t ui_type, lpqword_t offers, dword_t num_offers,
    lpdword_t hresult_ptr, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (user_index >= XUserMaxUserCount || !offers || num_offers > 6) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped,
                                                  X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  auto close = [hresult_ptr](MessageBoxDialog* dialog) -> X_RESULT {
    if (hresult_ptr) {
      // TODO
      *hresult_ptr = X_E_SUCCESS;
    }
    return X_ERROR_SUCCESS;
  };

  std::string title = "Xbox Marketplace";
  std::string desc = "";
  cxxopts::OptionNames buttons = {"OK"};

  switch (ui_type) {
    case X_MARKETPLACE_DOWNLOAD_ITEMS_ENTRYPOINTS::FREEITEMS:
      desc =
          "Game requested to open download page for the following free offer "
          "IDs:";
      break;
    case X_MARKETPLACE_DOWNLOAD_ITEMS_ENTRYPOINTS::PAIDITEMS:
      desc =
          "Game requested to open download page for the following offer IDs:";
      break;
    default:
      return X_ERROR_INVALID_PARAMETER;
  }

  for (uint32_t i = 0; i < num_offers; i++) {
    desc += fmt::format("\n0x{:16X}", offers[i].get());
  }

  desc +=
      "\n\nNote that since Xenia cannot access Xbox Marketplace, any DLC "
      "must "
      "be installed manually using File -> Install Content.";

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  return xeXamDispatchDialog<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, input_system, title, desc, buttons, 0),
      close, overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceDownloadItemsUI, kUI, kSketchy);

dword_result_t XamShowGoldUpgradeUI_entry(dword_t user_index, dword_t unk,
                                          dword_t title_id) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowGoldUpgradeUI, kUI, kStub);

dword_result_t XamShowMultiplayerUpgradeUI_entry(dword_t user_index) {
  return XamShowGoldUpgradeUI_entry(user_index, 6,
                                    kernel_state()->emulator()->title_id());
}
DECLARE_XAM_EXPORT1(XamShowMultiplayerUpgradeUI, kUI, kImplemented);

dword_result_t XamShowForcedNameChangeUI_entry(dword_t user_index) {
  return XamShowMarketplaceUIEx_entry(
      user_index, X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV2, 0, 0xffffffff,
      0, 0, 0, 0);
}
DECLARE_XAM_EXPORT1(XamShowForcedNameChangeUI, kUI, kImplemented);

bool xeDrawProfileContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          const uint64_t xuid, const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const xe::ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid) {
  const ImVec2 start_position = ImGui::GetCursorPos();

  // Calculate the size for the selectable based on icon + text
  const float text_height = ImGui::GetTextLineHeight() * 3;  // 3 lines of text
  const ImVec2 content_size =
      ImVec2(ImGui::GetContentRegionAvail().x,
             std::max(xe::ui::default_image_icon_size.y, text_height));

  // Draw selectable first as background
  bool clicked = false;
  if (xuid && selected_xuid) {
    if (ImGui::Selectable("##Selectable", *selected_xuid == xuid,
                          ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowOverlap,
                          content_size)) {
      *selected_xuid = xuid;
      clicked = true;
    }
    // Update selection when focus moves to this item
    if (ImGui::IsItemFocused()) {
      *selected_xuid = xuid;
    }
    // Reset cursor to draw content on top
    ImGui::SetCursorPos(start_position);
  }

  // Draw content on top of selectable
  ImGui::BeginGroup();
  {
    if (profile_icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(profile_icon),
                   xe::ui::default_image_icon_size);
    } else {
      if (user_index < XUserMaxUserCount) {
        const auto icon = imgui_drawer->GetNotificationIcon(user_index);
        ImGui::Image(reinterpret_cast<ImTextureID>(icon),
                     xe::ui::default_image_icon_size);
      } else {
        ImGui::Dummy(xe::ui::default_image_icon_size);
      }
    }

    ImGui::SameLine();

    ImGui::BeginGroup();
    {
      ImGui::TextUnformatted(
          fmt::format("User: {}\n", account->GetGamertagString()).c_str());
      ImGui::TextUnformatted(fmt::format("XUID: {:016X}  \n", xuid).c_str());
      if (user_index != XUserIndexAny) {
        ImGui::TextUnformatted(
            fmt::format("Assigned to slot: {}\n", user_index + 1).c_str());
      } else {
        ImGui::TextUnformatted(fmt::format("Profile is not signed in").c_str());
      }
    }
    ImGui::EndGroup();
  }
  ImGui::EndGroup();

  if (clicked) {
    ImGui::OpenPopup("Profile Menu");
  }

  if (context_menu) {
    return context_menu();
  }

  return true;
}

// kinect: for the Kinect player with the tracking ID, who is enrolled for
// Kinect ID as the profile they sign in.
X_RESULT xeXamShowSigninUI(uint32_t user_index, uint32_t users_needed,
                           uint32_t flags, bool kinect = false,
                           uint32_t kinect_tracking_id = 0) {
  // Mask values vary. Probably matching user types? Local/remote?
  // Games seem to sit and loop until we trigger sign in notification.
  if (users_needed != 1 && users_needed != 2 && users_needed != 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  auto close = [kinect, kinect_tracking_id](ui::SigninUI* dialog) -> void {
    if (kinect && dialog->signed_in_user() != XUserIndexAny) {
      kernel_state()->nui_identity()->RequestEnrollment(
          kinect_tracking_id, dialog->signed_in_user());
    }
  };
  // The player is the profile once enrolled, which the title looks for as
  // the sign-in closes.
  std::function<void()> finish;
  if (kinect) {
    finish = []() {
      kernel_state()->nui_identity()->WaitForEnrollment(
          std::chrono::seconds(10));
    };
  }

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  auto* dialog = new ui::SigninUI(
      imgui_drawer, kernel_state()->xam_state()->profile_manager(),
      emulator->input_system()->GetLastUsedSlot(), users_needed);
  if (kinect) {
    dialog->EnableKinectCursor(kinect_tracking_id);
  }
  return xeXamDispatchDialogAsync<ui::SigninUI>(dialog, close, finish);
}

X_RESULT xeXamShowCreateProfileUIEx(uint32_t user_index, dword_t flag,
                                    char* unkn2_ptr) {
  Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  auto close = [](ui::CreateProfileUI* dialog) -> void {};

  return xeXamDispatchDialogAsync<ui::CreateProfileUI>(
      new ui::CreateProfileUI(imgui_drawer, emulator), close);
}

dword_result_t XamShowSigninUI_entry(dword_t users_needed, dword_t flags) {
  return xeXamShowSigninUI(XUserIndexAny, users_needed, flags);
}
DECLARE_XAM_EXPORT1(XamShowSigninUI, kUserProfiles, kImplemented);

dword_result_t XamShowSigninUIEx_entry(
    dword_t users_needed, dword_t flags,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  X_RESULT result = xeXamShowSigninUI(XUserIndexAny, users_needed, flags);
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamShowSigninUIEx, kUserProfiles, kSketchy);

dword_result_t XamShowNuiSigninUI_entry(dword_t tracking_id, dword_t user_index,
                                        dword_t flags) {
  uint32_t users_needed = 1;
  uint32_t sent_flags = flags | static_cast<uint32_t>(SigninUiFlags::NUI);
  XELOGI("XamShowNuiSigninUI({:08X}, {}, {:X})", uint32_t(tracking_id),
         uint32_t(user_index), uint32_t(flags));
  // xeXamNuiHudCheck(tracking_id) = success then continue else return
  return xeXamShowSigninUI(user_index, users_needed, sent_flags, true,
                           tracking_id);
}
DECLARE_XAM_EXPORT1(XamShowNuiSigninUI, kUserProfiles, kSketchy);

dword_result_t XamShowSigninUIp_entry(dword_t user_index, dword_t users_needed,
                                      dword_t flags) {
  return xeXamShowSigninUI(user_index, users_needed, flags);
}
DECLARE_XAM_EXPORT1(XamShowSigninUIp, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUIEx_entry(dword_t user_index, dword_t flag,
                                              lpstring_t unkn2_ptr) {
  return xeXamShowCreateProfileUIEx(user_index, flag, unkn2_ptr);
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUIEx, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUI_entry(dword_t user_index, dword_t flag) {
  return xeXamShowCreateProfileUIEx(user_index, flag, 0);
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUI, kUserProfiles, kImplemented);

dword_result_t XamShowAchievementsUI_entry(dword_t user_index,
                                           dword_t title_id) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_NO_SUCH_USER;
  }

  uint32_t proper_title_id =
      title_id ? title_id.value()
               : kernel_state()->xam_state()->spa_info()->title_id();

  const auto info =
      kernel_state()->xam_state()->user_tracker()->GetUserTitleInfo(
          user->xuid(), proper_title_id);

  if (!info) {
    return X_ERROR_NO_SUCH_USER;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();
  xe::hid::InputSystem* input_system =
      kernel_state()->emulator()->input_system();

  auto close = [](ui::GameAchievementsUI* dialog) -> void {};
  return xeXamDispatchDialogAsync<ui::GameAchievementsUI>(
      new ui::GameAchievementsUI(imgui_drawer, input_system, &info.value(),
                                 user),
      close);
}
DECLARE_XAM_EXPORT1(XamShowAchievementsUI, kUserProfiles, kImplemented);

dword_result_t XamShowNuiAchievementsUI_entry(dword_t tracking_id,
                                              dword_t user_index,
                                              dword_t title_id) {
  return XamShowAchievementsUI_entry(user_index, title_id);
}
DECLARE_XAM_EXPORT1(XamShowNuiAchievementsUI, kUserProfiles, kSketchy);

dword_result_t XamShowNuiGamerCardUIForXUID_entry(dword_t tracking_id,
                                                  dword_t user_index,
                                                  qword_t xuid,
                                                  dword_t unknown) {
  // There is no gamer card to show.
  XELOGI("XamShowNuiGamerCardUIForXUID({:08X}, {}, {:016X})",
         uint32_t(tracking_id), uint32_t(user_index), uint64_t(xuid));
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiGamerCardUIForXUID, kUI, kStub);

dword_result_t XamShowGamerCardUI_entry(dword_t user_index) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_ACCESS_DENIED;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();

  auto close = [](ui::GamercardUI* dialog) -> void {};
  return xeXamDispatchDialogAsync<ui::GamercardUI>(
      new ui::GamercardUI(kernel_state()->emulator()->display_window(),
                          imgui_drawer, kernel_state(), user->xuid()),
      close);
}
DECLARE_XAM_EXPORT1(XamShowGamerCardUI, kUserProfiles, kImplemented);

dword_result_t XamShowEditProfileUI_entry(dword_t user_index) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_ACCESS_DENIED;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();

  auto close = [](ui::GamercardUI* dialog) -> void {};
  return xeXamDispatchDialogAsync<ui::GamercardUI>(
      new ui::GamercardUI(kernel_state()->emulator()->display_window(),
                          imgui_drawer, kernel_state(), user->xuid()),
      close);
}
DECLARE_XAM_EXPORT1(XamShowEditProfileUI, kUserProfiles, kImplemented);

bool xeXamChooseIndieGame(std::string* file_name, uint32_t* device_id,
                          std::string* display_name) {
  const Emulator* emulator = kernel_state()->emulator();
  if (!emulator->display_window() || !emulator->imgui_drawer()) {
    return false;
  }

  auto games = kernel_state()->content_manager()->ListContent(
      static_cast<uint32_t>(DummyDeviceId::HDD), 0, kXN_2002,
      XContentType::kMarketplaceContent);
  std::sort(
      games.begin(), games.end(),
      [](const XCONTENT_AGGREGATE_DATA& a, const XCONTENT_AGGREGATE_DATA& b) {
        return a.display_name() < b.display_name();
      });
  std::vector<ui::DiscSwapUI::DiscInfo> items;
  for (const auto& game : games) {
    const std::string name = xe::to_utf8(game.display_name());
    items.push_back({name.empty() ? game.file_name() : name,
                     xe::to_path(game.file_name())});
  }

  const bool none_installed = items.empty();
  const std::string message =
      none_installed ? "ERROR: No indie games are installed. Install one with "
                       "Tools > Install Content, then try again."
                     : std::string();
  std::filesystem::path chosen;
  auto close = [&chosen](ui::DiscSwapUI* dialog) -> X_RESULT {
    if (dialog->result() == ui::DiscSwapResult::kSelected) {
      chosen = dialog->selected_path();
    }
    return X_ERROR_SUCCESS;
  };
  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);
  xeXamDispatchDialog<ui::DiscSwapUI>(
      new ui::DiscSwapUI(emulator->imgui_drawer(), emulator->input_system(),
                         message, items, none_installed, "Indie Games",
                         "Select a game to play:", /*allow_browse=*/false),
      close, /*overlapped=*/0);

  for (size_t i = 0; i < games.size(); ++i) {
    if (!chosen.empty() && items[i].path == chosen) {
      *file_name = games[i].file_name();
      *device_id = games[i].device_id;
      *display_name = items[i].label;
      return true;
    }
  }
  return false;
}

// UI
dword_result_t XamShowNuiTroubleshooterUI_entry(dword_t user_index,
                                                dword_t tracking_id,
                                                dword_t flags) {
  /* Notes:
     - calls XamPackageManagerGetExperienceMode(&var) with var = 1
     - If returns less than zero or (var & 1) == 0 then get error message:
       - if XamPackageManagerGetExperienceMode = 0 then call XamShowMessageBoxUI
         - if XamShowMessageBoxUI returns 0x3e5 then XamShowNuiTroubleshooterUI
     returns 0
       - else XamShowNuiTroubleshooterUI returns 0x65b and call another func
     - else:
       - call XamNuiHudSetEngagedTrackingID(tracking_id) and doesn't care aboot
     return and set var2 = 2
       - checks if (flag & 0x800000) == 0
         - if true call XamNuiGetDeviceStatus.
           - if XamNuiGetDeviceStatus != 0 set var2 = 3
       - else var2 = 4
       - XamAppRequestLoadEx(var2);
       - if return = 0 then XamShowNuiTroubleshooterUI returns 5
       - else set buffer[8] and call
     XMsgSystemProcessCall(0xfe,0x21028,buffer,0xc);
     - XamNuiNatalCameraUpdateComplete calls
     XamShowNuiTroubleshooterUI(0xff,0,0) if param = -0x7ff8fffe
  */

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::Window* display_window = emulator->display_window();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  if (display_window && imgui_drawer) {
    xe::threading::Fence fence;
    if (display_window->app_context().CallInUIThreadSynchronous([&]() {
          xe::ui::ImGuiDialog::ShowMessageBox(
              imgui_drawer, "NUI Troubleshooter",
              "The game has indicated there is a problem with NUI (Kinect).")
              ->Then(&fence);
        })) {
      kernel_state()->xam_state()->is_xam_dialog_present_.store(true);
      GuestScheduler::WaitOnFence(fence);
      kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    }
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiTroubleshooterUI, kNone, kStub);

dword_result_t XamShowNuiHardwareRequiredUI_entry(unknown_t unk1) {
  if (unk1 != 0) {
    return X_ERROR_INVALID_PARAMETER;
  }

  return XamShowNuiTroubleshooterUI_entry(0xff, 0, 0x400000);
}
DECLARE_XAM_EXPORT1(XamShowNuiHardwareRequiredUI, kNone, kImplemented);

dword_result_t XamShowNuiGuideUI_entry(unknown_t tracking_id, unknown_t unk2) {
  /* Notes:
   - calls xeXamNuiHudCheck, if success call XamAppRequestLoadEx(4) and if
   succeed call XMsgSystemProcessCall and some sub function that ensures correct
   h_result is returned
   - if XamAppRequestLoadEx(4) fails return X_ERROR_ACCESS_DENIED
  */

  return X_ERROR_ACCESS_DENIED;
}
DECLARE_XAM_EXPORT1(XamShowNuiGuideUI, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(UI);

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_AVATAR_EDITOR_DIALOG_WX_H_
#define XENIA_UI_AVATAR_EDITOR_DIALOG_WX_H_

#include <array>
#include <cstdint>
#include <vector>

#include <wx/bitmap.h>
#include <wx/dialog.h>

#include "xenia/kernel/xam/avatar_assets.h"

class wxColourPickerCtrl;
class wxOwnerDrawnComboBox;
class wxPanel;
class wxSizer;
class wxSlider;

namespace xe {
namespace app {

class EmulatorWindow;

// Edits a profile's Xbox avatar from the avatar asset pack's items, and saves
// it to the profile for every title.
class AvatarEditorDialog : public wxDialog {
 public:
  AvatarEditorDialog(wxWindow* parent, EmulatorWindow* emulator_window,
                     uint64_t xuid);

 private:
  struct Category {
    // Its list opens below it with a scrollbar (GTK's wxChoice menus open
    // around the selection and close unless the button is held).
    wxOwnerDrawnComboBox* choice = nullptr;
    // Item slot masks it lists.
    std::vector<uint32_t> slots;
    bool optional = true;
    // The pack records listed after "(None)" when optional.
    std::vector<uint32_t> items;
  };

  void LoadAvatar();
  void Build();
  wxOwnerDrawnComboBox* AddCategory(wxWindow* parent, wxSizer* sizer,
                                    const wxString& label,
                                    std::vector<uint32_t> slots, bool optional);
  void PopulateCategories();
  void RefreshControls();
  void OnCategoryChanged(size_t category);
  void OnGenderChanged();
  void OnRandomize();
  void OnImport();
  void OnExport();
  void OnSave();
  // Rebuilds the preview's models from the avatar, then draws them.
  void UpdatePreview();
  void DrawPreview();

  EmulatorWindow* emulator_window_;
  uint64_t xuid_;
  kernel::xam::AvatarDescription avatar_;
  bool have_assets_ = false;

  std::vector<Category> categories_;
  wxOwnerDrawnComboBox* gender_choice_ = nullptr;
  wxSlider* height_slider_ = nullptr;
  wxSlider* weight_slider_ = nullptr;
  std::array<wxColourPickerCtrl*, 9> color_pickers_ = {};

  // The avatar as titles get it, drawn on the CPU; dragging turns it.
  wxPanel* preview_ = nullptr;
  wxBitmap preview_bitmap_;
  kernel::xam::AvatarAssets preview_assets_;
  float preview_yaw_ = 0.35f;
  int drag_x_ = 0;
  bool dragging_ = false;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_AVATAR_EDITOR_DIALOG_WX_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/avatar_editor_dialog_wx.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>

#include <wx/button.h>
#include <wx/clrpicker.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/odcombo.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace app {

using namespace kernel::xam;

namespace {

constexpr const char* kColorNames[] = {
    wxTRANSLATE("Skin"),        wxTRANSLATE("Hair"),
    wxTRANSLATE("Lips"),        wxTRANSLATE("Eyes"),
    wxTRANSLATE("Eyebrows"),    wxTRANSLATE("Eye shadow"),
    wxTRANSLATE("Facial hair"), wxTRANSLATE("Face paint 1"),
    wxTRANSLATE("Face paint 2")};

wxColour ToColour(uint32_t argb) {
  return wxColour((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

uint32_t FromColour(const wxColour& colour) {
  return 0xFF000000u | uint32_t(colour.Red()) << 16 |
         uint32_t(colour.Green()) << 8 | uint32_t(colour.Blue());
}

// Whether the item's slots are one of these.
bool HasSlots(const AvatarAssetPack& pack, uint32_t index,
              const std::vector<uint32_t>& slots) {
  const AvatarAssetPack::Record* record = pack.GetRecord(index);
  return record &&
         std::find(slots.begin(), slots.end(), record->slots) != slots.end();
}

}  // namespace

AvatarEditorDialog::AvatarEditorDialog(wxWindow* parent,
                                       EmulatorWindow* emulator_window,
                                       uint64_t xuid)
    : wxDialog(parent, wxID_ANY, _("Avatar Editor"), wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      emulator_window_(emulator_window),
      xuid_(xuid) {
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  auto* xam_state = kernel_state ? kernel_state->xam_state() : nullptr;
  if (xam_state && xam_state->profile_manager()) {
    if (const auto* account = xam_state->profile_manager()->GetAccount(xuid_)) {
      SetTitle(
          wxString::Format(_("Avatar Editor - %s"),
                           wxString::FromUTF8(account->GetGamertagString())));
    }
  }
  LoadAvatar();
  Build();
}

void AvatarEditorDialog::LoadAvatar() {
  have_assets_ = AvatarAssetPack::Get() != nullptr;
  if (!have_assets_) {
    return;
  }
  bool loaded = false;
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  auto* xam_state = kernel_state ? kernel_state->xam_state() : nullptr;
  auto* profile = xam_state ? xam_state->GetUserProfile(xuid_) : nullptr;
  if (profile) {
    auto setting = xam_state->user_tracker()->GetSetting(
        profile, kernel::kDashboardID,
        static_cast<uint32_t>(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1));
    if (setting) {
      // get_host_data() has no binary form; the manifest is the extended data.
      const std::span<const uint8_t> bytes = setting->get_extended_data();
      loaded = bytes.size() >= kAvatarManifestSize &&
               ReadAvatarManifest(bytes, &avatar_);
    }
  }
  if (!loaded) {
    // What titles give a profile without an avatar.
    RandomizeAvatar(xuid_, 0, &avatar_);
  }
}

wxOwnerDrawnComboBox* AvatarEditorDialog::AddCategory(
    wxWindow* parent, wxSizer* sizer, const wxString& label,
    std::vector<uint32_t> slots, bool optional) {
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  row->Add(new wxStaticText(parent, wxID_ANY, label, wxDefaultPosition,
                            wxSize(100, -1)),
           wxSizerFlags().CenterVertical().Border(wxALL, 3));
  auto* choice = new wxOwnerDrawnComboBox(parent, wxID_ANY, wxEmptyString,
                                          wxDefaultPosition, wxSize(240, -1), 0,
                                          nullptr, wxCB_READONLY);
  choice->SetPopupMaxHeight(360);
  row->Add(choice, wxSizerFlags(1).Expand().Border(wxALL, 3));
  sizer->Add(row, wxSizerFlags().Expand());
  const size_t index = categories_.size();
  choice->Bind(wxEVT_COMBOBOX,
               [this, index](wxCommandEvent&) { OnCategoryChanged(index); });
  Category category;
  category.choice = choice;
  category.slots = std::move(slots);
  category.optional = optional;
  categories_.push_back(std::move(category));
  return choice;
}

void AvatarEditorDialog::Build() {
  auto* root = new wxBoxSizer(wxVERTICAL);
  if (!have_assets_) {
    root->Add(
        new wxStaticText(
            this, wxID_ANY,
            _("No avatar assets yet. Start a game whose disc has the avatar "
              "asset pack (Kinect Sports, or a disc with a Kinect system "
              "update such as Fable: The Journey) once, install the system "
              "avatar package FFFE07DF00000002 with Install Content, or set "
              "avatar_asset_pack_path.")),
        wxSizerFlags().Border(wxALL, 12));
    root->Add(new wxButton(this, wxID_CANCEL, _("Close")),
              wxSizerFlags().Right().Border(wxALL, 8));
    SetSizerAndFit(root);
    return;
  }

  auto* columns = new wxBoxSizer(wxHORIZONTAL);

  preview_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(260, 420));
  preview_->SetBackgroundStyle(wxBG_STYLE_PAINT);
  preview_->SetToolTip(_("Drag to turn the avatar."));
  preview_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(preview_);
    dc.Clear();
    if (preview_bitmap_.IsOk()) {
      dc.DrawBitmap(preview_bitmap_, 0, 0);
    }
  });
  preview_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
    DrawPreview();
    event.Skip();
  });
  preview_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
    dragging_ = true;
    drag_x_ = event.GetX();
    preview_->CaptureMouse();
  });
  preview_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
    if (dragging_) {
      dragging_ = false;
      if (preview_->HasCapture()) {
        preview_->ReleaseMouse();
      }
    }
  });
  preview_->Bind(wxEVT_MOUSE_CAPTURE_LOST,
                 [this](wxMouseCaptureLostEvent&) { dragging_ = false; });
  preview_->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
    if (!dragging_) {
      return;
    }
    preview_yaw_ += (event.GetX() - drag_x_) * 0.012f;
    drag_x_ = event.GetX();
    DrawPreview();
  });
  columns->Add(preview_, wxSizerFlags().Expand().Border(wxALL, 8));

  auto* left = new wxBoxSizer(wxVERTICAL);

  auto* body_box = new wxStaticBox(this, wxID_ANY, _("Body"));
  auto* body_sizer = new wxStaticBoxSizer(body_box, wxVERTICAL);
  auto add_row = [](wxWindow* parent, wxSizer* sizer, const wxString& label,
                    wxWindow* widget) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(parent, wxID_ANY, label, wxDefaultPosition,
                              wxSize(100, -1)),
             wxSizerFlags().CenterVertical().Border(wxALL, 3));
    row->Add(widget, wxSizerFlags(1).Expand().Border(wxALL, 3));
    sizer->Add(row, wxSizerFlags().Expand());
  };
  gender_choice_ = new wxOwnerDrawnComboBox(body_box, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxDefaultSize, 0,
                                            nullptr, wxCB_READONLY);
  gender_choice_->Append(_("Male"));
  gender_choice_->Append(_("Female"));
  gender_choice_->Bind(wxEVT_COMBOBOX,
                       [this](wxCommandEvent&) { OnGenderChanged(); });
  add_row(body_box, body_sizer, _("Body type:"), gender_choice_);
  height_slider_ = new wxSlider(body_box, wxID_ANY, 0, -100, 100);
  height_slider_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
    avatar_.height = height_slider_->GetValue() / 100.0f;
  });
  add_row(body_box, body_sizer, _("Height:"), height_slider_);
  weight_slider_ = new wxSlider(body_box, wxID_ANY, 0, -100, 100);
  weight_slider_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
    avatar_.weight = weight_slider_->GetValue() / 100.0f;
  });
  add_row(body_box, body_sizer, _("Weight:"), weight_slider_);
  height_slider_->SetToolTip(_("Saved, but games don't show it yet."));
  weight_slider_->SetToolTip(_("Saved, but games don't show it yet."));
  left->Add(body_sizer, wxSizerFlags().Expand().Border(wxALL, 6));

  auto* face_box = new wxStaticBox(this, wxID_ANY, _("Face"));
  auto* face_sizer = new wxStaticBoxSizer(face_box, wxVERTICAL);
  AddCategory(face_box, face_sizer, _("Eyes:"), {0x2000}, false);
  AddCategory(face_box, face_sizer, _("Eyebrows:"), {0x4000}, false);
  AddCategory(face_box, face_sizer, _("Mouth:"), {0x8000}, false);
  AddCategory(face_box, face_sizer, _("Nose:"), {0x80000}, true);
  AddCategory(face_box, face_sizer, _("Chin:"), {0x100000}, true);
  AddCategory(face_box, face_sizer, _("Ears:"), {0x200000}, true);
  AddCategory(face_box, face_sizer, _("Facial hair:"), {0x10000}, true);
  AddCategory(face_box, face_sizer, _("Eye shadow:"), {0x40000}, true);
  AddCategory(face_box, face_sizer, _("Face paint:"), {0x20000}, true);
  left->Add(face_sizer, wxSizerFlags().Expand().Border(wxALL, 6));
  columns->Add(left, wxSizerFlags(1).Expand());

  auto* right = new wxBoxSizer(wxVERTICAL);
  auto* clothes_box =
      new wxStaticBox(this, wxID_ANY, _("Hair, clothes and accessories"));
  auto* clothes_sizer = new wxStaticBoxSizer(clothes_box, wxVERTICAL);
  // Full outfits occupy several slots, flagged 0x800000.
  std::vector<uint32_t> outfit_slots;
  for (const auto& record : AvatarAssetPack::Get()->records()) {
    if (record.strb_offset && record.type == 1 && (record.slots & 0x800000) &&
        std::find(outfit_slots.begin(), outfit_slots.end(), record.slots) ==
            outfit_slots.end()) {
      outfit_slots.push_back(record.slots);
    }
  }
  AddCategory(clothes_box, clothes_sizer, _("Hair:"), {0x4}, true);
  AddCategory(clothes_box, clothes_sizer, _("Hat:"), {0x40}, true);
  AddCategory(clothes_box, clothes_sizer, _("Glasses:"), {0x100}, true);
  AddCategory(clothes_box, clothes_sizer, _("Top:"), {0x8, 0x208}, true);
  AddCategory(clothes_box, clothes_sizer, _("Bottoms:"), {0x10}, true);
  AddCategory(clothes_box, clothes_sizer, _("Shoes:"), {0x20}, true);
  AddCategory(clothes_box, clothes_sizer, _("Outfit:"), outfit_slots, true);
  AddCategory(clothes_box, clothes_sizer, _("Gloves:"), {0x880}, true);
  AddCategory(clothes_box, clothes_sizer, _("Wristwear:"), {0x200}, true);
  AddCategory(clothes_box, clothes_sizer, _("Earrings:"), {0x400}, true);
  AddCategory(clothes_box, clothes_sizer, _("Rings:"), {0x800}, true);
  right->Add(clothes_sizer, wxSizerFlags().Expand().Border(wxALL, 6));

  auto* colors_box = new wxStaticBox(this, wxID_ANY, _("Colors"));
  auto* colors_sizer = new wxStaticBoxSizer(colors_box, wxVERTICAL);
  auto* colors_grid = new wxFlexGridSizer(2, wxSize(6, 2));
  for (size_t i = 0; i < color_pickers_.size(); ++i) {
    colors_grid->Add(new wxStaticText(colors_box, wxID_ANY,
                                      wxGetTranslation(kColorNames[i])),
                     wxSizerFlags().CenterVertical().Border(wxLEFT, 3));
    color_pickers_[i] = new wxColourPickerCtrl(colors_box, wxID_ANY);
    color_pickers_[i]->Bind(wxEVT_COLOURPICKER_CHANGED,
                            [this, i](wxColourPickerEvent& event) {
                              avatar_.colors[i] = FromColour(event.GetColour());
                              UpdatePreview();
                            });
    colors_grid->Add(color_pickers_[i], wxSizerFlags().Border(wxALL, 2));
  }
  colors_sizer->Add(colors_grid, wxSizerFlags().Border(wxALL, 3));
  right->Add(colors_sizer, wxSizerFlags().Expand().Border(wxALL, 6));
  columns->Add(right, wxSizerFlags(1).Expand());
  root->Add(columns, wxSizerFlags(1).Expand().Border(wxALL, 4));

  root->Add(new wxStaticText(
                this, wxID_ANY,
                _("Saved to the profile for every game. A running game shows "
                  "changes the next time it loads the avatar.")),
            wxSizerFlags().Border(wxLEFT | wxRIGHT, 10));

  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  auto* randomize = new wxButton(this, wxID_ANY, _("Randomize"));
  randomize->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnRandomize(); });
  buttons->Add(randomize, wxSizerFlags().Border(wxRIGHT, 4));
  auto* import = new wxButton(this, wxID_ANY, _("Import..."));
  import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnImport(); });
  buttons->Add(import, wxSizerFlags().Border(wxRIGHT, 4));
  auto* export_button = new wxButton(this, wxID_ANY, _("Export..."));
  export_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnExport(); });
  buttons->Add(export_button);
  buttons->AddStretchSpacer(1);
  auto* save = new wxButton(this, wxID_OK, _("Save"));
  save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSave(); });
  buttons->Add(save, wxSizerFlags().Border(wxRIGHT, 4));
  buttons->Add(new wxButton(this, wxID_CANCEL, _("Cancel")));
  root->Add(buttons, wxSizerFlags().Expand().Border(wxALL, 10));

  SetSizerAndFit(root);
  SetMinSize(GetSize());
  PopulateCategories();
  RefreshControls();
}

void AvatarEditorDialog::PopulateCategories() {
  const AvatarAssetPack* pack = AvatarAssetPack::Get();
  for (Category& category : categories_) {
    category.items = GetAvatarEditorItems(category.slots, avatar_.gender);
    category.choice->Clear();
    if (category.optional) {
      category.choice->Append(_("(None)"));
    }
    for (uint32_t index : category.items) {
      category.choice->Append(wxString::FromUTF8(pack->GetName(index)));
    }
  }
}

void AvatarEditorDialog::RefreshControls() {
  const AvatarAssetPack* pack = AvatarAssetPack::Get();
  gender_choice_->SetSelection(avatar_.gender == 2 ? 1 : 0);
  height_slider_->SetValue(
      int(std::clamp(avatar_.height, -1.0f, 1.0f) * 100.0f));
  weight_slider_->SetValue(
      int(std::clamp(avatar_.weight, -1.0f, 1.0f) * 100.0f));
  for (Category& category : categories_) {
    int selection = category.optional ? 0 : wxNOT_FOUND;
    for (uint32_t index : avatar_.items) {
      if (!HasSlots(*pack, index, category.slots)) {
        continue;
      }
      auto it = std::find(category.items.begin(), category.items.end(), index);
      if (it != category.items.end()) {
        selection = int(it - category.items.begin()) + category.optional;
      }
    }
    category.choice->SetSelection(selection);
  }
  for (size_t i = 0; i < color_pickers_.size(); ++i) {
    color_pickers_[i]->SetColour(ToColour(avatar_.colors[i]));
  }
  UpdatePreview();
}

void AvatarEditorDialog::UpdatePreview() {
  std::vector<uint8_t> manifest(kAvatarManifestSize);
  preview_assets_ = AvatarAssets();
  if (WriteAvatarManifest(avatar_, manifest)) {
    BuildAvatarAssets(manifest, 0xFFFFFFFF, &preview_assets_);
  }
  DrawPreview();
}

void AvatarEditorDialog::DrawPreview() {
  if (!preview_) {
    return;
  }
  const wxSize size = preview_->GetClientSize();
  if (size.GetWidth() < 16 || size.GetHeight() < 16) {
    return;
  }
  std::vector<uint8_t> rgb;
  RenderAvatarPreview(preview_assets_, uint32_t(size.GetWidth()),
                      uint32_t(size.GetHeight()), preview_yaw_, &rgb);
  // wxImage takes ownership of malloc'd data.
  auto* data = static_cast<unsigned char*>(std::malloc(rgb.size()));
  std::memcpy(data, rgb.data(), rgb.size());
  preview_bitmap_ = wxBitmap(wxImage(size.GetWidth(), size.GetHeight(), data));
  preview_->Refresh(false);
}

void AvatarEditorDialog::OnCategoryChanged(size_t index) {
  const AvatarAssetPack* pack = AvatarAssetPack::Get();
  Category& category = categories_[index];
  const int selection = category.choice->GetSelection();
  if (selection < 0) {
    return;
  }
  if (category.optional && selection == 0) {
    std::erase_if(avatar_.items, [&](uint32_t item) {
      return HasSlots(*pack, item, category.slots);
    });
  } else {
    const size_t item = size_t(selection) - category.optional;
    if (item < category.items.size()) {
      SetAvatarItem(&avatar_, category.items[item]);
    }
  }
  // Others may have come off (an outfit replaces the top and bottoms).
  RefreshControls();
}

void AvatarEditorDialog::OnGenderChanged() {
  const uint8_t gender = gender_choice_->GetSelection() == 1 ? 2 : 1;
  if (gender == avatar_.gender) {
    return;
  }
  const AvatarAssetPack* pack = AvatarAssetPack::Get();
  avatar_.gender = gender;
  // Items for the other body type come off; the basics are refilled.
  std::erase_if(avatar_.items, [&](uint32_t item) {
    const AvatarAssetPack::Record* record = pack->GetRecord(item);
    return !record || (record->gender != gender && record->gender != 3);
  });
  PopulateCategories();
  for (Category& category : categories_) {
    const bool basic = !category.optional ||
                       std::find(category.slots.begin(), category.slots.end(),
                                 0x10u) != category.slots.end() ||
                       std::find(category.slots.begin(), category.slots.end(),
                                 0x20u) != category.slots.end() ||
                       std::find(category.slots.begin(), category.slots.end(),
                                 0x8u) != category.slots.end();
    if (!basic || category.items.empty()) {
      continue;
    }
    // Unless something already covers those slots (an outfit).
    const AvatarAssetPack::Record* first = pack->GetRecord(category.items[0]);
    bool covered = false;
    for (uint32_t item : avatar_.items) {
      const AvatarAssetPack::Record* record = pack->GetRecord(item);
      covered = covered ||
                (record && first && (record->slots & first->slots & 0x7FFFFF));
    }
    if (!covered) {
      SetAvatarItem(&avatar_, category.items[0]);
    }
  }
  RefreshControls();
}

void AvatarEditorDialog::OnRandomize() {
  RandomizeAvatar(std::random_device()() | uint64_t(std::random_device()())
                                               << 32,
                  avatar_.gender, &avatar_);
  PopulateCategories();
  RefreshControls();
}

void AvatarEditorDialog::OnImport() {
  wxFileDialog dialog(this, _("Import avatar"), "", "",
                      _("Avatar manifests (*.bin)|*.bin|All files (*.*)|*.*"),
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dialog.ShowModal() != wxID_OK) {
    return;
  }
  const std::vector<uint8_t> bytes = xe::filesystem::ReadAllBytes(
      std::filesystem::path(dialog.GetPath().wc_str()));
  AvatarDescription avatar;
  if (bytes.size() != kAvatarManifestSize ||
      !ReadAvatarManifest(bytes, &avatar)) {
    wxMessageBox(_("That isn't an avatar manifest (a 1000-byte file)."),
                 _("Import avatar"), wxOK | wxICON_WARNING, this);
    return;
  }
  avatar_ = std::move(avatar);
  PopulateCategories();
  RefreshControls();
}

void AvatarEditorDialog::OnExport() {
  wxFileDialog dialog(this, _("Export avatar"), "", "avatar.bin",
                      _("Avatar manifests (*.bin)|*.bin"),
                      wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (dialog.ShowModal() != wxID_OK) {
    return;
  }
  std::vector<uint8_t> manifest(kAvatarManifestSize);
  FILE* file = nullptr;
  if (WriteAvatarManifest(avatar_, manifest)) {
    file = xe::filesystem::OpenFile(
        std::filesystem::path(dialog.GetPath().wc_str()), "wb");
  }
  const bool written = file && std::fwrite(manifest.data(), 1, manifest.size(),
                                           file) == manifest.size();
  if (file) {
    std::fclose(file);
  }
  if (!written) {
    wxMessageBox(_("Couldn't write the file."), _("Export avatar"),
                 wxOK | wxICON_WARNING, this);
  }
}

void AvatarEditorDialog::OnSave() {
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  auto* xam_state = kernel_state ? kernel_state->xam_state() : nullptr;
  std::vector<uint8_t> manifest(kAvatarManifestSize);
  if (!xam_state || !WriteAvatarManifest(avatar_, manifest)) {
    EndModal(wxID_CANCEL);
    return;
  }
  // Where XamAvatarSetManifest saves it, which every title reads.
  const UserSetting setting(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1,
                            std::move(manifest));
  xam_state->user_tracker()->UpsertSetting(xuid_, kernel::kDashboardID,
                                           &setting);
  XELOGI("Avatars: saved the avatar of profile {:016X}", xuid_);
  const uint32_t user_index =
      xam_state->profile_manager()->GetUserIndexAssignedToProfile(xuid_);
  if (user_index != XUserIndexAny) {
    kernel_state->BroadcastNotification(kXNotificationSystemAvatarChanged,
                                        1u << user_index);
    kernel_state->BroadcastNotification(
        kXNotificationSystemProfileSettingChanged, 1u << user_index);
  }
  EndModal(wxID_OK);
}

}  // namespace app
}  // namespace xe

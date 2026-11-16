// A picture the overlay keeps for the whole session (the practice menu's game font, a room's
// portraits, the character select's extra) while the overlay itself can be rebuilt: Direct3D 12
// rebuilds it, with a new ImGui context, when HDR output turns on or off.
//
// ImGui's renderers free a registered texture on teardown only while one context holds it
// (RefCount == 1). A texture registered again in each new context without being unregistered from
// the old one counted 2 after the first rebuild, so the next teardown left it alone: it kept the old
// renderer's handle, and the new overlay drew it as nothing (the practice menu's bars came up with
// no words on them). Keep such a texture with keep_user_texture() once a frame before drawing it:
// a new context gets a fresh registration and a fresh upload, and the count stays at one.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"
#include "imgui_internal.h"

namespace gx {

inline void keep_user_texture(ImTextureData* tex, ImGuiContext*& registered_in) {
  ImGuiContext* now = ImGui::GetCurrentContext();
  if (!tex || !now) return;
  if (registered_in != now) {
    registered_in = now;
    tex->RefCount = 0;   // the old registration belonged to a context that no longer exists
    if (tex->Status != ImTextureStatus_WantCreate) {
      // Whatever GPU copy it had went with the old renderer (freed there, as the count was one).
      tex->SetTexID(ImTextureID_Invalid);
      tex->BackendUserData = nullptr;
      tex->SetStatus(ImTextureStatus_WantCreate);
    }
    ImGui::RegisterUserTexture(tex);
  } else if (tex->Status == ImTextureStatus_Destroyed) {
    tex->SetStatus(ImTextureStatus_WantCreate);   // a renderer reset inside the same context
  }
}

}  // namespace gx

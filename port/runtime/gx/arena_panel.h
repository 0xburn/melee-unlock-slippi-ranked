// The arena room in the black bars beside the game picture, on the online character select
// (hle/slippi_arena.h). Controller only: the C-stick opens the room menu, which then takes the
// buttons (X ask / give up / get in line, Y rules, Z add a friend, R doubles, A answer, B close)
// while the game keeps X, Y, L and R for costumes and skins the rest of the time.
//
// Looks: the combined one (corners on top, the line and the stands under them) is the default;
// Card, Split, Corners and Ring are the others. Every look slides in from the sides when the
// character select comes back, and can be folded into small tabs at the edges that open on hover.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace arena_panel {

enum class Look : uint8_t { Combined = 0, Card = 1, Split = 2, Corners = 3, Ring = 4 };
constexpr int kLookCount = 5;

struct Frame {
  float display_w = 0, display_h = 0;
  float game_x0 = 0, game_y0 = 0, game_x1 = 0, game_y1 = 0;   // the game picture in the window
  uint16_t buttons = 0;          // GameCube buttons of the player's controller
  int8_t stick_x = 0, stick_y = 0, sub_x = 0, sub_y = 0;
  double time = 0;               // seconds
  bool on_online_menus = false;  // the online character select (or waiting there)
};

// UI thread, once per overlay frame, inside ImGui. Draws nothing outside the online menus.
void frame(const Frame& f);
// True while the room menu has the controller (the game must not see those presses).
bool wants_capture();

}  // namespace arena_panel

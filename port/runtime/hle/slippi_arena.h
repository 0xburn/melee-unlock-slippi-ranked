// Arena rooms for Slippi Direct: friends beyond the two players watch the match live, in a viewer
// window of their own, and rotate in by the room's rules (arena_rules.h).
//
// Every member keeps one room link to the room's host (the member who made the room). A link is a
// Slippi netplay connection of its own, on its own socket, made the way a Direct match connects; it
// carries only room messages (NP_MSG_MU_ARENA). The two fighters still play over their own Direct
// connection exactly as in a plain Direct game: nobody watching is ever on it.
//
// Between games the room starts the Direct search between the two friends it put in the ring, from
// the online character select; the players pick their characters and press Start as usual. During a
// game the first fighter (ring slot 0) sends the live feed (arena_feed.h) to the host, which passes it to
// everyone watching; each watcher's game writes it as a recording still being written and opens a
// viewer window on it.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "arena_rules.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace slippi::arena {

// ---- setup (before the game starts) ----
// Test rooms with no Slippi servers: --arena-test <index>/<count>/<base port>[/<character>]. Member 0
// hosts; each member i links to it on loopback (host side base+100+i, member side base+200+i) and
// plays its matches on base+300+20*(games%10)+i. Every member is locked in with <character> (default Fox) as soon
// as it is connected, since nobody presses Start in a test.
bool set_test(const std::string& spec);
bool test_mode();
// The online recording's code list (a watcher's live recording needs it; the feed does not carry it).
void set_recording_codes(std::vector<uint8_t> codes);
// Opens a viewer window on a live recording; returns its process handle, or null. Set by the host.
void set_viewer_launcher(std::function<void*(const std::string& path)> launch);

// ---- simulation thread ----
void tick();                                                   // once a frame, any scene
void recording_event(const uint8_t* data, uint32_t size);     // the local recording, command byte first
void shutdown();

// ---- the panel (UI thread) ----
enum class Act : uint8_t {
  ToggleLine = 1, Ask = 2, Answer = 3, GiveUp = 4, SetMode = 5, ProposeSwitch = 6, VoteSwitch = 7, Leave = 8,
  OpenRoom = 20, AddCode = 21,   // the host's own (AddCode carries a connect code)
};
void act(Act a, uint8_t arg = 0, const std::string& text = {});

struct View {
  bool active = false;      // in a room
  bool hosting = false;
  bool linked = false;      // a member: the link to the host is up
  uint8_t self = kNone;
  Room room;
  std::array<int, kMaxMembers> ping_ms{};   // each member's ping to the host, -1 unknown, 0 the host
  bool watching_live = false;               // a viewer window is open on this game
  std::string status;                       // one plain line: what this member's game is doing
};
View view();
// True while this game is in a room (any thread).
bool in_room();

// ---- tests ----
// The member's last few log-worthy events, for hidden runs (also in the log as "arena: ...").
std::vector<std::string> recent_events();

}  // namespace slippi::arena

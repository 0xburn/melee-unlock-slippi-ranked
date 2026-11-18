// Warm-up room: between matches with someone the player is already connected to (Direct, Teams), or
// while waiting for their turn in an arena room, the player goes to Training and the connection
// stays up. Back on the online character select the pair plays on as if nobody had left.
//
// This file is the room's own logic: plain data and plain functions, no sockets, no clock, no game
// state, so it is unit tested (port/tests/warmup_test.cpp). The coordinator (native_practice.cpp)
// feeds it what the game and the link say each frame and carries out what it answers.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace slippi::warmup {

constexpr int kMaxPeers = 3;   // Teams: three others

enum class Kind : uint8_t {
  Pair,    // connected to an opponent (Direct, Teams): the connection is parked meanwhile
  Arena,   // in an arena room, not fighting: the room's link stays up on its own
};
enum class Stage : uint8_t {
  Off,
  Going,       // asked to go; the game is on its way into Training
  Training,    // in Training (its character select, stage select or match)
  Returning,   // asked to go back; the game is on its way to the online character select
};
enum class PeerState : uint8_t {
  Picking,     // on the character select, not locked in
  Ready,       // locked in, waiting
  WarmingUp,   // warming up too (only builds that say so)
  Left,        // gone: the connection lost them
};
enum class Banner : uint8_t { None, PeerReady, PeerLeft, PeerWarming, YourTurn, PeerBack };
enum class Action : uint8_t {
  None,
  GoBack,     // start the way back now (the room's turn came, or its countdown ran out)
  Ready,      // a pair: everyone else just locked in: go back at once and lock in for the match
  Arrived,    // the online character select is up again: the warm-up is over
  GiveUp,     // the game never got where it was sent: end the warm-up where it is
  Abandoned,  // the player left Training for somewhere else (the menus): the warm-up is over
};

// What the coordinator saw this frame.
struct Observe {
  uint64_t now_ms = 0;
  bool in_training = false;     // Training's major scene (its selects or its match)
  bool online_select = false;   // the online character select is polling the link
  // Pair: the connection still has everyone; each opponent's state (only the first `peers` count).
  bool link_up = true;
  int peers = 0;
  PeerState peer[kMaxPeers] = {PeerState::Picking, PeerState::Picking, PeerState::Picking};
  // Arena: the room put this player in the ring for the next game.
  bool turn = false;
};

class Room {
 public:
  // Off -> Going. False when a warm-up is already on.
  bool enter(Kind kind, uint64_t now_ms);
  // The player asks to go back (a hold, the button, the menu row, a press during the countdown).
  // False when there is nothing to go back from.
  bool request_return(uint64_t now_ms);
  // The arena's call: how long the "your turn" banner counts down before the game goes back by
  // itself (0: at once).
  void set_countdown_ms(uint32_t ms) { countdown_ms_ = ms; }
  // A pair: when every opponent locks in while the player is in Training, the room answers Ready (on
  // by default). Opponents already locked in when the player left do not count: that was a choice.
  void set_back_when_ready(bool on) { back_when_ready_ = on; }
  Action tick(const Observe& o);
  void reset() { *this = Room{}; }

  Stage stage() const { return stage_; }
  Kind kind() const { return kind_; }
  bool active() const { return stage_ != Stage::Off; }
  Banner banner() const { return banner_; }
  uint64_t banner_since_ms() const { return banner_since_; }
  int banner_peer() const { return banner_peer_; }
  // Milliseconds left on the turn countdown, or -1 when none runs.
  int countdown_left_ms(uint64_t now_ms) const;
  int peers() const { return peers_; }
  PeerState peer(int i) const { return i >= 0 && i < kMaxPeers ? peer_[i] : PeerState::Left; }
  bool anyone_left() const;
  uint64_t entered_ms() const { return entered_ms_; }

  // Limits, public for the tests.
  static constexpr uint64_t kGoingTimeoutMs = 15000;     // Training never came up
  static constexpr uint64_t kReturnTimeoutMs = 12000;    // the character select never came back
  static constexpr uint64_t kElsewhereMs = 2000;         // out of Training and not on the select this long

 private:
  void show(Banner b, uint64_t now_ms, int peer = -1);
  Stage stage_ = Stage::Off;
  Kind kind_ = Kind::Pair;
  uint64_t stage_since_ = 0, entered_ms_ = 0, elsewhere_since_ = 0;
  Banner banner_ = Banner::None;
  uint64_t banner_since_ = 0;
  int banner_peer_ = -1;
  int peers_ = 0;
  PeerState peer_[kMaxPeers] = {PeerState::Picking, PeerState::Picking, PeerState::Picking};
  bool seen_peers_ = false;
  bool turn_ = false;
  uint32_t countdown_ms_ = 3000;
  bool back_when_ready_ = true;
  bool all_ready_ = false;   // every opponent locked in, as of the last frame
  uint64_t deadline_ = 0;   // the turn countdown's end, 0 none
};

const char* stage_name(Stage s);
const char* peer_state_name(PeerState s);

}  // namespace slippi::warmup

// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_warmup.h"

namespace slippi::warmup {

bool Room::enter(Kind kind, uint64_t now_ms) {
  if (stage_ != Stage::Off) return false;
  const uint32_t countdown = countdown_ms_;
  const bool back_when_ready = back_when_ready_;
  *this = Room{};
  countdown_ms_ = countdown;
  back_when_ready_ = back_when_ready;
  kind_ = kind;
  stage_ = Stage::Going;
  stage_since_ = entered_ms_ = now_ms;
  return true;
}

bool Room::request_return(uint64_t now_ms) {
  if (stage_ != Stage::Going && stage_ != Stage::Training) return false;
  stage_ = Stage::Returning;
  stage_since_ = now_ms;
  deadline_ = 0;
  return true;
}

int Room::countdown_left_ms(uint64_t now_ms) const {
  if (!deadline_ || stage_ != Stage::Training) return -1;
  return now_ms >= deadline_ ? 0 : (int)(deadline_ - now_ms);
}

bool Room::anyone_left() const {
  for (int i = 0; i < peers_; ++i) if (peer_[i] == PeerState::Left) return true;
  return false;
}

void Room::show(Banner b, uint64_t now_ms, int peer) {
  banner_ = b;
  banner_since_ = now_ms;
  banner_peer_ = peer;
}

Action Room::tick(const Observe& o) {
  if (stage_ == Stage::Off) return Action::None;

  // The opponents, whatever the stage: a change is a banner while the player is in Training.
  if (kind_ == Kind::Pair) {
    const int n = o.peers < 0 ? 0 : o.peers > kMaxPeers ? kMaxPeers : o.peers;
    if (n > peers_) peers_ = n;
    for (int i = 0; i < peers_; ++i) {
      // Once gone, gone: a link that lost a player never gets them back.
      const PeerState now = !o.link_up || i >= n ? PeerState::Left : o.peer[i];
      const PeerState was = peer_[i];
      if (was == PeerState::Left) continue;
      peer_[i] = now;
      if (!seen_peers_ || now == was || stage_ != Stage::Training) continue;
      if (now == PeerState::Left) show(Banner::PeerLeft, o.now_ms, i);
      else if (now == PeerState::Ready) show(Banner::PeerReady, o.now_ms, i);
      else if (now == PeerState::WarmingUp) show(Banner::PeerWarming, o.now_ms, i);
      else if (was == PeerState::WarmingUp) show(Banner::PeerBack, o.now_ms, i);
    }
    seen_peers_ = true;
  }
  // Everyone else locked in just now (not already when the player left): the match is waiting.
  bool became_ready = false;
  if (kind_ == Kind::Pair) {
    bool all = peers_ > 0;
    for (int i = 0; i < peers_; ++i) if (peer_[i] != PeerState::Ready) all = false;
    became_ready = all && !all_ready_ && stage_ == Stage::Training;
    all_ready_ = all;
  }

  switch (stage_) {
    case Stage::Going:
      if (o.in_training) {
        stage_ = Stage::Training;
        stage_since_ = o.now_ms;
        elsewhere_since_ = 0;
        // (Locked in already on the way here: that was before the player was in Training, no news.)
        // A turn already called (the room moved on while the game was on its way): count from now.
        if (kind_ == Kind::Arena && o.turn) {
          turn_ = true;
          deadline_ = o.now_ms + countdown_ms_;
          show(Banner::YourTurn, o.now_ms);
          if (!countdown_ms_) {
            stage_ = Stage::Returning;
            deadline_ = 0;
            return Action::GoBack;
          }
        }
        return Action::None;
      }
      if (o.now_ms - stage_since_ > kGoingTimeoutMs) {
        stage_ = Stage::Off;
        return Action::GiveUp;
      }
      return Action::None;

    case Stage::Training: {
      if (o.online_select) {   // back on the select by some other way (the game's own menus)
        stage_ = Stage::Off;
        return Action::Arrived;
      }
      if (!o.in_training) {
        if (!elsewhere_since_) elsewhere_since_ = o.now_ms ? o.now_ms : 1;
        if (o.now_ms - elsewhere_since_ >= kElsewhereMs) {
          stage_ = Stage::Off;
          return Action::Abandoned;
        }
      } else {
        elsewhere_since_ = 0;
      }
      if (became_ready && back_when_ready_) {
        stage_ = Stage::Returning;
        stage_since_ = o.now_ms;
        return Action::Ready;
      }
      if (kind_ == Kind::Arena) {
        if (o.turn && !turn_) {
          turn_ = true;
          deadline_ = o.now_ms + countdown_ms_;
          show(Banner::YourTurn, o.now_ms);
        } else if (!o.turn && turn_) {   // the room changed its mind (the one ahead came back)
          turn_ = false;
          deadline_ = 0;
          if (banner_ == Banner::YourTurn) show(Banner::None, o.now_ms);
        }
        if (deadline_ && o.now_ms >= deadline_) {
          stage_ = Stage::Returning;
          stage_since_ = o.now_ms;
          deadline_ = 0;
          return Action::GoBack;
        }
      }
      return Action::None;
    }

    case Stage::Returning:
      if (o.online_select) {
        stage_ = Stage::Off;
        return Action::Arrived;
      }
      if (o.now_ms - stage_since_ > kReturnTimeoutMs) {
        stage_ = Stage::Off;
        return Action::GiveUp;
      }
      return Action::None;

    case Stage::Off: break;
  }
  return Action::None;
}

const char* stage_name(Stage s) {
  switch (s) {
    case Stage::Off: return "off";
    case Stage::Going: return "going";
    case Stage::Training: return "training";
    case Stage::Returning: return "returning";
  }
  return "?";
}

const char* peer_state_name(PeerState s) {
  switch (s) {
    case PeerState::Picking: return "picking";
    case PeerState::Ready: return "ready";
    case PeerState::WarmingUp: return "warming up";
    case PeerState::Left: return "left";
  }
  return "?";
}

}  // namespace slippi::warmup

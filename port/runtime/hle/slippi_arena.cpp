// Arena rooms for Slippi Direct (see slippi_arena.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_arena.h"
#include "arena_feed.h"
#include "slippi_net.h"
#include "slippi_online.h"
#include "host.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace slippi::arena {
namespace feed = slippi::arena_feed;
void watch_feed(const uint8_t* data, size_t size);

namespace {

// Room message kinds (first byte of an NP_MSG_MU_ARENA payload).
enum Msg : uint8_t { kHello = 1, kWelcome = 2, kRoomState = 3, kAction = 4, kReport = 5, kPings = 6, kFeed = 7 };
enum Report : uint8_t { kStarted = 1, kEnded = 2 };
constexpr uint64_t kRetryMs = 1500;

struct Link {
  std::unique_ptr<NetplayClient> np;
  std::unique_ptr<Matchmaking> mm;   // a real room: the Direct-style search that makes this link
  std::string code;            // a real room: the friend at the other end
  uint8_t id = kNone;          // the member at the other end (host side), kNone until its hello
  int test_peer = -1;          // test rooms: the other member's index
  uint64_t retry_at = 0;
  uint64_t ready_at = 0;       // when it came up: a link up this long with no hello answered is reopened
  bool was_ready = false, hello_sent = false;
};

template <class T> void drop_later(std::unique_ptr<T>& p) {
  if (p) std::thread([q = std::move(p)]() mutable { q.reset(); }).detach();   // their threads join off this one
}

struct Test { bool on = false; int index = 0, count = 0, base = 0; uint8_t character = 2; } g_test;

// Simulation thread.
bool g_active = false, g_hosting = false;
std::string g_code, g_name;
uint8_t g_self = kNone;
Room g_room;
uint32_t g_sent_rev = 0xFFFFFFFFu;
std::vector<Link> g_links;                 // host: one per member; member: the one to the host
std::vector<uint8_t> g_codes;
std::function<void*(const std::string&)> g_launch_viewer;
feed::Sender g_sender;
bool g_sending = false;                    // this game's recording goes out as the feed
feed::Receiver g_receiver;
uint32_t g_receiver_tag = 0;
bool g_viewer_open_for_tag = false;
HANDLE g_viewer = nullptr;
uint32_t g_ended_tag = 0, g_started_tag = 0;   // the host's: reports already applied
uint32_t g_my_tag = 0;                    // this game's own recording
bool g_my_online = false;                 // and it is an online match
std::string g_my_opp;                     // the opponent it is against (the room may move on first)
bool g_my_doubles = false;                // and it was a doubles game
uint8_t g_port_team[4] = {0, 0, 0, 0};    // each port's team in it (Game Start)
std::vector<uint8_t> g_table;             // its command table event, for a sender made at Game Start
bool g_wait_room = false;                 // a member's game ended: wait for the room to say what is next
void* g_yield_to = nullptr;               // this room joins the room at the other end of that link
uint64_t g_next_match_ms = 0, g_next_pings_ms = 0;
std::string g_pair_code;                   // the opponent this game last searched for
std::array<int, kMaxMembers> g_ping{};
std::string g_status;
bool g_live = false;

// UI thread hand-off.
std::mutex g_mutex;
View g_view;
struct Pending { Act a; uint8_t arg; std::string text; };
std::deque<Pending> g_pending;
std::deque<std::string> g_events;

uint64_t now_ms() { return GetTickCount64(); }

void event(const char* fmt, ...) {
  char line[300];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  host::log("arena: %s", line);
  std::lock_guard<std::mutex> lk(g_mutex);
  g_events.push_back(line);
  while (g_events.size() > 64) g_events.pop_front();
}

void put_str(std::vector<uint8_t>& o, const std::string& s) {
  const size_t n = std::min<size_t>(s.size(), 64);
  o.push_back((uint8_t)n);
  o.insert(o.end(), s.begin(), s.begin() + n);
}
bool get_str(const std::vector<uint8_t>& m, size_t& at, std::string& out) {
  if (at >= m.size()) return false;
  const size_t n = m[at++];
  if (at + n > m.size()) return false;
  out.assign((const char*)m.data() + at, n);
  at += n;
  return true;
}
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s)); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool send(Link& link, const std::vector<uint8_t>& m, bool bulk = false) {
  return link.np && link.np->SendArena(m.data(), m.size(), bulk);
}

Link* link_of(uint8_t id) {
  for (Link& l : g_links) if (l.id == id) return &l;
  return nullptr;
}

// ---- links ----
std::unique_ptr<NetplayClient> test_link(int peer) {
  if (!enet_ready()) return nullptr;
  const bool host_side = g_test.index == 0;
  const int member = host_side ? peer : g_test.index;
  const uint16_t mine = (uint16_t)(g_test.base + (host_side ? 100 : 200) + member);
  const uint16_t theirs = (uint16_t)(g_test.base + (host_side ? 200 : 100) + member);
  return std::make_unique<NetplayClient>(std::vector<std::string>{"127.0.0.1"}, std::vector<uint16_t>{theirs}, 1, mine,
                                         host_side, (uint8_t)(host_side ? 0 : 1));
}

void open_links() {
  g_links.clear();
  if (!g_test.on) return;
  if (g_test.index == 0) {
    for (int i = 1; i < g_test.count; ++i) { Link l; l.test_peer = i; g_links.push_back(std::move(l)); }
  } else {
    Link l; l.test_peer = 0; g_links.push_back(std::move(l));
  }
}

// ---- the room (host side) ----
void broadcast_room() {
  if (!g_hosting || g_room.rev == g_sent_rev) return;
  g_sent_rev = g_room.rev;
  std::vector<uint8_t> m = {kRoomState};
  const std::vector<uint8_t> bytes = encode(g_room);
  m.insert(m.end(), bytes.begin(), bytes.end());
  for (Link& l : g_links) if (l.id != kNone) send(l, m);
  if (g_room.doubles)
    event("room rev %u: doubles %s, ring %d %d vs %d %d, line %zu, games %u", g_room.rev,
          g_room.mode == Mode::WinnerStays ? "winner stays" : g_room.mode == Mode::TakeTurns ? "take turns" : "ask",
          (int)g_room.ring[0], (int)g_room.ring[1], (int)g_room.ring[2], (int)g_room.ring[3], g_room.line.size(), g_room.games);
  else
    event("room rev %u: %s, ring %d %d, line %zu, games %u", g_room.rev,
          g_room.mode == Mode::WinnerStays ? "winner stays" : g_room.mode == Mode::TakeTurns ? "take turns" : "ask",
          (int)g_room.ring[0], (int)g_room.ring[1], g_room.line.size(), g_room.games);
}

bool apply_action(uint8_t who, Act a, uint8_t arg) {
  switch (a) {
    case Act::ToggleLine: return toggle_line(g_room, who);
    case Act::Ask: return ask(g_room, who);
    case Act::Answer: return answer(g_room, who, (Answer)arg);
    case Act::GiveUp: return give_up(g_room, who, arg != 0);
    case Act::SetMode: return set_mode(g_room, (Mode)arg);
    case Act::ProposeSwitch: return propose_switch(g_room, who);
    case Act::VoteSwitch: return vote_switch(g_room, who, arg != 0);
    case Act::Leave: {
      if (g_room.in_game && ring_slot(g_room, who) >= 0) game_end(g_room, kNone);
      return leave(g_room, who);
    }
    default: return false;
  }
}

void member_gone(uint8_t id) {
  if (!find(g_room, id)) return;
  event("member %u left", id);
  apply_action(id, Act::Leave, 0);
}

// A fighter's report of its game: the first report of each game counts.
void apply_report(uint8_t what, uint32_t tag, const std::string& winner_code) {
  if (what == kStarted) {
    if (tag == g_ended_tag || tag == g_started_tag) return;
    g_started_tag = tag;
    if (game_start(g_room)) event("game %08X started", tag);
    return;
  }
  if (what != kEnded || tag == g_ended_tag) return;
  g_ended_tag = tag;
  if (!g_room.in_game) game_start(g_room);   // the start report was lost or late
  uint8_t team = kNone;
  for (const Member& m : g_room.members)
    if (!winner_code.empty() && m.code == winner_code) {
      const int slot = ring_slot(g_room, m.id);
      if (slot >= 0) team = (uint8_t)team_of_slot(g_room, slot);
    }
  game_end(g_room, team);
  event("game %08X ended, winner %s", tag, winner_code.empty() ? "none" : winner_code.c_str());
}

void host_message(Link& from, const std::vector<uint8_t>& m) {
  if (m.empty()) return;
  switch (m[0]) {
    case kHello: {
      size_t at = 1;
      std::string code, name;
      if (!get_str(m, at, code) || !get_str(m, at, name) || from.id != kNone) return;
      const bool they_host = at + 2 <= m.size() && m[at] != 0;
      const size_t their_count = at + 2 <= m.size() ? m[at + 1] : 1;
      if (they_host && !g_test.on) {
        // Two rooms met: the bigger one keeps its host; between equals, the lower connect code hosts.
        const size_t mine = g_room.members.size();
        if (their_count > mine || (their_count == mine && code < g_code)) {
          g_yield_to = (void*)&from;   // done once this link's messages are read (the list may not change mid-loop)
          return;
        }
      }
      const uint8_t id = join(g_room, code, name);
      from.id = id;
      std::vector<uint8_t> w = {kWelcome, id};
      send(from, w);
      g_sent_rev = 0xFFFFFFFFu;   // everyone gets the room with the new member
      event(id == kNone ? "refused %s (room full or already in it)" : "%s joined as %u", code.c_str(), id);
      return;
    }
    case kAction:
      if (from.id != kNone && m.size() >= 3) apply_action(from.id, (Act)m[1], m[2]);
      return;
    case kReport: {
      // Only the players of the game report it.
      if (m.size() < 6 || from.id == kNone || ring_slot(g_room, from.id) < 0) return;
      size_t at = 6;
      std::string winner;
      get_str(m, at, winner);
      apply_report(m[1], get32(&m[2]), winner);
      return;
    }
    case kFeed: {
      // From the first player only; passed on to everyone watching (not the fighters), and kept
      // here when the host watches.
      if (from.id == kNone || from.id != g_room.ring[0]) return;
      for (Link& l : g_links)
        if (&l != &from && l.id != kNone && ring_slot(g_room, l.id) < 0) send(l, m, true);
      if (ring_slot(g_room, g_self) < 0) watch_feed(m.data() + 1, m.size() - 1);
      return;
    }
    default:
      return;
  }
}

// ---- watching (any member who is not fighting) ----
void close_viewer() {
  if (!g_viewer) return;
  DWORD code = 0;
  if (GetExitCodeProcess(g_viewer, &code) && code == STILL_ACTIVE) TerminateProcess(g_viewer, 0);   // the window we opened
  CloseHandle(g_viewer);
  g_viewer = nullptr;
}

std::string live_path(uint32_t tag) {
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::u8path(online::config().user_dir) / "arena";
  std::filesystem::create_directories(dir, ec);
  // Old live recordings of this room's earlier games are removed as the next one starts.
  static const bool keep = [] { const char* v = std::getenv("MELEE_ARENA_KEEP_LIVE"); return v && *v == '1'; }();   // tests
  for (const auto& f : std::filesystem::directory_iterator(dir, ec))
    if (!keep && f.path().filename().u8string().rfind("live-", 0) == 0) std::filesystem::remove(f.path(), ec);
  char name[32];
  std::snprintf(name, sizeof name, "live-%08X.slp", tag);
  return (dir / name).u8string();
}

}  // namespace

void watch_feed(const uint8_t* data, size_t size) {
  if (size < 5) return;
  const uint32_t tag = get32(data + 1);
  if (data[0] == feed::kStart && tag != g_receiver_tag) {
    g_receiver.close();
    close_viewer();
    g_receiver_tag = tag;
    g_viewer_open_for_tag = false;
    if (!g_receiver.open(live_path(tag), g_codes, tag)) { event("cannot write the live recording: %s", g_receiver.error().c_str()); return; }
    event("watching game %08X", tag);
  }
  if (tag != g_receiver_tag) return;
  if (!g_receiver.message(0, data, (uint32_t)size)) event("feed message refused: %s", g_receiver.error().c_str());
  // The viewer opens once the recording has its first frames (it reads the players from them).
  // Tests with many members (MELEE_ARENA_NO_VIEWER=1): the live recording only, no viewer window.
  static const bool no_viewer = [] { const char* v = std::getenv("MELEE_ARENA_NO_VIEWER"); return v && *v == '1'; }();
  if (!g_viewer_open_for_tag && g_receiver.stats().frames >= 2 && g_launch_viewer && !no_viewer) {
    g_viewer_open_for_tag = true;
    char name[32];
    std::snprintf(name, sizeof name, "live-%08X.slp", tag);
    const std::string path = (std::filesystem::u8path(online::config().user_dir) / "arena" / name).u8string();
    g_viewer = (HANDLE)g_launch_viewer(path);
    event(g_viewer ? "viewer opened on %s" : "viewer did not open on %s", name);
  }
  if (g_receiver.ended()) {
    const auto& s = g_receiver.stats();
    event("game %08X over: %u frames written, %u duplicates, %u gaps", tag, s.frames, s.duplicates, s.gaps);
  }
}

namespace {

void member_message(const std::vector<uint8_t>& m) {
  if (m.empty()) return;
  switch (m[0]) {
    case kWelcome:
      if (m.size() >= 2) {
        g_self = m[1];
        event(g_self == kNone ? "the room refused this member" : "in the room as member %u", g_self);
      }
      return;
    case kRoomState: {
      Room r;
      if (!decode(m.data() + 1, m.size() - 1, r)) { event("room message refused"); return; }
      if (r.rev != g_room.rev) {
        g_room = r;
        if (!r.in_game) g_wait_room = false;
        event("room rev %u: ring %d %d, line %zu, games %u%s", r.rev, (int)r.ring[0], (int)r.ring[1], r.line.size(), r.games,
              ring_slot(r, g_self) >= 0 ? " (fighting)" : "");
      }
      return;
    }
    case kPings:
      for (size_t k = 0; m.size() >= 2 && k < m[1] && 2 + 3 * k + 3 <= m.size(); ++k) {
        const size_t at = 2 + 3 * k;
        if (m[at] < kMaxMembers) g_ping[m[at]] = (int16_t)(m[at + 1] << 8 | m[at + 2]);
      }
      return;
    case kFeed:
      watch_feed(m.data() + 1, m.size() - 1);
      return;
    default:
      return;
  }
}

void send_to_host(const std::vector<uint8_t>& m, bool bulk = false) {
  if (!g_hosting && !g_links.empty()) send(g_links[0], m, bulk);
}

void open_room() {
  g_active = true;
  g_hosting = true;
  g_room = Room{};
  g_self = join(g_room, g_code, g_name);
  g_ping.fill(-1);
  event("room opened by %s", g_code.c_str());
}

void do_act(const Pending& p) {
  if (p.a == Act::OpenRoom) {
    if (!g_active && !g_code.empty()) open_room();
    return;
  }
  if (p.a == Act::AddCode) {
    // Adding a friend: a player not in a room opens one. Only the host searches; a member's room
    // grows through the host (friends enter the host's code).
    if (g_code.empty() || p.text.empty() || p.text == g_code || g_test.on) return;
    if (!g_active) open_room();
    if (!g_hosting) { event("only the host adds friends"); return; }
    for (const Link& l : g_links) if (l.code == p.text) return;
    for (const Member& m : g_room.members) if (m.code == p.text) return;
    Link l;
    l.code = p.text;
    g_links.push_back(std::move(l));
    event("adding %s", p.text.c_str());
    return;
  }
  if (!g_active) return;
  if (g_hosting) {
    if (apply_action(g_self, p.a, p.arg)) event("action %d by the host", (int)p.a);
    return;
  }
  std::vector<uint8_t> m = {kAction, (uint8_t)p.a, p.arg};
  send_to_host(m);
}

// ---- links: keep them up, read them ----
void service_links() {
  const uint64_t now = now_ms();
  for (Link& l : g_links) {
    if (!l.np && l.test_peer >= 0) {
      if (now < l.retry_at) continue;
      l.np = test_link(l.test_peer);
      l.was_ready = false;
      l.hello_sent = false;
      l.retry_at = now + kRetryMs;
      continue;
    }
    if (!l.np) {
      // A real room: search for the friend as Slippi Direct does; they enter this player's code too.
      if (!l.mm) {
        if (now < l.retry_at) continue;
        l.mm = online::arena_matchmaking();
        if (!l.mm) { l.retry_at = now + 5000; continue; }
        Matchmaking::MatchSearchSettings search;
        search.mode = Matchmaking::DIRECT;
        search.connect_code = utf8_to_shiftjis(l.code);
        l.mm->SetLocalPort((uint16_t)(51000 + (now * 7919) % 9000));
        l.mm->FindMatch(search);
        event("looking for %s", l.code.c_str());
        continue;
      }
      const auto state = l.mm->GetMatchmakeState();
      if (state == Matchmaking::CONNECTION_SUCCESS) {
        l.np = l.mm->GetNetplayClient();
        drop_later(l.mm);
        l.was_ready = false;
        l.hello_sent = false;
      } else if (state == Matchmaking::ERROR_ENCOUNTERED) {
        event("search for %s: %s", l.code.c_str(), l.mm->GetErrorMessage().c_str());
        drop_later(l.mm);
        l.retry_at = now + 3000;
      }
      continue;
    }
    const auto st = l.np->GetSlippiConnectStatus();
    if (st == NetplayClient::ConnectStatus::FAILED || st == NetplayClient::ConnectStatus::DISCONNECTED) {
      if (l.was_ready) {
        event("link to %s lost", g_hosting ? "a member" : "the host");
        if (g_hosting) member_gone(l.id);
        else { g_room = Room{}; g_self = kNone; }
        if (!g_hosting && !g_test.on) {
          // A real room whose host has gone is over for this member (friends can start a new one).
          event("the room's host left: the room is closed here");
          drop_later(l.np);
          g_links.clear();
          g_active = false;
          g_status.clear();
          return;
        }
      }
      l.id = g_hosting ? kNone : l.id;
      drop_later(l.np);
      l.retry_at = now + kRetryMs;
      continue;
    }
    if (!l.np->ArenaReady()) continue;
    if (!l.was_ready) {
      l.was_ready = true;
      l.ready_at = now;
      event("link to %s up", g_hosting ? "a member" : "the host");
    }
    // Up, but the hello never got its answer (the other side was reconnecting when this one came
    // up): reopen it rather than wait for the connection to time out.
    const bool unanswered = g_hosting ? l.id == kNone : g_self == kNone;
    if (unanswered && now - l.ready_at > 6000) {
      event("link to %s up with no answer: reopening", g_hosting ? "a member" : "the host");
      drop_later(l.np);
      l.was_ready = false;
      l.retry_at = now + kRetryMs;
      continue;
    }
    if (!l.hello_sent && (!g_hosting || !g_test.on)) {
      std::vector<uint8_t> h = {kHello};
      put_str(h, g_code);
      put_str(h, g_name);
      h.push_back(g_hosting ? 1 : 0);
      h.push_back((uint8_t)g_room.members.size());
      l.hello_sent = send(l, h);
    }
    std::vector<uint8_t> m;
    for (int n = 0; n < 4000 && !g_yield_to && l.np->PopArena(m); ++n) {
      if (g_hosting) host_message(l, m);
      else member_message(m);
    }
    if (g_yield_to) break;
  }
  if (g_yield_to) {
    Link* keep = (Link*)g_yield_to;
    g_yield_to = nullptr;
    event("joining %s's room", keep->code.c_str());
    Link kept = std::move(*keep);
    for (Link& l : g_links) { drop_later(l.np); drop_later(l.mm); }
    g_links.clear();
    kept.hello_sent = false;
    kept.id = kNone;
    g_links.push_back(std::move(kept));
    g_hosting = false;
    g_room = Room{};
    g_self = kNone;
  }
}

void send_pings() {
  if (!g_hosting || now_ms() < g_next_pings_ms) return;
  g_next_pings_ms = now_ms() + 1000;
  g_ping.fill(-1);
  if (g_self < kMaxMembers) g_ping[g_self] = 0;
  std::vector<uint8_t> m = {kPings, 0};
  for (Link& l : g_links) {
    if (l.id >= kMaxMembers || !l.np) continue;
    const int ms = l.np->PreMatchPingMs();
    g_ping[l.id] = ms > 0 ? ms : -1;
  }
  for (int id = 0; id < kMaxMembers; ++id) {
    if (!find(g_room, (uint8_t)id)) continue;
    m.push_back((uint8_t)id);
    m.push_back((uint8_t)((uint16_t)g_ping[id] >> 8));
    m.push_back((uint8_t)g_ping[id]);
  }
  m[1] = (uint8_t)((m.size() - 2) / 3);
  for (Link& l : g_links) if (l.id != kNone) send(l, m);
}

// ---- the match: start the right Direct search between games ----
const Member* opponent() {
  const int slot = ring_slot(g_room, g_self);
  if (slot < 0 || g_room.doubles) return nullptr;
  return find(g_room, g_room.ring[slot ^ 1]);
}

// What this member's game should be searching for between games: the opponent's code (singles), or
// the room's Teams code with everyone in the ring (doubles). False when it should not play.
struct Target { int mode = 0; std::string code, label; uint8_t team = 0; std::vector<uint8_t> others; };
bool target(Target& t) {
  const int slot = ring_slot(g_room, g_self);
  if (slot < 0) return false;
  if (!g_room.doubles) {
    const Member* opp = opponent();
    if (!opp) return false;
    t.mode = Matchmaking::DIRECT;
    t.code = opp->code;
    t.label = opp->name;
    t.others = {opp->id};
    return true;
  }
  // Doubles: the four search Teams with the host's code; teams by ring slot (1-2 red, 3-4 blue).
  const Member* host = find(g_room, owner(g_room));
  if (!host) return false;
  t.mode = Matchmaking::TEAMS;
  t.code = host->code;
  t.label = "doubles";
  t.team = (uint8_t)(slot / 2);
  for (int s = 0; s < 4; ++s) if (s != slot && g_room.ring[s] != kNone) t.others.push_back(g_room.ring[s]);
  return t.others.size() == 3;
}

void steer_match() {
  if (g_self == kNone || g_room.in_game || g_wait_room || now_ms() < g_next_match_ms) return;
  const online::ArenaMatchStatus st = online::arena_match_status();
  if (st.in_match) return;
  // The search's own state: a pair that has just connected is not an "active session" yet.
  const bool searching = st.state >= (int)Matchmaking::INITIALIZING && st.state <= (int)Matchmaking::CONNECTION_SUCCESS;
  const bool ours = st.mode == Matchmaking::DIRECT || st.mode == Matchmaking::TEAMS;
  Target t;
  if (!target(t)) {
    g_status = ring_slot(g_room, g_self) >= 0 ? "Waiting for another player" : "Watching";
    if (searching && ours && online::in_online_menus()) {
      event("out of the ring: leaving the match with %s", st.code.c_str());
      online::native_cleanup_match();
      g_next_match_ms = now_ms() + 1000;
    }
    return;
  }
  if (searching && (st.mode != t.mode || st.code != t.code)) {
    if (!online::in_online_menus()) return;
    event("next match %s: leaving %s", t.code.c_str(), st.code.c_str());
    online::native_cleanup_match();
    g_next_match_ms = now_ms() + 1000;
    return;
  }
  if (st.state == (int)Matchmaking::ERROR_ENCOUNTERED) {
    online::native_cleanup_match();
    g_next_match_ms = now_ms() + 2000;
    g_status = "Could not reach " + t.label + ", trying again";
    return;
  }
  if (searching) {
    g_status = st.state == (int)Matchmaking::CONNECTION_SUCCESS ? "Ready to fight " + t.label : "Connecting to " + t.label;
    if (g_test.on && st.state == (int)Matchmaking::CONNECTION_SUCCESS && !st.local_ready && online::arena_lock_in(g_test.character, 0))
      event("locked in for %s", t.code.c_str());
    return;
  }
  if (!online::in_online_menus()) { g_status = "Go to Direct to play " + t.label; return; }
  if (g_test.on) {
    // Test rooms: the players find each other on loopback, in ring-slot order (slot 0 decides).
    auto& lp = Matchmaking::local_peer;
    lp.enabled = true;
    lp.local_index = ring_slot(g_room, g_self);
    // A block of ports per game (both sides know the room's game count), so a new pair never reaches
    // the last pair's connection while it is still closing.
    const int block = g_test.base + 300 + 20 * (int)(g_room.games % 10);
    lp.local_port = (uint16_t)(block + g_test.index);
    lp.remotes.clear();
    for (int s = 0; s < ring_size(g_room); ++s) {
      const Member* m = find(g_room, g_room.ring[s]);
      if (!m || m->id == g_self) continue;
      int index = -1;
      std::sscanf(m->code.c_str(), "ARENA#%d", &index);
      lp.remotes.push_back("127.0.0.1:" + std::to_string(block + index));
    }
  }
  std::string pairing = t.code;
  for (uint8_t id : t.others) pairing += "/" + std::to_string(id);
  if (pairing != g_pair_code) { online::arena_note_new_pairing(); g_pair_code = pairing; }
  std::string error;
  const bool locked = g_test.on;
  if (online::arena_start_search(t.mode, t.code, locked, g_test.character, 0, t.team, &error)) {
    event("searching for %s (%s)", t.code.c_str(), t.mode == Matchmaking::TEAMS ? "doubles" : "singles");
    g_status = "Connecting to " + t.label;
  } else {
    event("could not search for %s: %s", t.code.c_str(), error.c_str());
  }
  g_next_match_ms = now_ms() + 1000;
}

// Test rooms only: the host plays the members' parts from MELEE_ARENA_TEST_PLAN, so hidden runs
// cover asking, answering and the doubles vote end to end. Steps, each once and in order, between
// games once the room has finished at least N games:
//   mode:<N>:winner|turns|ask      ask:<N>:<member>      answer:<N>:yes|winner|no (every fighter)
//   doubles:<N>                    (proposed by the host; every voter says yes)
struct PlanStep { std::string what, arg; uint32_t after = 0; uint8_t member = kNone; bool done = false; };
std::vector<PlanStep> g_plan;
bool g_plan_read = false;

void run_test_plan() {
  if (!g_test.on || !g_hosting) return;
  if (!g_plan_read) {
    g_plan_read = true;
    const char* env = std::getenv("MELEE_ARENA_TEST_PLAN");
    std::string all = env ? env : "";
    for (size_t at = 0; at < all.size();) {
      const size_t end = std::min(all.find(',', at), all.size());
      const std::string step = all.substr(at, end - at);
      at = end + 1;
      PlanStep p;
      char what[16] = {}, arg[16] = {};
      unsigned after = 0;
      if (std::sscanf(step.c_str(), "%15[a-z]:%u:%15s", what, &after, arg) >= 2) {
        p.what = what; p.after = after; p.arg = arg;
        if (p.what == "ask") p.member = (uint8_t)std::atoi(arg);
        g_plan.push_back(p);
      }
    }
    if (!g_plan.empty()) event("test plan: %zu steps", g_plan.size());
  }
  if (g_room.in_game) return;
  for (PlanStep& p : g_plan) {
    if (p.done) continue;
    if (g_room.games < p.after) return;   // in order: wait for this one
    bool ok = false;
    if (p.what == "mode") ok = set_mode(g_room, p.arg == "turns" ? Mode::TakeTurns : p.arg == "ask" ? Mode::Ask : Mode::WinnerStays);
    else if (p.what == "ask") ok = ask(g_room, p.member);
    else if (p.what == "answer") {
      const Answer a = p.arg == "no" ? Answer::No : p.arg == "winner" ? Answer::YesAndPlayWinner : Answer::Yes;
      for (int s = 0; s < ring_size(g_room) && g_room.asker != kNone; ++s) ok = answer(g_room, g_room.ring[s], a) || ok;
    } else if (p.what == "doubles") {
      ok = propose_switch(g_room, g_self);
      for (uint8_t v : switch_voters(g_room)) vote_switch(g_room, v, true);
    }
    p.done = true;
    event("test plan: %s %s after %u games: %s", p.what.c_str(), p.arg.c_str(), p.after, ok ? "done" : "refused");
  }
}

void publish_view() {
  View v;
  v.active = g_active;
  v.hosting = g_hosting;
  v.linked = g_hosting || (!g_links.empty() && g_links[0].was_ready);
  v.self = g_self;
  v.room = g_room;
  v.ping_ms = g_ping;
  DWORD code = 0;
  v.watching_live = g_viewer && GetExitCodeProcess(g_viewer, &code) && code == STILL_ACTIVE;
  v.status = g_status;
  std::lock_guard<std::mutex> lk(g_mutex);
  g_view = std::move(v);
}

}  // namespace

// ---------------------------------------------------------------- public
bool set_test(const std::string& spec) {
  int index = -1, count = 0, base = 0, character = 2;
  const int n = std::sscanf(spec.c_str(), "%d/%d/%d/%d", &index, &count, &base, &character);
  if (n < 3 || count < 2 || count > kMaxMembers || index < 0 || index >= count || base < 1024 || base > 60000 ||
      character < 0 || character > 25)
    return false;
  g_test = {true, index, count, base, (uint8_t)character};
  char code[16];
  std::snprintf(code, sizeof code, "ARENA#%d", index);
  g_code = code;
  g_name = "P" + std::to_string(index + 1);
  g_active = true;
  g_hosting = index == 0;
  g_ping.fill(-1);
  if (g_hosting) { g_room = Room{}; g_self = join(g_room, g_code, g_name); }
  open_links();
  return true;
}
bool test_mode() { return g_test.on; }
void set_recording_codes(std::vector<uint8_t> codes) { g_codes = std::move(codes); }
void set_viewer_launcher(std::function<void*(const std::string&)> launch) { g_launch_viewer = std::move(launch); }

void tick() {
  {
    std::deque<Pending> todo;
    { std::lock_guard<std::mutex> lk(g_mutex); todo.swap(g_pending); }
    if (g_code.empty() && !todo.empty()) online::local_identity(&g_code, &g_name);   // a real room: the Slippi account
    for (const Pending& p : todo) do_act(p);
  }
  if (!g_active) return;
  service_links();
  send_pings();
  run_test_plan();
  broadcast_room();
  steer_match();
  publish_view();
}

void recording_event(const uint8_t* data, uint32_t size) {
  if (!g_active || !data || !size) return;
  const uint8_t command = data[0];
  if (command == 0x35) { g_table.assign(data, data + size); return; }
  if (command == 0x36) {
    // Game Start: an online match (the Game Start's scene, or its online match id) in the ring.
    const uint8_t* start = data + 1;
    const uint32_t n = size - 1;
    g_my_online = n >= 0x2F8 && (start[0x1A3] == 0x08 || start[0x2BD] != 0) && ring_slot(g_room, g_self) >= 0;
    g_my_tag = feed::game_tag(start, n);
    if (g_my_online) close_viewer();   // playing now: the last game's viewer window has had its turn
    g_my_doubles = g_room.doubles;
    feed::port_teams(start, n, g_port_team);
    if (!g_my_doubles) { const Member* opp = opponent(); g_my_opp = opp ? opp->code : std::string(); }
    else {
      // Doubles: a player of the other team stands for it in the report.
      const int slot = ring_slot(g_room, g_self);
      const Member* other = slot >= 0 ? find(g_room, g_room.ring[slot < 2 ? 2 : 0]) : nullptr;
      g_my_opp = other ? other->code : std::string();
    }
    // The first fighter sends the game out when anybody is watching.
    const bool watchers = g_room.members.size() > (size_t)ring_size(g_room);
    g_sending = g_my_online && g_room.ring[0] == g_self && watchers;
    if (g_sending) {
      g_sender = feed::Sender{};
      g_sender.set_out([](const uint8_t* d, uint32_t len) {
        std::vector<uint8_t> m = {kFeed};
        m.insert(m.end(), d, d + len);
        if (g_hosting) {
          for (Link& l : g_links) if (l.id != kNone && ring_slot(g_room, l.id) < 0) send(l, m, true);
        } else {
          send_to_host(m, true);
        }
      });
      if (!g_table.empty()) g_sender.event(g_table.data(), (uint32_t)g_table.size());
      event("sending game %08X to the room", g_my_tag);
    }
  }
  if (g_sending) g_sender.event(data, size);
  // Every fighter reports its game's start and end to the host; the first report of each counts.
  if ((command == 0x36 || command == 0x39) && g_my_online) {
    std::vector<uint8_t> m = {kReport, (uint8_t)(command == 0x36 ? kStarted : kEnded)};
    put32(m, g_my_tag);
    std::string winner;
    // Game End: the method (1 time, 2 game, 3 game in newer recordings; 7 is a no contest), the
    // LRA-Start port, then each port's placement. Whoever placed first wins (in doubles, their team).
    if (command == 0x39) {
      const int result = feed::my_result(data + 1, size - 1, online::local_player_index(), g_port_team, g_my_doubles);
      if (result >= 0) winner = result ? g_code : g_my_opp;
    }
    put_str(m, winner);
    if (g_hosting) apply_report(m[1], g_my_tag, winner);
    else send_to_host(m);
    if (command == 0x39) {
      event("my game %08X ended (winner %s)", g_my_tag, winner.empty() ? "none" : winner.c_str());
      if (!g_hosting && g_room.in_game) g_wait_room = true;   // until the room has this game's end
      g_my_online = false;
    }
  }
  if (command == 0x39 && g_sending) {
    const auto& st = g_sender.stats();
    event("feed sent: %u frames, %llu bytes, %u replaced after rollbacks, %u late", st.frames, (unsigned long long)st.bytes,
          st.superseded, st.late);
    g_sending = false;
  }
}

void shutdown() {
  close_viewer();
  g_receiver.close();
  for (Link& l : g_links) l.np.reset();
  g_links.clear();
  g_active = false;
}

void act(Act a, uint8_t arg, const std::string& text) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_pending.push_back({a, arg, text});
}

bool in_room() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_view.active;
}

View view() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_view;
}

std::vector<std::string> recent_events() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return {g_events.begin(), g_events.end()};
}

}  // namespace slippi::arena

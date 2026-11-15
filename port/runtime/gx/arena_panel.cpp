// The arena room in the side bars (see arena_panel.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "arena_panel.h"
#include "host.h"
#include "slippi_arena.h"
#include "slippi_online.h"
#include "yougame_link.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace arena_panel {
namespace {

using slippi::arena::Act;
using slippi::arena::Member;
using slippi::arena::Role;
using slippi::arena::Room;
using slippi::arena::View;
namespace ar = slippi::arena;

constexpr uint16_t kDLeft = 0x0001, kDRight = 0x0002, kDDown = 0x0004, kDUp = 0x0008, kZ = 0x0010, kR = 0x0020,
                   kA = 0x0100, kB = 0x0200, kX = 0x0400, kY = 0x0800, kStart = 0x1000;

// Colours of the game's own menus: steel panels, gold tags, the red and blue corners.
constexpr ImU32 kInk = IM_COL32(8, 10, 18, 255), kWhite = IM_COL32(255, 255, 255, 255), kDim = IM_COL32(154, 166, 192, 255),
                kGold = IM_COL32(243, 194, 46, 255), kGoldDark = IM_COL32(201, 138, 12, 255), kRed = IM_COL32(216, 52, 60, 255),
                kBlue = IM_COL32(58, 86, 216, 255), kSteel = IM_COL32(26, 33, 51, 235), kLine = IM_COL32(74, 86, 116, 255),
                kGrey = IM_COL32(127, 138, 163, 255), kRecord = IM_COL32(255, 210, 74, 255);

enum class Page { None, Rules, Code };

struct State {
  bool loaded = false;
  Look look = Look::Combined;
  bool tabs = false;            // folded into tabs at the edges
  bool focus = false;           // the room menu has the controller
  Page page = Page::None;
  int row = 0;                  // the rules page's row, or the prompt's choice
  int key_row = 0, key_col = 0; // the on-screen keyboard
  std::string code;             // typed so far
  std::vector<std::string> recent;
  int recent_pick = -1;         // a recent code highlighted (-1: the keyboard)
  uint16_t prev = 0;
  int prev_dir = 0, prev_c = 0;
  double dir_at = 0;
  double shown_at = -1;         // when the bars began to slide in
  bool was_visible = false;
  uint32_t seen_prompt = 0;
} g;

const char* const kLookNames[kLookCount] = {"Corners + cards", "Card", "Split", "Corners", "Ring"};
const char* const kModeNames[3] = {"Winner stays", "Take turns", "Ask"};
const char* const kKeys[5] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ#", "0123456789", "<>"};   // last row: delete, done

std::filesystem::path settings_file() {
  return std::filesystem::u8path(slippi::online::config().user_dir) / "arena" / "panel.txt";
}
void load() {
  if (g.loaded) return;
  g.loaded = true;
  std::ifstream f(settings_file());
  int look = 0, tabs = 0;
  if (f >> look >> tabs) {
    g.look = (Look)std::clamp(look, 0, kLookCount - 1);
    g.tabs = tabs != 0;
  }
}
void save() {
  std::error_code ec;
  std::filesystem::create_directories(settings_file().parent_path(), ec);
  std::ofstream f(settings_file());
  f << (int)g.look << " " << (g.tabs ? 1 : 0) << "\n";
}
void load_recent() {
  g.recent.clear();
  std::ifstream f(std::filesystem::u8path(slippi::online::config().user_dir) / "direct-codes.json");
  const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
  if (!j.is_array()) return;
  for (const auto& e : j)
    if (e.is_object() && e.find("connectCode") != e.end() && e["connectCode"].is_string() && g.recent.size() < 6) {
      const std::string c = e["connectCode"];
      if (c.find('#') != std::string::npos) g.recent.push_back(c);
    }
}

// ---- drawing ----
ImDrawList* dl() { return ImGui::GetForegroundDrawList(); }
float g_k = 1.0f;   // scale: 1 at a 152-pixel bar
float px(float v) { return v * g_k; }

void text(float x, float y, float size, ImU32 c, const std::string& s, bool shadow = true) {
  if (shadow) dl()->AddText(ImGui::GetFont(), px(size), ImVec2(x + px(1), y + px(1)), IM_COL32(0, 0, 0, 220), s.c_str());
  dl()->AddText(ImGui::GetFont(), px(size), ImVec2(x, y), c, s.c_str());
}
float text_w(float size, const std::string& s) { return ImGui::GetFont()->CalcTextSizeA(px(size), 10000.f, 0.f, s.c_str()).x; }
void text_center(float cx, float y, float size, ImU32 c, const std::string& s) { text(cx - text_w(size, s) * .5f, y, size, c, s); }
std::string fit(const std::string& s, float size, float w) {
  if (text_w(size, s) <= w) return s;
  std::string t = s;
  while (!t.empty() && text_w(size, t + ".") > w) t.pop_back();
  return t + ".";
}

// The game's slanted tag (NEXT UP, JUST WATCHING ...).
float tag(float x, float y, const char* label, ImU32 top, ImU32 bottom, ImU32 ink = IM_COL32(35, 23, 0, 255)) {
  const float h = px(15), w = text_w(10.5f, label) + px(22);
  const ImVec2 pts[4] = {ImVec2(x, y), ImVec2(x + w, y), ImVec2(x + w - px(8), y + h), ImVec2(x, y + h)};
  dl()->AddConvexPolyFilled(pts, 4, bottom);
  const ImVec2 hi[4] = {ImVec2(x, y), ImVec2(x + w, y), ImVec2(x + w - px(4), y + h * .5f), ImVec2(x, y + h * .5f)};
  dl()->AddConvexPolyFilled(hi, 4, top);
  text(x + px(6), y + px(1.5f), 10.5f, ink, label, false);
  return w;
}
void tag_gold(float x, float y, const char* l) { tag(x, y, l, IM_COL32(255, 228, 128, 255), kGoldDark); }
void tag_grey(float x, float y, const char* l) { tag(x, y, l, IM_COL32(201, 208, 222, 255), IM_COL32(127, 138, 163, 255)); }
void tag_red(float x, float y, const char* l) { tag(x, y, l, IM_COL32(255, 128, 136, 255), IM_COL32(184, 31, 40, 255), kWhite); }

// A player's badge: a disc with the first letter (stock icons come later).
void badge(float cx, float cy, float r, ImU32 ring, const std::string& name) {
  dl()->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(34, 40, 60, 255));
  dl()->AddCircle(ImVec2(cx, cy), r, ring, 0, px(2));
  const std::string letter = name.empty() ? "?" : std::string(1, name[0]);
  text_center(cx, cy - px(r > px(20) ? 12 : 7), r > px(20) ? 24 : 13, kWhite, letter);
}

void button_glyph(float x, float y, char b) {
  ImU32 c = IM_COL32(174, 180, 191, 255), ink = IM_COL32(21, 24, 31, 255);
  if (b == 'Z') { c = IM_COL32(123, 82, 214, 255); ink = kWhite; }
  if (b == 'A') { c = IM_COL32(23, 178, 107, 255); ink = kWhite; }
  if (b == 'B') { c = IM_COL32(220, 60, 60, 255); ink = kWhite; }
  if (b == 'C') { c = kGold; }
  dl()->AddCircleFilled(ImVec2(x + px(7), y + px(7)), px(7.5f), c);
  const std::string s(1, b);
  text(x + px(7) - text_w(9.5f, s) * .5f, y + px(1.5f), 9.5f, ink, s, false);
}
void hint(float x, float y, char b, const std::string& label, float w) {
  button_glyph(x, y, b);
  text(x + px(19), y + px(1), 11, IM_COL32(207, 214, 230, 255), fit(label, 11, w - px(19)));
}

std::string record(const Member& m) { return std::to_string(m.wins) + "-" + std::to_string(m.losses); }
std::string ping_text(const View& v, uint8_t id) {
  if (v.room.members.empty()) return "";
  if (id == ar::owner(v.room)) return "host";
  const int ms = id < ar::kMaxMembers ? v.ping_ms[id] : -1;
  return ms < 0 ? "" : std::to_string(ms) + "ms";
}

// A member's row card: badge, name, record or ping, a glow for "you".
void slot_card(float x, float y, float w, const View& v, const Member& m, bool you, bool asking) {
  const float h = px(32);
  dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(26, 33, 51, 240), px(5));
  dl()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), you ? kGold : kLine, px(5), 0, you ? px(2) : px(1));
  badge(x + px(16), y + h * .5f, px(11), you ? kGold : IM_COL32(223, 229, 242, 255), m.name);
  const std::string right = asking ? "asks" : ping_text(v, m.id);
  const float rw = right.empty() ? 0 : text_w(9.5f, right) + px(4);
  text(x + px(31), y + px(4), 12.5f, kWhite, fit(m.name, 12.5f, w - px(35) - rw));
  text(x + px(31), y + px(18), 9.5f, kRecord, record(m), false);
  if (!right.empty()) text(x + w - rw, y + px(18), 9.5f, asking ? kGold : kDim, right, false);
}
void open_spot(float x, float y, float w) {
  const float h = px(28);
  for (float t = x; t < x + w; t += px(8)) {   // dashed outline
    dl()->AddLine(ImVec2(t, y), ImVec2(std::min(t + px(4), x + w), y), kLine, px(1));
    dl()->AddLine(ImVec2(t, y + h), ImVec2(std::min(t + px(4), x + w), y + h), kLine, px(1));
  }
  dl()->AddLine(ImVec2(x, y), ImVec2(x, y + h), kLine, px(1));
  dl()->AddLine(ImVec2(x + w, y), ImVec2(x + w, y + h), kLine, px(1));
  text_center(x + w * .5f, y + px(8), 10, IM_COL32(111, 123, 152, 255), "open spot");
}

// One corner: title, a big badge per fighter on that side, name, record, streak.
float corner(float x0, float x1, float y0, bool red, const View& v, int side, bool live) {
  const Room& r = v.room;
  const float w = x1 - x0;
  const ImU32 col = red ? kRed : kBlue;
  const float h = px(r.doubles ? 150 : 132);
  const ImU32 top = red ? IM_COL32(216, 52, 60, 150) : IM_COL32(58, 86, 216, 160);
  dl()->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y0 + h), top, top, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
  dl()->AddLine(red ? ImVec2(x1 - px(1.5f), y0) : ImVec2(x0 + px(1.5f), y0), red ? ImVec2(x1 - px(1.5f), y0 + h) : ImVec2(x0 + px(1.5f), y0 + h), col, px(3));
  text_center(x0 + w * .5f, y0 + px(8), 14, kWhite, red ? "RED CORNER" : "BLUE CORNER");
  if (live && red) tag_red(x1 - px(44), y0 + px(26), "LIVE");
  const int slots = r.doubles ? 2 : 1;
  float y = y0 + px(28);
  for (int k = 0; k < slots; ++k) {
    const int slot = r.doubles ? side * 2 + k : side;
    const Member* m = ar::find(r, r.ring[slot]);
    if (!m) { text_center(x0 + w * .5f, y + px(24), 11, kDim, "waiting..."); y += px(60); continue; }
    const float rad = px(r.doubles ? 17 : 28);
    badge(x0 + w * .5f, y + rad, rad, m->id == v.self ? kGold : IM_COL32(223, 229, 242, 255), m->name);
    y += rad * 2 + px(4);
    text_center(x0 + w * .5f, y, 13, kWhite, fit(m->name, 13, w - px(8)));
    y += px(15);
    std::string line = record(*m);
    if (r.streak_id == m->id && r.streak >= 2) line += "   x" + std::to_string(r.streak) + " streak";
    const std::string p = ping_text(v, m->id);
    text_center(x0 + w * .5f, y, 12, kGold, line);
    y += px(14);
    if (!p.empty()) { text_center(x0 + w * .5f, y, 9.5f, kDim, p); y += px(12); }
    y += px(4);
  }
  return y0 + h;
}

// Members in the line (with an open spot after), or watching.
float line_cards(float x, float y, float w, const View& v, bool with_open_spot) {
  tag_gold(x, y, "NEXT UP");
  y += px(19);
  for (uint8_t id : v.room.line) {
    const Member* m = ar::find(v.room, id);
    if (!m) continue;
    slot_card(x, y, w, v, *m, id == v.self, v.room.asker == id);
    y += px(36);
  }
  if (with_open_spot) { open_spot(x, y, w); y += px(32); }
  return y;
}
float watch_cards(float x, float y, float w, const View& v) {
  tag_grey(x, y, "JUST WATCHING");
  y += px(19);
  bool any = false;
  for (const Member& m : v.room.members) {
    if (ar::role(v.room, m.id) != Role::Watching) continue;
    slot_card(x, y, w, v, m, m.id == v.self, v.room.asker == m.id);
    y += px(36);
    any = true;
  }
  if (!any) { text(x + px(2), y, 10, kDim, "nobody"); y += px(14); }
  return y;
}

// The prompt that needs this member's answer, if any: 1 an ask to answer, 2 a doubles vote.
int prompt_kind(const View& v) {
  const Room& r = v.room;
  const int slot = ar::ring_slot(r, v.self);
  if (slot >= 0 && r.asker != ar::kNone && !(r.refused & (1u << slot)) && r.leaving_slot == ar::kNone) return 1;
  if (r.switch_pending) {
    const auto voters = ar::switch_voters(r);
    if (std::find(voters.begin(), voters.end(), v.self) != voters.end() && !(r.switch_votes & (1u << v.self))) return 2;
  }
  return 0;
}

void prompt_box(float x, float y, float w, const View& v, int kind) {
  const Room& r = v.room;
  std::vector<std::string> options;
  std::string question;
  if (kind == 1) {
    const Member* a = ar::find(r, r.asker);
    question = (a ? a->name : std::string("Someone")) + " asks to play next. Give up your spot?";
    options = {"Yes", "Yes, and I play the winner", "No"};
  } else {
    question = r.doubles ? "Back to singles?" : "Switch the four of you to doubles?";
    options = {"Yes", "No"};
  }
  // The question, wrapped to the box.
  std::vector<std::string> lines;
  {
    std::string word, line;
    for (size_t i = 0; i <= question.size(); ++i) {
      const char c = i < question.size() ? question[i] : ' ';
      if (c != ' ') { word += c; continue; }
      const std::string trial = line.empty() ? word : line + " " + word;
      if (text_w(11.5f, trial) > w - px(14) && !line.empty()) { lines.push_back(line); line = word; }
      else line = trial;
      word.clear();
    }
    if (!line.empty()) lines.push_back(line);
  }
  const float h = px(10) + lines.size() * px(14) + options.size() * px(19) + px(6);
  dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(28, 35, 54, 245), px(6));
  dl()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), kGold, px(6), 0, px(2));
  float yy = y + px(6);
  for (const std::string& l : lines) { text(x + px(7), yy, 11.5f, kWhite, l); yy += px(14); }
  yy += px(2);
  for (size_t i = 0; i < options.size(); ++i) {
    const bool sel = g.focus && g.page == Page::None && g.row == (int)i;
    if (sel) dl()->AddRectFilled(ImVec2(x + px(5), yy), ImVec2(x + w - px(5), yy + px(17)), kGold, px(4));
    text(x + px(10), yy + px(2), 11, sel ? IM_COL32(35, 23, 0, 255) : IM_COL32(207, 214, 230, 255), fit(options[i], 11, w - px(20)), !sel);
    yy += px(19);
  }
}

// The keyboard, the recent codes and the rules: one box in the right bar while open.
void page_box(float x, float y, float w, const View& v) {
  if (g.page == Page::Rules) {
    const float h = px(116);
    dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(28, 35, 54, 245), px(6));
    dl()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), kGold, px(6), 0, px(2));
    text(x + px(7), y + px(5), 12, kGold, "RULES");
    const std::string values[3] = {kModeNames[(int)v.room.mode], kLookNames[(int)g.look], g.tabs ? "Tabs" : "Shown"};
    const char* labels[3] = {"Mode", "Look", "Panel"};
    for (int i = 0; i < 3; ++i) {
      const float yy = y + px(24) + i * px(26);
      if (g.row == i) dl()->AddRectFilled(ImVec2(x + px(4), yy - px(1)), ImVec2(x + w - px(4), yy + px(23)), IM_COL32(243, 194, 46, 60), px(4));
      text(x + px(8), yy, 9.5f, kDim, labels[i], false);
      text(x + px(8), yy + px(10), 11.5f, kWhite, std::string("< ") + values[i] + " >");
    }
    hint(x + px(6), y + h - px(19), 'B', "Back", w - px(12));
    return;
  }
  if (g.page == Page::Code) {
    const float h = px(78) + g.recent.size() * px(17) + px(100);
    dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(28, 35, 54, 245), px(6));
    dl()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), kGold, px(6), 0, px(2));
    text(x + px(7), y + px(5), 12, kGold, "ADD A FRIEND");
    text(x + px(7), y + px(20), 9.5f, kDim, "They enter your code too.", false);
    float yy = y + px(36);
    dl()->AddRectFilled(ImVec2(x + px(6), yy), ImVec2(x + w - px(6), yy + px(20)), IM_COL32(5, 7, 11, 255), px(3));
    text(x + px(10), yy + px(3), 12.5f, kWhite, g.code + ((int)(ImGui::GetTime() * 2) % 2 ? "_" : " "));
    yy += px(26);
    for (size_t i = 0; i < g.recent.size(); ++i) {
      const bool sel = g.recent_pick == (int)i;
      if (sel) dl()->AddRectFilled(ImVec2(x + px(5), yy), ImVec2(x + w - px(5), yy + px(16)), kGold, px(3));
      text(x + px(10), yy + px(1.5f), 10.5f, sel ? IM_COL32(35, 23, 0, 255) : IM_COL32(207, 214, 230, 255), g.recent[i], !sel);
      yy += px(17);
    }
    yy += px(4);
    const float cell = (w - px(12)) / 10.0f;
    for (int r = 0; r < 5; ++r) {
      const std::string keys = kKeys[r];
      for (size_t c = 0; c < keys.size(); ++c) {
        const float kx = x + px(6) + c * (r == 4 ? cell * 5 : cell), ky = yy + r * px(17);
        const float kw = r == 4 ? cell * 5 - px(2) : cell - px(2);
        const bool sel = g.recent_pick < 0 && g.key_row == r && g.key_col == (int)c;
        dl()->AddRectFilled(ImVec2(kx, ky), ImVec2(kx + kw, ky + px(15)), sel ? kGold : IM_COL32(45, 54, 78, 255), px(2));
        const std::string label = r == 4 ? (c == 0 ? "DEL" : "OK") : std::string(1, keys[c]);
        text(kx + kw * .5f - text_w(9.5f, label) * .5f, ky + px(2), 9.5f, sel ? IM_COL32(35, 23, 0, 255) : kWhite, label, false);
      }
    }
  }
}

// ---- input ----
void send_act(Act a, uint8_t arg = 0, const std::string& text = {}) { ar::act(a, arg, text); }

void type_key(char c) {
  if (g.code.size() < 10) g.code += c;
}
void submit_code() {
  std::string c = g.code;
  for (char& ch : c) ch = (char)std::toupper((unsigned char)ch);
  if (c.find('#') == std::string::npos || c.size() < 3) return;
  send_act(Act::AddCode, 0, c);
  g.page = Page::None;
  g.code.clear();
}

void handle_input(const Frame& f, const View& v) {
  const uint16_t pressed = f.buttons & ~g.prev;
  g.prev = f.buttons;
  // Directions from the D-pad or the stick, with a repeat.
  int dir = 0;
  if ((f.buttons & kDUp) || f.stick_y > 60) dir = 1;
  else if ((f.buttons & kDDown) || f.stick_y < -60) dir = 2;
  else if ((f.buttons & kDLeft) || f.stick_x < -60) dir = 3;
  else if ((f.buttons & kDRight) || f.stick_x > 60) dir = 4;
  int step = 0;
  if (dir && (dir != g.prev_dir || f.time - g.dir_at > 0.18)) { step = dir; g.dir_at = f.time + (dir != g.prev_dir ? 0.2 : 0); }
  g.prev_dir = dir;
  // The C-stick flicks the room menu open and shut.
  const int c = (std::abs(f.sub_x) > 64 || std::abs(f.sub_y) > 64) ? 1 : 0;
  const bool flick = c && !g.prev_c;
  g.prev_c = c;
  if (flick) { g.focus = !g.focus; g.page = Page::None; g.row = 0; return; }
  if (!g.focus) return;

  if (g.page == Page::Code) {
    // Keyboard typing too.
    for (ImWchar ch : ImGui::GetIO().InputQueueCharacters)
      if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '#') type_key((char)ch);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !g.code.empty()) g.code.pop_back();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) submit_code();
    if (step == 1) {
      if (g.recent_pick < 0 && g.key_row == 0 && !g.recent.empty()) g.recent_pick = (int)g.recent.size() - 1;
      else if (g.recent_pick > 0) --g.recent_pick;
      else if (g.recent_pick < 0) g.key_row = std::max(0, g.key_row - 1);
    } else if (step == 2) {
      if (g.recent_pick >= 0) { if (++g.recent_pick >= (int)g.recent.size()) { g.recent_pick = -1; g.key_row = 0; } }
      else g.key_row = std::min(4, g.key_row + 1);
    } else if (step == 3) g.key_col = std::max(0, g.key_col - 1);
    else if (step == 4) g.key_col++;
    g.key_col = std::min(g.key_col, (int)std::string(kKeys[g.key_row]).size() - 1);
    if (pressed & kA) {
      if (g.recent_pick >= 0) { g.code = g.recent[g.recent_pick]; submit_code(); }
      else if (g.key_row == 4) { if (g.key_col == 0) { if (!g.code.empty()) g.code.pop_back(); } else submit_code(); }
      else type_key(kKeys[g.key_row][g.key_col]);
    }
    if (pressed & kStart) submit_code();
    if (pressed & kB) { if (!g.code.empty()) g.code.pop_back(); else g.page = Page::None; }
    return;
  }
  if (g.page == Page::Rules) {
    if (step == 1) g.row = std::max(0, g.row - 1);
    if (step == 2) g.row = std::min(2, g.row + 1);
    const int delta = step == 3 ? -1 : step == 4 || (pressed & kA) ? 1 : 0;
    if (delta) {
      if (g.row == 0 && v.active) send_act(Act::SetMode, (uint8_t)(((int)v.room.mode + delta + 3) % 3));
      if (g.row == 1) { g.look = (Look)(((int)g.look + delta + kLookCount) % kLookCount); save(); }
      if (g.row == 2) { g.tabs = !g.tabs; save(); }
    }
    if (pressed & kB) { g.page = Page::None; g.row = 0; }
    return;
  }
  // The room menu's own buttons.
  const int prompt = v.active ? prompt_kind(v) : 0;
  if (prompt) {
    const int options = prompt == 1 ? 3 : 2;
    if (step == 1) g.row = std::max(0, g.row - 1);
    if (step == 2) g.row = std::min(options - 1, g.row + 1);
    if (pressed & kA) {
      if (prompt == 1) send_act(Act::Answer, (uint8_t)(g.row == 0 ? ar::Answer::Yes : g.row == 1 ? ar::Answer::YesAndPlayWinner : ar::Answer::No));
      else send_act(Act::VoteSwitch, g.row == 0 ? 1 : 0);
      g.row = 0;
      return;
    }
  }
  if (pressed & kB) { g.focus = false; return; }
  if (pressed & kY) { g.page = Page::Rules; g.row = 0; return; }
  if (pressed & kZ) { g.page = Page::Code; g.code.clear(); g.recent_pick = -1; g.key_row = 0; g.key_col = 0; load_recent(); return; }
  if (!v.active) return;
  const Role role = ar::role(v.room, v.self);
  if (pressed & kX) {
    if (role == Role::Fighter) send_act(Act::GiveUp, 0);
    else send_act(Act::ToggleLine);
  }
  if ((pressed & kA) && !prompt && role != Role::Fighter && v.room.asker == ar::kNone) send_act(Act::Ask);
  if ((pressed & kR) && !v.room.switch_pending) send_act(Act::ProposeSwitch);
}

// ---- the looks ----
struct Bars { float lx0, lx1, rx0, rx1, y0, y1; };

// What the room is doing for this member ("Connecting to P2", "Watching"), one line in a bar.
void status_line(float x0, float x1, float y, const View& v) {
  if (!v.status.empty()) text(x0, y, 9.5f, kDim, fit(v.status, 9.5f, x1 - x0), false);
}

void bottom_hints(float x0, float x1, float y, const std::vector<std::pair<char, std::string>>& items) {
  float yy = y - items.size() * px(17);
  for (const auto& it : items) { hint(x0, yy, it.first, it.second, x1 - x0); yy += px(17); }
}

std::vector<std::pair<char, std::string>> action_hints(const View& v) {
  std::vector<std::pair<char, std::string>> out;
  if (!g.focus) {
    out.push_back({'C', prompt_kind(v) ? "C-stick to answer" : "C-stick: room menu"});
    return out;
  }
  const Role role = ar::role(v.room, v.self);
  if (role == Role::Fighter) out.push_back({'X', v.room.leaving_slot != ar::kNone && v.room.ring[v.room.leaving_slot] == v.self ? "Keep my spot" : "Give up my spot"});
  else if (role == Role::InLine) out.push_back({'X', "Just watch"});
  else out.push_back({'X', "Get in line"});
  if (role != Role::Fighter && v.room.asker == ar::kNone) out.push_back({'A', "Ask to play next"});
  if (v.room.members.size() >= 4 && !v.room.switch_pending) out.push_back({'R', v.room.doubles ? "Back to singles" : "Doubles (all 4 agree)"});
  out.push_back({'B', "Close"});
  return out;
}

void draw_combined(const Bars& b, const View& v, bool corners_only) {
  const float lw = b.lx1 - b.lx0, rw = b.rx1 - b.rx0, pad = px(8);
  const float ly = corner(b.lx0, b.lx1, b.y0, true, v, 0, v.room.in_game);
  const float ry = corner(b.rx0, b.rx1, b.y0, false, v, 1, v.room.in_game);
  if (!corners_only) {
    line_cards(b.lx0 + pad, ly + px(8), lw - pad * 2, v, v.room.line.size() < 2);
    watch_cards(b.rx0 + pad, ry + px(8), rw - pad * 2, v);
  } else {
    // Corners: the line and the stands as two short lists in the bars' feet.
    float y = ly + px(8);
    tag_gold(b.lx0 + pad, y, "NEXT UP");
    y += px(19);
    for (uint8_t id : v.room.line) { const Member* m = ar::find(v.room, id); if (m) { text(b.lx0 + pad, y, 11.5f, kWhite, fit(m->name, 11.5f, lw - pad * 2)); y += px(15); } }
    if (v.room.line.empty()) text(b.lx0 + pad, y, 10, kDim, "open spot");
    y = ry + px(8);
    tag_grey(b.rx0 + pad, y, "JUST WATCHING");
    y += px(19);
    bool any = false;
    for (const Member& m : v.room.members) if (ar::role(v.room, m.id) == Role::Watching) { text(b.rx0 + pad, y, 11.5f, kWhite, fit(m.name, 11.5f, rw - pad * 2)); y += px(15); any = true; }
    if (!any) text(b.rx0 + pad, y, 10, kDim, "nobody");
  }
  const int prompt = prompt_kind(v);
  const int my_slot = ar::ring_slot(v.room, v.self);
  const bool left_side = my_slot == 0 || (my_slot == 1 && v.room.doubles);
  if (prompt) prompt_box(left_side ? b.lx0 + pad : b.rx0 + pad, b.y1 - px(170), (left_side ? lw : rw) - pad * 2, v, prompt);
  bottom_hints(b.lx0 + pad, b.lx1 - pad, b.y1 - px(8), {{'Y', "Rules"}, {'Z', "Add a friend's code"}});
  const float mode_y = b.y1 - px(8) - action_hints(v).size() * px(17) - px(14);
  text(b.rx0 + pad, mode_y, 9.5f, kGold, std::string(v.room.doubles ? "DOUBLES  " : "") + kModeNames[(int)v.room.mode], false);
  status_line(b.lx0 + pad, b.lx1 - pad, b.y1 - px(8) - 2 * px(17) - px(14), v);
  bottom_hints(b.rx0 + pad, b.rx1 - pad, b.y1 - px(8), action_hints(v));
}

void draw_card(const Bars& b, const View& v, bool ring_box) {
  const float x = b.rx0 + px(6), w = b.rx1 - b.rx0 - px(12);
  dl()->AddRectFilled(ImVec2(b.rx0 + px(3), b.y0 + px(4)), ImVec2(b.rx1 - px(3), b.y1 - px(4)), kSteel, px(6));
  dl()->AddRect(ImVec2(b.rx0 + px(3), b.y0 + px(4)), ImVec2(b.rx1 - px(3), b.y1 - px(4)), IM_COL32(154, 166, 192, 255), px(6), 0, px(2));
  float y = b.y0 + px(12);
  text(x + px(2), y, 9.5f, kGold, "ARENA", false);
  if (v.room.in_game) tag_red(b.rx1 - px(48), y, "LIVE");
  y += px(11);
  text(x + px(2), y, 14, kWhite, std::string(v.room.doubles ? "DOUBLES - " : "") + kModeNames[(int)v.room.mode]);
  y += px(22);
  if (ring_box) {
    const float h = px(78);
    dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(44, 54, 84, 255), px(9));
    dl()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), kRed, px(9), 0, px(2));
    dl()->AddRect(ImVec2(x + px(3), y + px(3)), ImVec2(x + w - px(3), y + h - px(3)), kBlue, px(7), 0, px(2));
    for (int side = 0; side < 2; ++side) {
      const Member* m = ar::find(v.room, v.room.ring[v.room.doubles ? side * 2 : side]);
      const float cx = x + w * (side ? .76f : .24f);
      if (m) {
        badge(cx, y + px(26), px(15), m->id == v.self ? kGold : kWhite, m->name);
        text_center(cx, y + px(45), 11, kWhite, fit(m->name, 11, w * .45f));
        text_center(cx, y + px(59), 9.5f, kRecord, record(*m));
      }
    }
    text_center(x + w * .5f, y + px(20), 13, kGold, "VS");
    y += h + px(10);
  } else {
    tag_red(x, y, "IN THE RING");
    y += px(19);
    for (int s = 0; s < ar::ring_size(v.room); ++s) {
      const Member* m = ar::find(v.room, v.room.ring[s]);
      if (m) { slot_card(x, y, w, v, *m, m->id == v.self, false); y += px(36); }
    }
  }
  y = line_cards(x, y, w, v, v.room.line.size() < 2) + px(6);
  y = watch_cards(x, y, w, v);
  const int prompt = prompt_kind(v);
  if (prompt) prompt_box(x, b.y1 - px(250), w, v, prompt);
  auto items = action_hints(v);
  items.insert(items.begin(), {'Z', "Add a friend's code"});
  items.insert(items.begin(), {'Y', "Rules"});
  status_line(x + px(2), x + w, b.y1 - px(12) - items.size() * px(17) - px(14), v);
  bottom_hints(x + px(2), x + w, b.y1 - px(12), items);
}

void draw_split(const Bars& b, const View& v) {
  const float lw = b.lx1 - b.lx0, rw = b.rx1 - b.rx0, pad = px(8);
  float y = b.y0 + px(10);
  const Member* host = ar::find(v.room, ar::owner(v.room));
  text(b.lx0 + pad, y, 9.5f, kGold, (host ? host->name : std::string("ROOM")) + "'S ROOM", false);
  y += px(11);
  text(b.lx0 + pad, y, 14, kWhite, kModeNames[(int)v.room.mode]);
  y += px(24);
  tag_red(b.lx0 + pad, y, "IN THE RING");
  y += px(19);
  for (int s = 0; s < ar::ring_size(v.room); ++s) {
    const Member* m = ar::find(v.room, v.room.ring[s]);
    if (m) { slot_card(b.lx0 + pad, y, lw - pad * 2, v, *m, m->id == v.self, false); y += px(36); }
  }
  const Member* a = ar::find(v.room, v.room.ring[0]);
  const Member* c = ar::find(v.room, v.room.ring[v.room.doubles ? 2 : 1]);
  if (a && c) text_center(b.lx0 + lw * .5f, y + px(4), 24, kWhite, std::to_string(a->wins) + " - " + std::to_string(c->wins));
  float ry = line_cards(b.rx0 + pad, b.y0 + px(10), rw - pad * 2, v, true) + px(8);
  watch_cards(b.rx0 + pad, ry, rw - pad * 2, v);
  const int prompt = prompt_kind(v);
  if (prompt) prompt_box(b.rx0 + pad, b.y1 - px(250), rw - pad * 2, v, prompt);
  status_line(b.lx0 + pad, b.lx1 - pad, b.y1 - px(8) - 2 * px(17) - px(14), v);
  bottom_hints(b.lx0 + pad, b.lx1 - pad, b.y1 - px(8), {{'Y', "Rules"}, {'Z', "Add a friend's code"}});
  bottom_hints(b.rx0 + pad, b.rx1 - pad, b.y1 - px(8), action_hints(v));
}

void draw_not_in_room(const Bars& b) {
  // Not in a room: a small tab at the right edge until the player opens it (no clutter for anyone
  // who never uses rooms).
  if (!g.focus) {
    const float tw = px(30), th = px(64), x = b.rx1 - tw - px(4), y = b.y0 + (b.y1 - b.y0) * .42f;
    dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + tw, y + th), IM_COL32(26, 33, 51, 200), px(5));
    dl()->AddRect(ImVec2(x, y), ImVec2(x + tw, y + th), IM_COL32(154, 166, 192, 160), px(5));
    text_center(x + tw * .5f, y + px(6), 8.5f, kGold, "ARE");
    text_center(x + tw * .5f, y + px(17), 8.5f, kGold, "NA");
    button_glyph(x + tw * .5f - px(7), y + th - px(22), 'C');
    return;
  }
  const float x = b.rx0 + px(6), w = b.rx1 - b.rx0 - px(12);
  const float y = b.y0 + px(12), h = px(92);
  dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kSteel, px(6));
  dl()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(154, 166, 192, 255), px(6), 0, px(1.5f));
  text(x + px(7), y + px(6), 9.5f, kGold, "ARENA", false);
  text(x + px(7), y + px(18), 11.5f, kWhite, fit("Friends watch and", 11.5f, w - px(12)));
  text(x + px(7), y + px(32), 11.5f, kWhite, fit("rotate in", 11.5f, w - px(12)));
  hint(x + px(6), y + px(52), 'Z', "Add a friend's code", w - px(10));
  hint(x + px(6), y + px(70), 'B', "Close", w - px(10));
}

void draw_tabs(const Bars& b, const View& v, bool* hovered) {
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float tw = px(24), th = px(88);
  const float lx = b.lx0, rx = b.rx1 - tw, ty = b.y0 + (b.y1 - b.y0) * .3f;
  *hovered = (mouse.x >= lx && mouse.x <= lx + tw * 3 && mouse.y >= ty && mouse.y <= ty + th) ||
             (mouse.x >= rx - tw * 2 && mouse.x <= rx + tw && mouse.y >= ty && mouse.y <= ty + th);
  for (int side = 0; side < 2; ++side) {
    const float x = side ? rx : lx;
    dl()->AddRectFilled(ImVec2(x, ty), ImVec2(x + tw, ty + th), side ? kBlue : kRed, px(5));
    // Left: the room; right: how many are in it.
    text_center(x + tw * .5f, ty + px(8), 12, kWhite, side ? std::to_string(v.room.members.size()) : "R");
    text_center(x + tw * .5f, ty + px(26), 9, kWhite, side ? "IN" : "OOM");
    button_glyph(x + tw * .5f - px(7), ty + th - px(20), 'C');
  }
}

}  // namespace

bool wants_capture() { return g.focus; }

void frame(const Frame& f) {
  if (yougame::active()) return;   // rooms are a Slippi Direct feature; YouGame has its own rooms
  load();
  const View v = ar::view();
  const bool visible = f.on_online_menus && !(v.active && v.room.in_game && ar::role(v.room, v.self) == Role::Fighter);
  if (!visible) {
    g.was_visible = false;
    g.focus = false;
    g.prev = f.buttons;
    return;
  }
  if (!g.was_visible) { g.was_visible = true; g.shown_at = f.time; }
  handle_input(f, v);
  // The bars beside the picture; a picture that fills the window gets panels over its edges.
  Bars b;
  float bar = f.game_x0;
  const bool overlay = bar < 90.0f;
  if (overlay) bar = std::min(f.display_w * 0.17f, 220.0f);
  g_k = std::clamp(bar / 152.0f, 0.6f, 3.0f);
  b.lx0 = overlay ? 0 : 0; b.lx1 = bar;
  b.rx0 = f.display_w - bar; b.rx1 = f.display_w;
  b.y0 = f.game_y0; b.y1 = f.game_y1;
  if (overlay) {
    dl()->AddRectFilled(ImVec2(b.lx0, b.y0), ImVec2(b.lx1, b.y1), IM_COL32(0, 0, 0, 150));
    dl()->AddRectFilled(ImVec2(b.rx0, b.y0), ImVec2(b.rx1, b.y1), IM_COL32(0, 0, 0, 150));
  }
  // Slide in from the sides: ease out over a third of a second.
  const double t = std::clamp((f.time - g.shown_at) / 0.35, 0.0, 1.0);
  const float slide = (float)(1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t));
  const float shift = (1.0f - slide) * (bar + px(10));
  ImDrawList* draw = dl();
  const int start = draw->VtxBuffer.Size;
  if (!v.active) {
    draw_not_in_room(b);
  } else {
    bool hovered = false;
    if (g.tabs && !g.focus) draw_tabs(b, v, &hovered);
    if (!g.tabs || g.focus || hovered) {
      switch (g.look) {
        case Look::Combined: draw_combined(b, v, false); break;
        case Look::Corners: draw_combined(b, v, true); break;
        case Look::Card: draw_card(b, v, false); break;
        case Look::Ring: draw_card(b, v, true); break;
        case Look::Split: draw_split(b, v); break;
      }
    }
  }
  if (g.page != Page::None) page_box(b.rx0 + px(6), b.y0 + px(10), b.rx1 - b.rx0 - px(12), v);
  // Move what was just drawn: the left half to the left, the right half to the right.
  if (shift > 0.5f)
    for (int i = start; i < draw->VtxBuffer.Size; ++i) {
      ImDrawVert& vert = draw->VtxBuffer[i];
      vert.pos.x += vert.pos.x < f.display_w * .5f ? -shift : shift;
    }
}

}  // namespace arena_panel

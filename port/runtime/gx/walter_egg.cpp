// The character select easter egg (walter_egg.h): the state the game thread feeds and the drawing.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "walter_egg.h"
#include "walter_egg_picture.h"
#include "host.h"
#include "test_env.h"
#include "render_options.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace walter_egg {
namespace {

// ---- game thread
Detector g_detectors[4];
std::atomic<uint32_t> g_css_retrace{0};    // the last retrace a character select frame ran (0 never)
std::atomic<bool> g_on{false};              // Walter is out
std::atomic<uint32_t> g_toggles{0};         // codes entered (the drawing thread notices a new one)
std::atomic<uint8_t> g_sound{0};            // a menu sound the game should play on its next CSS frame

bool css_visible_now() {
  const uint32_t last = g_css_retrace.load(std::memory_order_relaxed);
  return last != 0 && host::retrace_count() - last <= 4;
}

// ---- drawing thread
struct Flight {
  bool flying = false;
  uint32_t toggles_seen = 0;
  Launch path;
  double start_time = 0;      // seconds at step 0
  double steps_per_second = 60.0;
  float box_w = 0, box_h = 0; // the travel range it was launched for
  long long last_step = 0;
  int bounces = 0;            // wall meetings so far (the tint)
  double squash_x_at = -1, squash_y_at = -1;
  double corner_at = -1;      // the celebration's start
  long long relaunch_at = -1;
  Rng rng;
} g_flight;

ImTextureData* g_texture = nullptr;
ImGuiContext* g_texture_context = nullptr;

ImTextureData* picture() {
  if (g_texture && g_texture_context != ImGui::GetCurrentContext()) {
    if (g_texture->Status == ImTextureStatus_Destroyed || g_texture->TexID == ImTextureID_Invalid)
      g_texture->Status = ImTextureStatus_WantCreate;
    ImGui::RegisterUserTexture(g_texture);
    g_texture_context = ImGui::GetCurrentContext();
  }
  if (g_texture) return g_texture;
  g_texture = IM_NEW(ImTextureData)();
  g_texture->Create(ImTextureFormat_RGBA32, kPictureSize, kPictureSize);
  uint32_t* out = reinterpret_cast<uint32_t*>(g_texture->GetPixels());
  size_t at = 0;
  for (uint32_t i = 0; i + 1 < kPictureRuns * 2 && at < (size_t)kPictureSize * kPictureSize; i += 2)
    for (uint32_t n = 0; n < kPicture[i] && at < (size_t)kPictureSize * kPictureSize; ++n) out[at++] = kPicture[i + 1];
  g_texture->UsedRect = ImTextureRect{0, 0, (unsigned short)kPictureSize, (unsigned short)kPictureSize};
  ImGui::RegisterUserTexture(g_texture);
  g_texture_context = ImGui::GetCurrentContext();
  return g_texture;
}

// The DVD logo's colours, one per bounce.
constexpr ImU32 kTints[] = {IM_COL32(255, 255, 255, 255), IM_COL32(255, 120, 120, 255), IM_COL32(120, 200, 255, 255),
                            IM_COL32(255, 225, 90, 255), IM_COL32(140, 255, 140, 255), IM_COL32(220, 140, 255, 255),
                            IM_COL32(255, 170, 80, 255)};

// MELEE_TEST_EGG_SHOTS=1 (tests): a picture every 2 seconds while he is out and a burst of 8 on a
// corner (the F2 capture path, capture/blink_N.ppm in the run folder), each one logged.
bool shots_on() {
  static const bool on = [] { const char* s = host::test_env::getenv("MELEE_TEST_EGG_SHOTS"); return s && *s == '1'; }();
  return on;
}
void shot(const char* why, unsigned frames) {
  if (!shots_on()) return;
  gx::gx_capture_request_set(frames);
  host::log("walter egg: shot %s (%u)", why, frames);
}

int aimed_override() {
  // MELEE_TEST_EGG_AIMED=1 / 0 (tests): every launch aimed / none aimed; unset: about one in five.
  static const int v = [] { const char* s = host::test_env::getenv("MELEE_TEST_EGG_AIMED"); return s ? (*s == '1' ? 1 : 0) : -1; }();
  return v;
}

void new_launch(Flight& f, float w, float h, double now, double from_x, double from_y) {
  const int force = aimed_override();
  const bool aimed = force >= 0 ? force == 1 : f.rng.next() % 5 == 0;
  // About a sixth of the picture's height a second, the screensaver's pace.
  const double speed = std::max(1.0, (double)h / 0.8 / 60.0 * 0.22);
  f.path = launch(w, h, speed, aimed, f.rng, from_x, from_y);
  f.box_w = w; f.box_h = h;
  f.start_time = now; f.last_step = 0; f.relaunch_at = -1; f.corner_at = -1;
  host::log("walter egg: launch %s%s", f.path.corner_step >= 0 ? "aimed at a corner, step " : "unaimed",
            f.path.corner_step >= 0 ? std::to_string(f.path.corner_step).c_str() : "");
}

}  // namespace

void watch_pads(const host::PadState pads[4]) {
  if (!css_visible_now()) {
    if (g_on.exchange(false)) host::log("walter egg: off (left the character select)");
    for (Detector& d : g_detectors) d = Detector{};
    return;
  }
  for (int p = 0; p < 4; ++p) {
    if (pads[p].err != 0) { g_detectors[p].last = 0; continue; }
    if (g_detectors[p].feed(pads[p].button)) {
      const bool on = !g_on.load();
      g_on.store(on);
      g_toggles.fetch_add(1);
      host::log("walter egg: %s (code on port %d)", on ? "on" : "off", p + 1);
    }
  }
}

uint8_t css_frame() {
  g_css_retrace.store(std::max<uint32_t>(1, host::retrace_count()), std::memory_order_relaxed);
  return g_sound.exchange(0);
}

bool active() { return g_on.load() && css_visible_now(); }

void draw(const Frame& fr) {
  Flight& f = g_flight;
  if (!active()) { f.flying = false; return; }
  const float gw = fr.game_x1 - fr.game_x0, gh = fr.game_y1 - fr.game_y0;
  if (gw < 50 || gh < 50) return;
  const float size = std::round(gh * 0.2f);
  const float w = gw - size, h = gh - size;
  const double now = fr.time;
  const uint32_t toggles = g_toggles.load();
  if (!f.flying || toggles != f.toggles_seen) {
    f.flying = true; f.toggles_seen = toggles; f.bounces = 0;
    f.rng.s ^= (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() | 1;
    new_launch(f, w, h, now, -1, -1);
  } else if (std::abs(w - f.box_w) > 0.5f || std::abs(h - f.box_h) > 0.5f) {
    // The window changed size: carry on from the same spot of the new picture, a fresh path.
    const double t = (now - f.start_time) * f.steps_per_second;
    const double px = f.path.x(t) / std::max(1.0, f.path.w) * w, py = f.path.y(t) / std::max(1.0, f.path.h) * h;
    new_launch(f, w, h, now, std::clamp(px, 0.0, (double)w), std::clamp(py, 0.0, (double)h));
  }
  double t = (now - f.start_time) * f.steps_per_second;
  const long long step = (long long)std::floor(t);
  for (long long s = f.last_step + 1; s <= step; ++s) {
    const int hit = f.path.hits(s);
    if (!hit) continue;
    ++f.bounces;
    const double at = f.start_time + s / f.steps_per_second;
    if (hit & 1) f.squash_x_at = at;
    if (hit & 2) f.squash_y_at = at;
    if (hit == 3) {
      f.corner_at = at;
      f.relaunch_at = s + 45;
      g_sound.store(1);   // the character select's own select sound
      shot("corner", 8);
      host::log("walter egg: corner at step %lld (%s)", s, f.path.corner_step == s ? "aimed" : "unaimed");
    }
  }
  f.last_step = std::max(f.last_step, step);
  {
    static double last_shot = -10;
    if (now - last_shot > 2.0 && !(f.corner_at >= 0 && now - f.corner_at < 0.75)) { last_shot = now; shot("flying", 1); }
  }
  if (f.relaunch_at >= 0 && step >= f.relaunch_at) {
    new_launch(f, w, h, now, f.path.x(t), f.path.y(t));
    t = 0;
  } else if (f.path.corner_step < 0 && step > 12000) {
    new_launch(f, w, h, now, f.path.x(t), f.path.y(t));   // past the checked stretch: a new path
    t = 0;
  }
  // During a celebration he holds the corner and spins.
  const bool celebrating = f.corner_at >= 0 && now - f.corner_at < 0.75;
  const double ct = celebrating ? (f.corner_at - f.start_time) * f.steps_per_second : t;
  const float x = fr.game_x0 + (float)f.path.x(ct), y = fr.game_y0 + (float)f.path.y(ct);

  // Squash on a wall (a short ease back), wobble while moving, a whole turn in a corner.
  auto squash = [&](double at) { const double a = now - at; return a >= 0 && a < 0.18 ? (float)(1.0 - a / 0.18) : 0.0f; };
  const float sx_ = squash(f.squash_x_at), sy_ = squash(f.squash_y_at);
  const float scale_x = 1.0f - 0.22f * sx_ + 0.12f * sy_;
  const float scale_y = 1.0f - 0.22f * sy_ + 0.12f * sx_;
  float angle = (float)(0.09 * std::sin(now * 7.0));
  if (celebrating) angle += (float)((now - f.corner_at) / 0.75 * 6.2831853);
  const ImU32 tint = kTints[f.bounces % (int)(sizeof kTints / sizeof kTints[0])];

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->PushClipRect(ImVec2(fr.game_x0, fr.game_y0), ImVec2(fr.game_x1, fr.game_y1), true);
  if (celebrating) {   // the flash
    const float a = (float)(1.0 - (now - f.corner_at) / 0.75);
    dl->AddRectFilled(ImVec2(fr.game_x0, fr.game_y0), ImVec2(fr.game_x1, fr.game_y1), IM_COL32(255, 255, 255, (int)(150 * a * a)));
  }
  const float cx = x + size * 0.5f, cy = y + size * 0.5f;
  const float hx = size * 0.5f * scale_x, hy = size * 0.5f * scale_y;
  const float c = std::cos(angle), s = std::sin(angle);
  auto corner_pt = [&](float dx, float dy) { return ImVec2(cx + dx * c - dy * s, cy + dx * s + dy * c); };
  if (ImTextureData* tex = picture())
    dl->AddImageQuad(tex->GetTexRef(), corner_pt(-hx, -hy), corner_pt(hx, -hy), corner_pt(hx, hy), corner_pt(-hx, hy),
                     ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
  dl->PopClipRect();
}

}  // namespace walter_egg

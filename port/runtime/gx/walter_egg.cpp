// The character select easter egg (walter_egg.h): the state the game thread feeds and the drawing.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "walter_egg.h"
#include "walter_egg_picture.h"
#include "host.h"
#include "test_env.h"
#include "render_options.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "user_texture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace walter_egg {
namespace {

// ---- game thread
Detector g_detectors[4];
std::atomic<uint32_t> g_css_retrace{0};    // the last retrace a character select frame ran (0 never)
std::atomic<bool> g_on{false};              // faces are out
std::atomic<uint32_t> g_sent[2];            // codes entered, ever: [0] Bailey (the code ended B A), [1] Walter (A B)
std::atomic<uint32_t> g_left_sent[2];       // g_sent when the character select was last left with faces out
std::atomic<uint32_t> g_visit{0};           // times the character select was left with faces out (they all go)
std::atomic<uint8_t> g_sound{0};            // a menu sound the game should play on its next CSS frame
std::atomic<uint32_t> g_hand[4];            // each port's character select hand: x10 << 16 | y10 (the game's units)
std::atomic<uint32_t> g_hand_at[4];         // the retrace it was last reported (0: not on this visit)
std::atomic<bool> g_a_held[4];              // each port's A button
uint32_t g_codes_this_visit = 0;

bool css_visible_now() {
  const uint32_t last = g_css_retrace.load(std::memory_order_relaxed);
  return last != 0 && host::retrace_count() - last <= 4;
}

// ---- drawing thread
// A hand holding a face (A pressed with the hand on it): the spot it took hold of, in the face's
// own square (0..1), and how far it has pulled that spot since. Let go, the pull swings back and the
// face flies off the other way.
struct Grab {
  int port = -1;                    // the port holding on; -1 nobody
  float u = 0.5f, v = 0.5f;
  float hand_x0 = 0, hand_y0 = 0;   // where the hand was when it took hold (pixels)
  float pull_x = 0, pull_y = 0;     // pixels
  double released_at = -1;          // the swing back: since when, and from what pull
  float released_x = 0, released_y = 0;
};

constexpr double kStepsPerSecond = 60.0;   // a face's own pace along a path

// One face out. Each one flies, bounces, turns and can be held on its own.
struct Face {
  int which = 0;              // 0 Bailey, 1 Walter
  Launch path;
  double t = 0;               // steps along this path so far
  float box_w = 0, box_h = 0; // the travel range it was launched for
  long long last_step = 0;
  int bounces = 0;            // wall meetings so far (the outline's colour)
  double squash_x_at = -1, squash_y_at = -1;
  double corner_at = -1;      // the celebration's start
  long long corner_step = 0;  // and the step it holds for it
  double out_at = 0;          // when it came out (it starts to turn ten seconds later)
  double held = 0;            // seconds a hand has held it so far: its own clock stands still meanwhile
  double slung_at = -1;       // a slingshot: when and how hard (0..1)
  float slung_power = 0;
  double turn = 0;            // how far it has turned from upright (radians) and how fast it is turning (a
  double spin = 0;            // second): it starts upright and still; walls nudge it, a slingshot sets it going,
                              // it winds down, and it settles upright again
  bool slung = false;         // tests: a pinned face flies once it has been slung
  Grab grab;
  Rng rng;
  // This frame: its own clock, size and where it is drawn.
  double now = 0;
  float size = 0, x = 0, y = 0, cx = 0, cy = 0, scale_x = 1, scale_y = 1, angle = 0;
  bool celebrating = false;
};

// As many as the codes ask for, up to this many at once: then the oldest one leaves for each new one,
// so codes can be entered for as long as anyone likes and the picture never holds more than this.
constexpr size_t kMaxFaces = 200;
std::vector<Face> g_faces;
uint32_t g_seen_sent[2] = {0, 0};
uint32_t g_seen_visit = 0;
bool g_a_was[4] = {false, false, false, false};
double g_last_frame = 0;
double g_flash_at = -10, g_sound_at = -10;   // the last corner's flash and sound (frame time)
uint64_t g_serial = 0;
bool g_full_logged = false;

constexpr int kUpscale = 4;   // the texture is the 128 px picture enlarged smoothly, so it never shows its pixels
constexpr int kGlowPad = 48;  // the outline's texture: the face's shape with this much room around it (texture pixels)
struct Pictures { ImTextureData* face = nullptr; ImTextureData* glow = nullptr; };
Pictures g_pictures[2];
ImGuiContext* g_face_context[2] = {nullptr, nullptr};
ImGuiContext* g_glow_context[2] = {nullptr, nullptr};

// The compiled-in picture (run-length coded), enlarged kUpscale times with a Catmull-Rom filter on
// colour premultiplied by alpha: smooth at any window size, and the cut-out's edge takes no fringe.
// With it, the same shape blurred, in white: drawn in a bright colour behind the face it is its outline.
const Pictures& pictures(int which) {
  which = which == 1 ? 1 : 0;
  Pictures& p = g_pictures[which];
  if (p.face) {
    gx::keep_user_texture(p.face, g_face_context[which]);
    gx::keep_user_texture(p.glow, g_glow_context[which]);
    return p;
  }
  const uint32_t* runs = which == 1 ? kPictureWalter : kPictureBailey;
  const uint32_t run_count = which == 1 ? kPictureWalterRuns : kPictureBaileyRuns;
  const int n = kPictureSize, m = kPictureSize * kUpscale;
  std::vector<float> src((size_t)n * n * 4, 0.0f);   // premultiplied r g b, then a
  size_t at = 0;
  for (uint32_t i = 0; i + 1 < run_count * 2 && at < (size_t)n * n; i += 2)
    for (uint32_t k = 0; k < runs[i] && at < (size_t)n * n; ++k, ++at) {
      const uint32_t px = runs[i + 1];
      const float a = (float)(px >> 24) / 255.0f;
      src[at * 4 + 0] = (float)(px & 0xFF) / 255.0f * a;
      src[at * 4 + 1] = (float)((px >> 8) & 0xFF) / 255.0f * a;
      src[at * 4 + 2] = (float)((px >> 16) & 0xFF) / 255.0f * a;
      src[at * 4 + 3] = a;
    }
  // The four taps and weights for each output coordinate (the same across and down).
  std::vector<int> tap((size_t)m * 4);
  std::vector<float> weight((size_t)m * 4);
  for (int o = 0; o < m; ++o) {
    const float pos = ((float)o + 0.5f) / (float)kUpscale - 0.5f;
    const int base = (int)std::floor(pos);
    const float t = pos - (float)base;
    const float w[4] = {((-t + 2.0f) * t - 1.0f) * t * 0.5f, ((3.0f * t - 5.0f) * t * t + 2.0f) * 0.5f,
                        ((-3.0f * t + 4.0f) * t + 1.0f) * t * 0.5f, (t - 1.0f) * t * t * 0.5f};
    for (int k = 0; k < 4; ++k) { tap[(size_t)o * 4 + k] = std::clamp(base - 1 + k, 0, n - 1); weight[(size_t)o * 4 + k] = w[k]; }
  }
  std::vector<float> wide((size_t)m * n * 4);   // enlarged across
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < m; ++x)
      for (int c = 0; c < 4; ++c) {
        float v = 0;
        for (int k = 0; k < 4; ++k) v += weight[(size_t)x * 4 + k] * src[((size_t)y * n + tap[(size_t)x * 4 + k]) * 4 + c];
        wide[((size_t)y * m + x) * 4 + c] = v;
      }
  ImTextureData* face = IM_NEW(ImTextureData)();
  face->Create(ImTextureFormat_RGBA32, m, m);
  uint32_t* out = reinterpret_cast<uint32_t*>(face->GetPixels());
  const int gm = m + 2 * kGlowPad;
  std::vector<float> shape((size_t)gm * gm, 0.0f), pass((size_t)gm * gm, 0.0f);
  for (int y = 0; y < m; ++y)
    for (int x = 0; x < m; ++x) {
      float v[4] = {0, 0, 0, 0};
      for (int k = 0; k < 4; ++k)
        for (int c = 0; c < 4; ++c) v[c] += weight[(size_t)y * 4 + k] * wide[((size_t)tap[(size_t)y * 4 + k] * m + x) * 4 + c];
      const float a = std::clamp(v[3], 0.0f, 1.0f);
      uint32_t px = 0;
      if (a > 0.004f) {
        const auto byte = [a](float premultiplied) { return (uint32_t)(std::clamp(premultiplied / a, 0.0f, 1.0f) * 255.0f + 0.5f); };
        px = (uint32_t)(a * 255.0f + 0.5f) << 24 | byte(v[2]) << 16 | byte(v[1]) << 8 | byte(v[0]);
      }
      out[(size_t)y * m + x] = px;
      shape[(size_t)(y + kGlowPad) * gm + (x + kGlowPad)] = a;
    }
  face->UsedRect = ImTextureRect{0, 0, (unsigned short)m, (unsigned short)m};
  // The outline: the shape blurred (three box passes, close to a Gaussian), then made strong near the edge.
  constexpr int kRadius = 9;
  for (int round = 0; round < 3; ++round) {
    for (int y = 0; y < gm; ++y) {
      float sum = 0;
      for (int x = -kRadius; x <= kRadius; ++x) sum += shape[(size_t)y * gm + std::clamp(x, 0, gm - 1)];
      for (int x = 0; x < gm; ++x) {
        pass[(size_t)y * gm + x] = sum / (float)(2 * kRadius + 1);
        sum += shape[(size_t)y * gm + std::min(x + kRadius + 1, gm - 1)] - shape[(size_t)y * gm + std::max(x - kRadius, 0)];
      }
    }
    for (int x = 0; x < gm; ++x) {
      float sum = 0;
      for (int y = -kRadius; y <= kRadius; ++y) sum += pass[(size_t)std::clamp(y, 0, gm - 1) * gm + x];
      for (int y = 0; y < gm; ++y) {
        shape[(size_t)y * gm + x] = sum / (float)(2 * kRadius + 1);
        sum += pass[(size_t)std::min(y + kRadius + 1, gm - 1) * gm + x] - pass[(size_t)std::max(y - kRadius, 0) * gm + x];
      }
    }
  }
  ImTextureData* glow = IM_NEW(ImTextureData)();
  glow->Create(ImTextureFormat_RGBA32, gm, gm);
  uint32_t* gout = reinterpret_cast<uint32_t*>(glow->GetPixels());
  for (size_t i = 0; i < (size_t)gm * gm; ++i)
    gout[i] = (uint32_t)(std::clamp(shape[i] * 2.6f, 0.0f, 1.0f) * 255.0f + 0.5f) << 24 | 0x00FFFFFFu;
  glow->UsedRect = ImTextureRect{0, 0, (unsigned short)gm, (unsigned short)gm};
  p.face = face;
  p.glow = glow;
  gx::keep_user_texture(p.face, g_face_context[which]);
  gx::keep_user_texture(p.glow, g_glow_context[which]);
  return p;
}

// A bright colour per wall, as an outline around the face; the face itself keeps its own colours.
constexpr ImU32 kGlow[] = {IM_COL32(255, 70, 110, 240), IM_COL32(255, 150, 40, 240), IM_COL32(255, 225, 60, 240),
                           IM_COL32(70, 235, 120, 240), IM_COL32(50, 205, 255, 240), IM_COL32(125, 120, 255, 240),
                           IM_COL32(230, 100, 255, 240)};

// Each face's size, of the picture's height: Bailey, Walter.
constexpr float kFaceSize[2] = {0.17f, 0.14f};
// The character select's own units on screen, at the hand's depth: the picture is 57.3 of them tall
// (measured: the hand crosses 25.3 units in 318 of 720p's pixels), its middle is 0, 0, up is positive.
// The hand's position is the glove's corner; its fingertip is 5 units to the right and 1.1 down.
constexpr float kUnitsTall = 57.3f;
constexpr float kHandTipX = 5.0f, kHandTipY = -1.1f;   // from the hand's reported position to the fingertip (units)

// MELEE_TEST_EGG_SHOTS=1 (tests): a picture every 2 seconds while faces are out and a burst of 8 on a
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
// MELEE_TEST_EGG_PIN=1 (tests): a face does not fly; it sits where port 1's hand was when it came out,
// so a script can take hold of it (a slingshot then sets it flying). MELEE_TEST_EGG_HANDMARK=1
// (tests): a small cross where each hand's fingertip is taken to be.
bool test_flag(const char* name) {
  const char* s = host::test_env::getenv(name);
  return s && *s == '1';
}
bool pinned() { static const bool on = test_flag("MELEE_TEST_EGG_PIN"); return on; }
bool hand_marks() { static const bool on = test_flag("MELEE_TEST_EGG_HANDMARK"); return on; }
// MELEE_TEST_EGG_EACH=n (tests): each code sends n faces (1 to kMaxFaces), so a run reaches the limit fast.
size_t faces_per_code() {
  static const size_t n = [] {
    const char* s = host::test_env::getenv("MELEE_TEST_EGG_EACH");
    const long v = s ? std::strtol(s, nullptr, 10) : 1;
    return (size_t)std::clamp<long>(v, 1, (long)kMaxFaces);
  }();
  return n;
}

int aimed_override() {
  // MELEE_TEST_EGG_AIMED=1 / 0 (tests): every launch aimed / none aimed; unset: about one in five.
  static const int v = [] { const char* s = host::test_env::getenv("MELEE_TEST_EGG_AIMED"); return s ? (*s == '1' ? 1 : 0) : -1; }();
  return v;
}

// The first few faces log their paths and corners; past that the log would only fill up.
bool chatty() { return g_faces.size() <= 4; }

// A face's pace along a path: about a sixth of the picture's height a second, the screensaver's.
double step_length(float h) { return std::max(1.0, (double)h / 0.8 / 60.0 * 0.22); }

void new_launch(Face& f, float w, float h, double from_x, double from_y) {
  const int force = aimed_override();
  const bool aimed = force >= 0 ? force == 1 : f.rng.next() % 5 == 0;
  f.path = launch(w, h, step_length(h), aimed, f.rng, from_x, from_y);
  f.box_w = w; f.box_h = h;
  f.t = 0; f.last_step = 0; f.corner_at = -1;
  if (chatty())
    host::log("walter egg: launch %s%s", f.path.corner_step >= 0 ? "aimed at a corner, step " : "unaimed",
              f.path.corner_step >= 0 ? std::to_string(f.path.corner_step).c_str() : "");
}

// Let go of a pull: the face flies off the other way, as from a slingshot, the faster the further it
// was pulled (power 0..1), and eases back to its own pace as it bounces around.
void sling(Face& f, float w, float h, double now, double from_x, double from_y, float dir_x, float dir_y, float power) {
  const double length = step_length(h);
  Launch l;
  l.w = w > 1 ? w : 1; l.h = h > 1 ? h : 1;
  l.mirror_x = dir_x < 0; l.mirror_y = dir_y < 0;
  l.vx = std::abs(dir_x) * length; l.vy = std::abs(dir_y) * length;
  from_x = std::clamp(from_x, 0.0, l.w); from_y = std::clamp(from_y, 0.0, l.h);
  l.x0 = l.mirror_x ? l.w - from_x : from_x;
  l.y0 = l.mirror_y ? l.h - from_y : from_y;
  l.corner_step = -1;
  f.path = l; f.box_w = w; f.box_h = h;
  f.t = 0; f.last_step = 0; f.corner_at = -1;
  f.slung_at = now; f.slung_power = power; f.slung = true;
  f.spin += (dir_x >= 0 ? 1.0 : -1.0) * (2.0 + 4.0 * power);   // and it sets it spinning
  host::log("walter egg: slung %.2f %.2f, power %.2f", dir_x, dir_y, power);
}

// A corner: the character select's own select sound and a flash over the picture. With many faces
// out corners come often, so the sound waits a moment between two and a flash at least 1.5 s.
void corner_cheer(double time) {
  if (time - g_sound_at >= 0.3) { g_sound_at = time; g_sound.store(1); }
  if (time - g_flash_at >= 1.5) g_flash_at = time;
}

struct Hands {
  const Frame* fr = nullptr;
  // Each hand's fingertip on screen.
  bool at(int port, ImVec2* out) const {
    if (!g_hand_at[port].load()) return false;
    const float unit = (fr->game_y1 - fr->game_y0) / kUnitsTall;
    const uint32_t packed = g_hand[port].load();
    const float hx = (float)(int16_t)(packed >> 16) * 0.1f, hy = (float)(int16_t)(packed & 0xFFFF) * 0.1f;
    out->x = (fr->game_x0 + fr->game_x1) * 0.5f + (hx + kHandTipX) * unit;
    out->y = (fr->game_y0 + fr->game_y1) * 0.5f - (hy + kHandTipY) * unit;
    return true;
  }
};

// A new face of this kind; at the limit, the oldest one nobody holds leaves to make room.
void add_face(int which, const Frame& fr, const Hands& hands) {
  if (g_faces.size() >= kMaxFaces) {
    auto oldest = std::find_if(g_faces.begin(), g_faces.end(), [](const Face& f) { return f.grab.port < 0; });
    g_faces.erase(oldest == g_faces.end() ? g_faces.begin() : oldest);
    if (!g_full_logged) {
      g_full_logged = true;
      host::log("walter egg: %zu out, the oldest leaves for each new one", kMaxFaces);
    }
  }
  const float gw = fr.game_x1 - fr.game_x0, gh = fr.game_y1 - fr.game_y0;
  Face f;
  f.which = which == 1 ? 1 : 0;
  // Faces sent on the same frame each take their own seed, so each takes its own path.
  uint64_t seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() + 0x9E3779B97F4A7C15ull * ++g_serial;
  seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
  seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
  f.rng.s = (seed ^ (seed >> 31)) | 1;
  f.size = std::round(gh * kFaceSize[f.which]);
  f.out_at = fr.time;
  const float w = gw - f.size, h = gh - f.size;
  ImVec2 hand;
  if (pinned() && hands.at(0, &hand))
    new_launch(f, w, h, std::clamp(hand.x - fr.game_x0 - f.size * 0.5f, 0.0f, w), std::clamp(hand.y - fr.game_y0 - f.size * 0.5f, 0.0f, h));
  else
    new_launch(f, w, h, -1, -1);
  g_faces.push_back(f);
}

// One face's travel this frame (its clock, path, walls, corners and turn), then where it is drawn.
void move(Face& f, const Frame& fr, double frame_time) {
  const float gh = fr.game_y1 - fr.game_y0, gw = fr.game_x1 - fr.game_x0;
  f.size = std::round(gh * kFaceSize[f.which]);
  const float size = f.size, w = gw - size, h = gh - size;
  // Its own clock: it stands still while a hand holds it.
  if (f.grab.port >= 0) f.held += frame_time;
  const double now = fr.time - f.held;
  f.now = now;
  if (std::abs(w - f.box_w) > 0.5f || std::abs(h - f.box_h) > 0.5f) {
    // The window changed size: carry on from the same spot of the new picture, a fresh path.
    const double px = f.path.x(f.t) / std::max(1.0, f.path.w) * w, py = f.path.y(f.t) / std::max(1.0, f.path.h) * h;
    new_launch(f, w, h, std::clamp(px, 0.0, (double)w), std::clamp(py, 0.0, (double)h));
  }
  // A celebration over: on from the corner, a new path.
  if (f.corner_at >= 0 && now - f.corner_at >= 0.75)
    new_launch(f, w, h, f.path.x((double)f.corner_step), f.path.y((double)f.corner_step));
  // It moves unless a hand holds it or it is celebrating in a corner; faster just after a slingshot.
  double pace = 1.0;
  if (f.slung_at >= 0) pace += 9.0 * f.slung_power * std::exp(-(now - f.slung_at) / 1.1);
  const bool still = (pinned() && !f.slung) || f.grab.port >= 0 || f.corner_at >= 0;
  if (!still) f.t += frame_time * kStepsPerSecond * pace;
  const long long step = (long long)std::floor(f.t);
  for (long long s = f.last_step + 1; s <= step; ++s) {
    f.last_step = s;
    const int hit = f.path.hits(s);
    if (!hit) continue;
    ++f.bounces;
    if (hit & 1) f.squash_x_at = now;
    if (hit & 2) f.squash_y_at = now;
    // A wall turns some of its travel into spin, as it does to a ball: which way depends on the wall
    // and on which way it was sliding along it, how much on how fast it came in. Not in its first ten
    // seconds out.
    if (const double gain = std::clamp((now - f.out_at - 10.0) / 2.0, 0.0, 1.0); gain > 0) {
      const double x0 = f.path.x((double)s - 1.0), y0 = f.path.y((double)s - 1.0), x1 = f.path.x((double)s), y1 = f.path.y((double)s);
      double kick = 0;
      if (hit & 1) kick -= (y1 >= y0 ? 1.0 : -1.0) * (x1 > f.path.w * 0.5 ? 1.0 : -1.0);
      if (hit & 2) kick += (x1 >= x0 ? 1.0 : -1.0) * (y1 > f.path.h * 0.5 ? 1.0 : -1.0);
      f.spin = f.spin * 0.5 + kick * 1.4 * std::min(pace, 2.0) * gain;
    }
    if (hit == 3) {
      // A corner: it holds it and spins.
      f.corner_at = now; f.corner_step = s; f.t = (double)s;
      corner_cheer(fr.time);
      shot("corner", 8);
      if (chatty()) host::log("walter egg: corner at step %lld (%s)", s, f.path.corner_step == s ? "aimed" : "unaimed");
      break;
    }
  }
  f.celebrating = f.corner_at >= 0;
  if (!f.celebrating && f.path.corner_step < 0 && !f.slung && step > 12000)
    new_launch(f, w, h, f.path.x(f.t), f.path.y(f.t));   // past the checked stretch: a new path
  const double ct = f.celebrating ? (double)f.corner_step : f.t;
  f.x = fr.game_x0 + (float)f.path.x(ct);
  f.y = fr.game_y0 + (float)f.path.y(ct);

  // Squash on a wall (a short ease back), wobble while moving, a whole turn in a corner.
  auto squash = [&](double at) { const double a = now - at; return a >= 0 && a < 0.18 ? (float)(1.0 - a / 0.18) : 0.0f; };
  const float sx_ = squash(f.squash_x_at), sy_ = squash(f.squash_y_at);
  f.scale_x = 1.0f - 0.22f * sx_ + 0.12f * sy_;
  f.scale_y = 1.0f - 0.22f * sy_ + 0.12f * sx_;
  // Its spin coasts and winds down (never faster than a turn a second), and once it is slow it eases
  // back upright.
  if (f.grab.port < 0) {
    f.spin = std::clamp(f.spin, -6.0, 6.0) * std::exp(-frame_time / 3.0);
    f.turn = std::remainder(f.turn + f.spin * frame_time, 6.283185307179586);   // -pi..pi: upright is 0
    if (std::abs(f.spin) < 1.2) f.turn *= std::exp(-frame_time / 0.8);
  }
  f.angle = (float)(0.09 * std::sin(now * 7.0) + f.turn);
  if (f.celebrating) f.angle += (float)((now - f.corner_at) / 0.75 * 6.2831853);
  f.cx = f.x + size * 0.5f;
  f.cy = f.y + size * 0.5f;
}

// A held face follows its hand; let go, it is slung.
void follow_hand(Face& f, const Frame& fr, const Hands& hands) {
  Grab& g = f.grab;
  if (g.port < 0) return;
  ImVec2 hand;
  if (!g_a_held[g.port].load() || !hands.at(g.port, &hand)) {
    host::log("walter egg: port %d let go (pulled %.0f %.0f px)", g.port + 1, g.pull_x, g.pull_y);
    g.released_at = fr.time; g.released_x = g.pull_x; g.released_y = g.pull_y;
    g.port = -1;
    const float reach = std::sqrt(g.pull_x * g.pull_x + g.pull_y * g.pull_y);
    const float w = (fr.game_x1 - fr.game_x0) - f.size, h = (fr.game_y1 - fr.game_y0) - f.size;
    if (reach > f.size * 0.2f)   // a real pull: the slingshot
      sling(f, w, h, f.now, f.x - fr.game_x0, f.y - fr.game_y0, -g.pull_x / reach, -g.pull_y / reach, std::min(1.0f, reach / (f.size * 1.6f)));
  } else {
    float px = hand.x - g.hand_x0, py = hand.y - g.hand_y0;
    const float reach = std::sqrt(px * px + py * py), most = f.size * 1.6f;
    if (reach > most) { px *= most / reach; py *= most / reach; }
    g.pull_x = px; g.pull_y = py;
  }
}

void draw_face(ImDrawList* dl, Face& f, const Frame& fr, const Pictures& pic) {
  Grab& g = f.grab;
  const float size = f.size, scale_x = f.scale_x, scale_y = f.scale_y, cx = f.cx, cy = f.cy;
  const float c = std::cos(f.angle), s = std::sin(f.angle);
  float pull_x = 0, pull_y = 0;
  if (g.port >= 0) { pull_x = g.pull_x; pull_y = g.pull_y; }
  else if (g.released_at >= 0) {
    const double a = fr.time - g.released_at;
    if (a < 1.4) {
      const float k = (float)(std::exp(-a * 4.5) * std::cos(a * 22.0));   // it swings back and settles
      pull_x = g.released_x * k; pull_y = g.released_y * k;
    } else {
      g.released_at = -1;
    }
  }
  const float pull_length = std::sqrt(pull_x * pull_x + pull_y * pull_y);
  const bool pulled = pull_length > 0.01f;
  const float toward_x = pulled ? pull_x / pull_length : 0, toward_y = pulled ? pull_y / pull_length : 0;
  const float reach2 = (0.34f * size) * (0.34f * size);

  // Its square as a grid, so a pull stretches it from the spot the hand holds. What is behind that
  // spot (against the pull) follows less the further back it is, so it stretches; what is ahead of it
  // is pushed along whole. Every point keeps its order along the pull, so it never folds over itself.
  const auto place = [&](float u, float v) {
    const float dx = (u - 0.5f) * size * scale_x, dy = (v - 0.5f) * size * scale_y;
    ImVec2 p(cx + dx * c - dy * s, cy + dx * s + dy * c);
    if (pulled) {
      const float ox = (u - g.u) * size * scale_x, oy = (v - g.v) * size * scale_y;   // from the held spot, on screen
      const float px = ox * c - oy * s, py = ox * s + oy * c;
      const float along = px * toward_x + py * toward_y, across = -px * toward_y + py * toward_x;
      const float back = along < 0 ? along : 0.0f;
      const float follow = std::exp(-(back * back + across * across) / reach2);
      p.x += pull_x * follow; p.y += pull_y * follow;
    }
    return p;
  };
  // A texture over its square from lo to hi (the outline's reaches a little past it), as a grid when pulled.
  const auto mesh = [&](ImTextureData* tex, float lo, float hi, ImU32 colour) {
    const int cells = pulled ? 16 : 1;
    for (int j = 0; j < cells; ++j)
      for (int i = 0; i < cells; ++i) {
        const float a0 = (float)i / (float)cells, a1 = (float)(i + 1) / (float)cells;
        const float b0 = (float)j / (float)cells, b1 = (float)(j + 1) / (float)cells;
        const float u0 = lo + (hi - lo) * a0, u1 = lo + (hi - lo) * a1, v0 = lo + (hi - lo) * b0, v1 = lo + (hi - lo) * b1;
        dl->AddImageQuad(tex->GetTexRef(), place(u0, v0), place(u1, v0), place(u1, v1), place(u0, v1),
                         ImVec2(a0, b0), ImVec2(a1, b0), ImVec2(a1, b1), ImVec2(a0, b1), colour);
      }
  };
  if (pic.face && pic.glow) {
    const float room = (float)kGlowPad / (float)(kPictureSize * kUpscale);   // the outline's texture reaches this far past its square
    mesh(pic.glow, -room, 1.0f + room, kGlow[f.bounces % (int)(sizeof kGlow / sizeof kGlow[0])]);
    mesh(pic.face, 0.0f, 1.0f, IM_COL32(255, 255, 255, 255));
  }
}

}  // namespace

void watch_pads(const host::PadState pads[4]) {
  if (!css_visible_now()) {
    if (g_on.exchange(false)) {
      // Every face goes. The drawing thread starts the next visit from these counts.
      for (int k = 0; k < 2; ++k) g_left_sent[k].store(g_sent[k].load());
      g_visit.fetch_add(1);
      host::log("walter egg: off (left the character select, %u codes this visit)", g_codes_this_visit);
    }
    g_codes_this_visit = 0;
    for (Detector& d : g_detectors) d = Detector{};
    for (int p = 0; p < 4; ++p) { g_hand_at[p].store(0); g_a_held[p].store(false); }
    return;
  }
  for (int p = 0; p < 4; ++p) {
    if (pads[p].err != 0) { g_detectors[p].last = 0; g_a_held[p].store(false); continue; }
    g_a_held[p].store((pads[p].button & kA) != 0);
    if (const int code = g_detectors[p].feed(pads[p].button)) {
      // Each code sends one more face out: B A a Bailey, A B a Walter.
      const uint8_t which = code == kWalter ? 1 : 0;
      g_sent[which].fetch_add(1);
      g_on.store(true);
      ++g_codes_this_visit;
      host::log("walter egg: on (code on port %d, %s, %u codes this visit)", p + 1, which ? "Walter" : "Bailey", g_codes_this_visit);
    }
  }
}

void set_hand(int port, int16_t x10, int16_t y10) {
  if (port < 0 || port > 3) return;
  g_hand[port].store((uint32_t)(uint16_t)x10 << 16 | (uint16_t)y10);
  g_hand_at[port].store(std::max<uint32_t>(1, host::retrace_count()));
}

uint8_t css_frame() {
  g_css_retrace.store(std::max<uint32_t>(1, host::retrace_count()), std::memory_order_relaxed);
  return g_sound.exchange(0);
}

bool active() { return g_on.load() && css_visible_now(); }

void draw(const Frame& fr) {
  if (!active()) return;
  const float gw = fr.game_x1 - fr.game_x0, gh = fr.game_y1 - fr.game_y0;
  if (gw < 50 || gh < 50) return;
  const Hands hands{&fr};

  // A new visit to the character select starts with none out; then one face for each code since.
  const uint32_t visit = g_visit.load();
  if (visit != g_seen_visit) {
    g_seen_visit = visit;
    g_faces.clear();
    g_full_logged = false;
    for (int k = 0; k < 2; ++k) g_seen_sent[k] = g_left_sent[k].load();
  }
  bool added = false;
  for (int k = 0; k < 2; ++k) {
    const uint32_t sent = g_sent[k].load();
    for (; g_seen_sent[k] != sent; ++g_seen_sent[k])
      for (size_t n = 0; n < faces_per_code(); ++n) { add_face(k, fr, hands); added = true; }
  }
  if (added) {
    for (int p = 0; p < 4; ++p) g_a_was[p] = g_a_held[p].load();   // the A that ended a code takes no hold
    host::log("walter egg: %zu out", g_faces.size());
  }
  const double frame_time = g_last_frame > 0 ? std::clamp(fr.time - g_last_frame, 0.0, 0.25) : 0.0;
  g_last_frame = fr.time;
  if (g_faces.empty()) return;

  bool celebrating = false;
  for (Face& f : g_faces) {
    move(f, fr, frame_time);
    celebrating = celebrating || f.celebrating;
  }
  {
    static double last_shot = -10;
    if (fr.time - last_shot > 2.0 && !celebrating) { last_shot = fr.time; shot("flying", 1); }
  }

  // A hand takes hold with A while its fingertip is on a face (the one on top), pulls as it moves,
  // and lets go with A.
  for (int p = 0; p < 4; ++p) {
    const bool a = g_a_held[p].load();
    const bool pressed = a && !g_a_was[p];
    g_a_was[p] = a;
    ImVec2 hand;
    if (!pressed || !hands.at(p, &hand)) continue;
    if (std::any_of(g_faces.begin(), g_faces.end(), [p](const Face& f) { return f.grab.port == p; })) continue;
    for (size_t i = g_faces.size(); i-- > 0;) {
      Face& f = g_faces[i];
      if (f.grab.port >= 0 || f.celebrating) continue;
      const float c = std::cos(f.angle), s = std::sin(f.angle);
      const float dx = hand.x - f.cx, dy = hand.y - f.cy;
      const float lx = (dx * c + dy * s) / (f.size * f.scale_x), ly = (-dx * s + dy * c) / (f.size * f.scale_y);   // its own square, -0.5..0.5
      if (lx * lx + ly * ly > 0.25f) continue;
      Grab& g = f.grab;
      g.port = p; g.u = lx + 0.5f; g.v = ly + 0.5f;
      g.hand_x0 = hand.x; g.hand_y0 = hand.y; g.pull_x = g.pull_y = 0; g.released_at = -1;
      host::log("walter egg: port %d took hold at %.2f %.2f", p + 1, g.u, g.v);
      break;
    }
  }
  for (Face& f : g_faces) follow_hand(f, fr, hands);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->PushClipRect(ImVec2(fr.game_x0, fr.game_y0), ImVec2(fr.game_x1, fr.game_y1), true);
  if (const double since = fr.time - g_flash_at; since >= 0 && since < 0.75) {   // a corner's flash
    const float a = (float)(1.0 - since / 0.75);
    dl->AddRectFilled(ImVec2(fr.game_x0, fr.game_y0), ImVec2(fr.game_x1, fr.game_y1), IM_COL32(255, 255, 255, (int)(150 * a * a)));
  }
  const Pictures* pics[2] = {nullptr, nullptr};
  for (Face& f : g_faces) {
    if (!pics[f.which]) pics[f.which] = &pictures(f.which);
    draw_face(dl, f, fr, *pics[f.which]);
  }
  if (hand_marks()) {
    for (int p = 0; p < 4; ++p) {
      ImVec2 hand;
      if (!hands.at(p, &hand)) continue;
      dl->AddLine(ImVec2(hand.x - 12, hand.y), ImVec2(hand.x + 12, hand.y), IM_COL32(255, 0, 255, 255), 2.0f);
      dl->AddLine(ImVec2(hand.x, hand.y - 12), ImVec2(hand.x, hand.y + 12), IM_COL32(255, 0, 255, 255), 2.0f);
    }
  }
  dl->PopClipRect();
}

}  // namespace walter_egg

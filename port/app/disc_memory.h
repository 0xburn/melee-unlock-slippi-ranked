// Locate the previously selected disc in local launcher settings.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace disc_memory {
namespace fs = std::filesystem;

// The last "iso=" line of an ini's text, as written (bytes, line ending removed); "" when none.
inline std::string iso_line(const std::string& text) {
  std::string found;
  size_t at = 0;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("iso=", 0) == 0) found = line.substr(4);
    at = end + 1;
  }
  return found;
}

inline std::string read_iso(const fs::path& ini) {
  std::ifstream f(ini, std::ios::binary);
  if (!f) return {};
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return iso_line(text);
}

// standalone.ini is written as UTF-8; launcher.ini in the system code page (GetFullPathNameA).
enum class Encoding { Utf8, Ansi };
// full_check: the disc may be from a mod run, so the header is not enough (see above).
struct Place { fs::path ini; Encoding encoding; bool full_check = true; };

inline fs::path prior_standalone_dir(const fs::path& local_app_data) {
  return local_app_data / "MeleeUnlocked-YouGame";
}

// The Windows game executable used this older folder even when started on its own. Keep a
// returning player's complete profile together, including controller settings, saves and replays.
// Merely opening the launcher creates launcher.ini, so that file does not identify a game profile.
inline fs::path standalone_dir(const fs::path& local_app_data) {
  if (local_app_data.empty()) return {};
  const auto current = local_app_data / "MeleeUnlocked";
  const auto prior = prior_standalone_dir(local_app_data);
  auto has_profile = [](const fs::path& dir) {
    std::error_code ec;
    for (const auto* marker : {"standalone.ini", "port-settings.ini", "User/Slippi/user.json"})
      if (fs::is_regular_file(dir / marker, ec)) return true;
    const auto card = dir / "User" / "GC" / "CardA";
    for (const auto& folder : {card, card / "VanillaOnly"}) {
      for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code kind;
        if (it->path().extension() == ".gci" && it->is_regular_file(kind)) return true;
      }
    }
    return false;
  };
  return !has_profile(current) && has_profile(prior) ? prior : current;
}

inline std::vector<Place> places(const fs::path& local_app_data) {
  if (local_app_data.empty()) return {};
  const auto current = local_app_data / "MeleeUnlocked";
  const auto prior = prior_standalone_dir(local_app_data);
  const auto profile = standalone_dir(local_app_data);
  return {{profile / "standalone.ini", Encoding::Utf8, false},
          {current / "launcher.ini", Encoding::Ansi, true},
          {(profile == prior ? current : prior) / "standalone.ini", Encoding::Utf8, false}};
}

// Records the disc a double-click used in standalone.ini (UTF-8), through a temporary file.
inline void remember(const fs::path& ini, const fs::path& disc) {
  const fs::path tmp = fs::path(ini).concat(".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "iso=" << disc.u8string() << "\n";
    if (!f) return;
  }
  std::error_code ec;
  fs::rename(tmp, ini, ec);
}

// The first remembered disc that passes `header` (cheap: a Melee NTSC 1.02 header) and, from a
// place with full_check, `unmodified` (reads the whole disc), or an empty path. `decode` turns an
// ini's bytes into a path in that ini's encoding.
template <class Decode, class Header, class Unmodified>
fs::path first_verified(const std::vector<Place>& from, Decode decode, Header header, Unmodified unmodified) {
  for (const Place& place : from) {
    const std::string raw = read_iso(place.ini);
    if (raw.empty()) continue;
    const fs::path disc = decode(raw, place.encoding);
    if (disc.empty() || !header(disc)) continue;
    if (place.full_check && !unmodified(disc)) continue;
    return disc;
  }
  return {};
}
}  // namespace disc_memory

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

inline std::vector<Place> places(const fs::path& local_app_data) {
  if (local_app_data.empty()) return {};
  const auto current = local_app_data / "MeleeUnlocked";
  return {{current / "standalone.ini", Encoding::Utf8, false},
          {current / "launcher.ini", Encoding::Ansi, true},
          {local_app_data / "MeleeUnlocked-YouGame" / "standalone.ini", Encoding::Utf8, false}};
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

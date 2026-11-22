// A direct launch finds its existing profile and a verified, previously selected disc.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "../app/disc_memory.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void write(const fs::path& file, const std::string& text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}

int main(int argc, char** argv) {
  CHECK(disc_memory::iso_line("") == "");
  CHECK(disc_memory::iso_line("iso=C:\\Games\\melee.iso\r\n") == "C:\\Games\\melee.iso");
  CHECK(disc_memory::iso_line("language=en\niso=a.iso\nnotesseen=1\niso=b.iso") == "b.iso");
  CHECK(disc_memory::iso_line("lobbyiso=x.iso\n") == "");

  const fs::path root = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("disc_memory_test_root"));
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path local = root / "Local";
  const auto decode = [](const std::string& raw, disc_memory::Encoding) { return fs::u8path(raw); };
  // "Verifies" when the file exists and starts with GALE01, as is_melee_102 checks the header.
  const auto verify = [](const fs::path& disc) {
    std::ifstream f(disc, std::ios::binary);
    char head[6] = {};
    return f.read(head, 6) && std::string(head, 6) == "GALE01";
  };
  // "Unmodified" stands in for mod_scan's whole-disc check: here, a file whose bytes end in VANILLA.
  int full_checks = 0;
  const auto unmodified = [&full_checks](const fs::path& disc) {
    ++full_checks;
    std::ifstream f(disc, std::ios::binary);
    const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return all.size() >= 7 && all.compare(all.size() - 7, 7, "VANILLA") == 0;
  };
  const fs::path good = root / "discs" / "melee.iso", other = root / "discs" / "other.iso", bad = root / "discs" / "notes.txt";
  const fs::path modded = root / "discs" / "20XX.iso";   // the 1.02 header, other contents
  write(good, "GALE01..VANILLA");
  write(other, "GALE01..VANILLA");
  write(bad, "hello");
  write(modded, "GALE01..20XXTE");

  // Nothing remembered anywhere: nothing found (the caller goes on to melee.iso, then the picker).
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified).empty());
  CHECK(disc_memory::places({}).empty());

  const auto chosen = local / "MeleeUnlocked" / "standalone.ini";
  const auto launcher = local / "MeleeUnlocked" / "launcher.ini";
  const auto prior = local / "MeleeUnlocked-YouGame" / "standalone.ini";
  // A launcher.ini alone must not hide an earlier standalone player's saves. A fork profile wins
  // when the player has already used both. Empty card folders on first launch are not saved data.
  CHECK(disc_memory::standalone_dir({}).empty());
  CHECK(disc_memory::standalone_dir(local) == chosen.parent_path());
  write(launcher, "language=en\n");
  fs::create_directories(chosen.parent_path() / "User/GC/CardA/VanillaOnly");
  write(prior.parent_path() / "User/GC/CardA/VanillaOnly/GALE01.gci", "old save");
  CHECK(disc_memory::standalone_dir(local) == prior.parent_path());
  CHECK(disc_memory::places(local).front().ini == prior);
  write(chosen.parent_path() / "port-settings.ini", "volume 58\n");
  CHECK(disc_memory::standalone_dir(local) == chosen.parent_path());
  write(prior, "iso=" + good.u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);
  write(launcher, "iso=" + modded.u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);
  write(launcher, "iso=" + other.u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == other);
  write(chosen, "iso=" + good.u8string() + "\n");
  full_checks = 0;
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);
  CHECK(full_checks == 0);

  // remember() writes the line read back.
  disc_memory::remember(chosen, good);
  CHECK(disc_memory::read_iso(chosen) == good.u8string());

  fs::remove_all(root, ec);
  if (failures) return 1;
  std::printf("disc_memory: ok\n");
  return 0;
}

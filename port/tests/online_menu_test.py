"""Execute the production menu input/entry functions with offline scene stubs."""
from pathlib import Path
import importlib.util
import subprocess
import shutil
import os
import shlex
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

def compiler_command():
    # Windows compilers lack setenv/unsetenv, which several fixtures use.
    compat = ['-include', str(Path(__file__).resolve().parent / 'posix_fixture_compat.h')] if os.name == 'nt' else []
    if os.environ.get('CXX'):
        return shlex.split(os.environ['CXX']) + compat
    for name in ('c++', 'g++', 'clang++'):
        if path := shutil.which(name):
            return [path] + compat
    raise RuntimeError('an existing C++ compiler is required (CXX, c++, g++, clang++)')
spec = importlib.util.spec_from_file_location('fixture', ROOT / 'tools/native_major_fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def menu_functions(*names):
    """Extract function bodies lazily so unrelated fixtures can reuse the compiler helper."""
    source = (ROOT / "sourceport/game/shim/mu_slippi_menu.c").read_text()
    return "\n".join(fixture.extract(source, name) for name in names)


STUBS = r"""
#include <cassert>
#include <initializer_list>
#include <cstdint>
#include <cstdio>
using u8 = unsigned char;
using s8 = signed char;
using u32 = unsigned;
enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_MODE_DIRECT,
       MU_SLP_MODE_TEAMS, MU_SLP_MODE_PARTY };
enum { MU_SLP_OPT_LOGIN=5, MU_SLP_OPT_LOGOUT, MU_SLP_OPT_UPDATE };
enum { MU_SLP_APP_LOGGED_OUT, MU_SLP_APP_LOGGED_IN, MU_SLP_APP_UPDATE };
enum { MENU_KIND_8 = 8, MENU_KIND_1P = 1, SEL_1P_2 = 2, GM_HANYU_CSS = 8,
       MU_SLP_WINNER_NULL = -1, MenuInput_Confirm = 1, MenuInput_Back = 2,
       MenuInput_Up = 4, MenuInput_Down = 8, CMD_OPEN_LOGIN = 0xb6, CMD_UPDATE_APP = 0xb8 };
struct HSD_GObj {};
struct MenuExitData { int pending_mode; } exit_data;
struct { int app_state, mode, is_winner, chose_stage; } slp;
struct { int cooldown; } mn_804D6BC8;
struct { int cur_menu, hovered_selection, entering_menu, buttons; } mn_804A04F0;
static unsigned network_policy, pressed, command;
static int p2p_auto_direct, entered, content, back;
int mu_slippi_option_unlocked(int);
int mn_80229624(int) { return pressed; }
int mn_80229938(int, int row) { return mu_slippi_option_unlocked(row); }
void mn_80229894(int, int, int) { ++back; }
int mu_online_p2p_menus() { return 0; }
int mu_online_p2p_port() { return 0; }
int confirm_port() { return 0; }
void gm_801677E8(s8) {}
void gm_801A4B60() { ++entered; }
void mu_content_mode(unsigned mode) { content = mode; }
MenuExitData* gm_GetCurrentSceneExitData() { return &exit_data; }
void mu_slippi_cmd(int cmd, void*, int, void*, int, void*) { command = cmd; }
void sfxForward() {} void sfxBack() {} void sfxMove() {}
#define OSReport(...) ((void)0)
"""


class OnlineMenuTest(unittest.TestCase):
    def compile_fixture(self, name, body, *defines):
        with tempfile.TemporaryDirectory(prefix=name + "-") as directory:
            cpp, exe = Path(directory) / "fixture.cpp", Path(directory) / "fixture"
            cpp.write_text(body)
            subprocess.run([*compiler_command(), "-std=c++17", *defines,
                            "-I", str(ROOT / "port/runtime/hle"), "-I", str(ROOT / "port/third_party"),
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_slippi_mode_selection_account_states_and_navigation(self):
        production = menu_functions(
            "mu_slippi_menu_count", "mu_slippi_option_unlocked",
            "mu_slippi_ranked_available", "mu_slippi_online_menu_think")
        self.compile_fixture("online-menu", STUBS + production + r"""
int main() {
  assert(mu_slippi_menu_count() == 8);
  assert(!mu_slippi_option_unlocked(-1) && !mu_slippi_option_unlocked(8));
  pressed = MenuInput_Confirm;
  for (int state : {MU_SLP_APP_LOGGED_OUT, MU_SLP_APP_UPDATE}) {
    slp.app_state = state;
    int allowed = state == MU_SLP_APP_UPDATE ? MU_SLP_OPT_UPDATE : MU_SLP_OPT_LOGIN;
    assert(!mu_slippi_ranked_available());
    for (int row = 0; row < 8; ++row) {
      assert(bool(mu_slippi_option_unlocked(row)) == (row == allowed));
      command = 0; mn_804A04F0.hovered_selection = row;
      mu_slippi_online_menu_think(nullptr);
      assert(entered == 0);
      assert(command == (row != allowed ? 0 : state == MU_SLP_APP_UPDATE ? CMD_UPDATE_APP : CMD_OPEN_LOGIN));
    }
  }
  slp.app_state = MU_SLP_APP_LOGGED_IN;
  assert(mu_slippi_ranked_available());
  for (int row = 0; row <= MU_SLP_MODE_PARTY; ++row) {
    assert(mu_slippi_option_unlocked(row));
    mn_804A04F0.hovered_selection = row;
    mu_slippi_online_menu_think(nullptr);
    assert(slp.mode == row && content == row && entered == row + 1);
    assert(exit_data.pending_mode == GM_HANYU_CSS);
  }
  pressed = MenuInput_Down; mu_slippi_online_menu_think(nullptr);
  assert(mn_804A04F0.hovered_selection == MU_SLP_MODE_RANKED);
  pressed = MenuInput_Up; mu_slippi_online_menu_think(nullptr);
  assert(mn_804A04F0.hovered_selection == MU_SLP_MODE_PARTY);
  pressed = MenuInput_Back; mu_slippi_online_menu_think(nullptr);
  assert(back == 1);
}
""")


if __name__ == "__main__":
    unittest.main()

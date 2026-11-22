"""Shipping roster and diagnostic policy boundaries; no game or real assets."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


class ComplianceDiagnosticsTest(unittest.TestCase):
    def test_te_fake_attack_uses_session_policy(self):
        lab = source("sourceport/game/shim/mu_lab.c")
        self.assertIn('#include "mu_test_env.h"', lab)
        self.assertIn('te_fake = mu_test_getenv("MELEE_TE_FAKE_ATTACK") != NULL;', lab)
        self.assertNotRegex(lab, r'(?<!mu_test_)getenv\("MELEE_TE_FAKE_ATTACK"\)')

    def test_shipping_sources_have_no_player_identities(self):
        # Construct identifiers so this regression check itself contains no player name.
        names = ["man" + "g0", "za" + "in", "hungry" + "box", "co" + "dy", "am" + "sa", "h" + "box", "ib" + "dw"]
        pattern = re.compile(r"\b(?:" + "|".join(names) + r")\b", re.I)
        # One build (0.9.990, owner's call): Fight a pro shows the pro's name on screen, as every main
        # release has. Only main's Fight a pro files may name a pro, and only on as many lines as main.
        fight_a_pro = {
            "port/runtime/hle/practice_ghosts.h": 6,       # the roster, its costumes note
            "port/runtime/hle/ghost_ai.cpp": 1,            # the specialist file names
            "port/runtime/hle/phillip_ai.cpp": 1,          # the order note
            "port/runtime/gx/practice_menu.h": 1,          # the sparring note
            "port/runtime/hle/native_practice.h": 1,       # the sparring note
            "port/tests/ghost_ai_test.cpp": 3,             # policy choice notes
            "sourceport/game/shim/mu_practice_cpu.c": 5,   # the pro per character, the name tag table
        }
        for directory in ("port", "sourceport"):
            for path in (ROOT / directory).rglob("*"):
                if path.suffix in (".c", ".cpp", ".h", ".inl", ".py", ".patch") and path.is_file():
                    # Test data only: never open environment or authentication files.
                    text = path.read_text(encoding="utf-8-sig")
                    rel = path.relative_to(ROOT).as_posix()
                    if rel in fight_a_pro:
                        lines = [l for l in text.splitlines() if pattern.search(l)]
                        self.assertEqual(len(lines), fight_a_pro[rel], rel)
                        continue
                    self.assertIsNone(pattern.search(text), rel)
        tag = source("sourceport/game/shim/mu_practice_cpu.c")
        tag_names = '", "'.join(n.upper() for n in (names[0], names[1], "", names[3], names[4]))
        self.assertIn('static const char* const names[] = {"' + tag_names + '"};', tag)

    def test_specialist_paths_match_main(self):
        ai = source("port/runtime/hle/ghost_ai.cpp")
        files = ("man" + "g0", "za" + "in", "slot2", "co" + "dy", "am" + "sa")
        self.assertIn('{"' + '", "'.join(files) + '"}', ai)

if __name__ == "__main__":
    unittest.main()

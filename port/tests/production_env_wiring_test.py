"""All diagnostic getenv reads must reach the session policy, including C shims."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]

class WiringTest(unittest.TestCase):
    def test_parent_and_host_callback_wiring(self):
        main = (ROOT / 'port/app/main.cpp').read_text()
        self.assertIn('mods::identify_vanilla(std::filesystem::u8path(o.iso), check, &verification)', main)
        host = (ROOT / 'port/app/source_host.cpp').read_text()
        self.assertIn('return host::test_env::getenv(name);', host)
        layer = host.split('std::string iso_layer_fingerprint(', 1)[1].split('std::string dir_layer_fingerprint', 1)[0]
        self.assertIn('layer_fingerprint(g_mod_overlay', layer)
        self.assertNotIn('fingerprints.txt', layer)

if __name__ == '__main__':
    unittest.main()

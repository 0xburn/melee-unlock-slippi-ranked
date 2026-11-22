"""Verify the single entry point for downloading CPU models."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
PHILLIP = ROOT / 'port/runtime/hle/phillip_ai.cpp'


class WiringTest(unittest.TestCase):
    def setUp(self):
        self.source = PHILLIP.read_text(encoding='utf-8-sig')

    def test_only_fetch_later_reaches_the_downloader(self):
        self.assertEqual(self.source.count('host::updater::fetch_verified('), 1)
        self.assertEqual(len(re.findall(r'std::thread\(fetch_worker\)', self.source)), 1)
        self.assertEqual(self.source.count('g_fetch_queue.push_back'), 1)
        worker = self.source.split('void fetch_worker() {', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('host::updater::fetch_verified(', worker)


if __name__ == '__main__':
    unittest.main()

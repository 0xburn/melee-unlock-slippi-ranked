"""Window-free source guards for the Windows-only adapter runtime.

The behavioral counter/lifecycle fixture is gc_adapter_reset_test.cpp (Windows).
These checks keep every hardware boundary guarded on macOS/Linux without opening
an adapter, starting the game, or substituting a different adapter implementation.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
ADAPTER = (ROOT / "port/runtime/host/gc_adapter.cpp").read_text()
SETTINGS = (ROOT / "port/runtime/gx/pc_settings.cpp").read_text()


def body(name):
    # Select multiline definitions, skipping the non-Windows open_hid no-op.
    match = re.search(r"^.*\b" + name + r"\([^;\n]*\) \{\n", ADAPTER, re.M)
    if not match:
        raise AssertionError(f"missing definition: {name}")
    start = match.end()
    depth = 1
    for pos in range(start, len(ADAPTER)):
        depth += (ADAPTER[pos] == "{") - (ADAPTER[pos] == "}")
        if not depth:
            # The guard must be the first statement; leading comment lines do not count (v0.9.80 added one).
            code = [line for line in ADAPTER[start:pos].strip().splitlines() if not line.strip().startswith("//")]
            return "\n".join(code).strip()
    raise AssertionError(f"unterminated definition: {name}")


class AdapterGuardTest(unittest.TestCase):
    def test_single_owner_policy_and_startup(self):
        self.assertIn("bool direct_adapter_allowed() { return !options.no_gc_adapter; }", ADAPTER)
        startup = body("ensure_scanner_started")
        self.assertTrue(startup.startswith("if (!direct_adapter_allowed()) return;"))
        self.assertIn("std::call_once(g_scanner_started", startup)
        self.assertIn("g_scanner = std::thread(scanner_thread);", startup)
        self.assertIn("++g_test_scanner_starts", startup)
        self.assertEqual(ADAPTER.count("std::thread(scanner_thread)"), 1)

    def test_every_open_route_and_worker_is_guarded(self):
        for name in ("open_adapter", "open_libusb", "open_hid"):
            with self.subTest(name=name):
                self.assertTrue(body(name).startswith("if (!direct_adapter_allowed()) return false;"))
        self.assertIn("return open_libusb() || open_hid();", body("open_adapter"))
        for name in ("reader_thread", "start_threads"):
            self.assertTrue(body(name).startswith("if (!direct_adapter_allowed()) return;"))
        self.assertTrue(body("scanner_thread").startswith("while (g_scanner_run.load() && direct_adapter_allowed())"))
        self.assertTrue(body("writer_thread").startswith("while (g_running.load() && direct_adapter_allowed())"))
        self.assertTrue(body("adapter_read").startswith("if (!direct_adapter_allowed()) { *got = 0; return LIBUSB_ERROR_NO_DEVICE; }"))
        self.assertTrue(body("adapter_write").startswith("if (!direct_adapter_allowed()) { *wrote = 0; return LIBUSB_ERROR_NO_DEVICE; }"))
        self.assertTrue(body("close_adapter").startswith("reset_transport = reset_transport && direct_adapter_allowed();"))

    def test_settings_queries_and_reset(self):
        self.assertIn("ensure_scanner_started();", body("gcadapter_problem"))
        self.assertIn("return g_problem;", body("gcadapter_problem"))
        self.assertTrue(body("gcadapter_request_reset").startswith("if (!direct_adapter_allowed()) return false;"))


if __name__ == "__main__":
    unittest.main()

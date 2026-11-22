"""Keep pacing observers and unchanged timing routines pinned across renderer upgrades."""
from pathlib import Path
import re
import difflib
import json
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASE = "2f4aa72"
GIT = "/Library/Developer/CommandLineTools/usr/bin/git" if sys.platform == "darwin" else "git"


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def baseline(path, revision=BASE):
    # The baselines are commits from the original project's history; a tree without them skips.
    try:
        out = subprocess.check_output([GIT, "show", f"{revision}:{path}"], cwd=ROOT, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        raise unittest.SkipTest(f"baseline commit {revision} is not in this repository")
    return out.decode("utf-8-sig").replace("\r\n", "\n")


class PacingDiagnosticsWiringTest(unittest.TestCase):
    def test_pacing_routines_equal_baseline_after_removing_only_observers(self):
        for api in (11, 12):
            with self.subTest(api=api):
                path = f"port/runtime/gx/gx_d3d{api}.cpp"
                current = source(path)
                self.assertEqual(current.count("pacing_diagnostics_.present("), 1)
                self.assertEqual(current.count("pacing_diagnostics_.poll("), 1)
                self.assertEqual(current.count("pacing_diagnostics_.statistics("), 1)
                clean = current.replace('#include "pacing_diagnostics_win.h"\n', '')
                clean = clean.replace('  pacing_diag::WindowsRecorder pacing_diagnostics_;\n', '')
                clean = clean.replace('  HRESULT diagnostic_statistics_result = E_FAIL;\n', '')
                clean = clean.replace('SUCCEEDED(diagnostic_statistics_result = swapchain_->GetFrameStatistics(&st))',
                                      'SUCCEEDED(swapchain_->GetFrameStatistics(&st))')
                clean = clean.replace('  pacing_diagnostics_.statistics(diagnostic_statistics_result, st);\n', '')
                clean = re.sub(r' +pacing_diagnostics_\.present\(present_call, opts_\.vsync,\n +present\.interval, present\.flags, opts_\.fps_cap, fullscreen_state_\.exclusive_active\);\n', '', clean)
                clean = clean.replace('  pacing_diagnostics_.poll(swapchain_.Get(), hwnd_);\n', '')
                # Build 88's slow-frame log times the GPU wait (an observer: the wait itself is unchanged).
                clean = clean.replace('    Stopwatch gpu_wait_sw;\n', '')
                clean = clean.replace('    ++slow_.gpu_waits; slow_.gpu_wait_ms += gpu_wait_sw.lap() * 1e3;\n', '')
                # v0.9.96 rewords the variable-refresh log line only (the cap and V-Sync are unchanged here).
                new_line = '"display; frame cap below %.0f fps, V-Sync follows the player\'s setting"'
                self.assertEqual(clean.count(new_line), 1)
                clean = clean.replace(new_line, '"display; frames are presented synced under %.0f a second, so they never tear"')
                # v0.9.83 adds MSAA, shared presentation shaders, and occlusion handling.
                # Pin the unaffected pacing routines rather than freezing the entire renderer.
                from source_test_helpers import body
                names = ('gpu_frame_begin', 'gpu_frame_end', 'update_variable_refresh') if api == 11 else (
                    'wait_frame_latency', 'wait_gpu', 'update_variable_refresh')
                for name in names:
                    self.assertEqual(body(clean, name), body(baseline(path), name), name)

    def test_timestamp_and_actual_present_arguments_are_recorded_after_wait_measurement(self):
        for api in (11, 12):
            s = source(f"port/runtime/gx/gx_d3d{api}.cpp")
            call = s.index('pacing_diagnostics_.present(present_call, opts_.vsync,')
            self.assertGreater(call, s.index('return swapchain_->Present(interval, present_flags);'))
            self.assertGreater(call, s.index('present_wait_ += Stopwatch::now() - present_call;'))
            self.assertIn('interval, present_flags, opts_.fps_cap, fullscreen_state_.exclusive_active);', s[call:call+210])
            self.assertGreater(s.index('pacing_diagnostics_.poll('), s.index('  gpu_frame_end();') if api == 11 else s.index('queue_->Signal(fence_.Get(), fence_value_);', s.index('return swapchain_->Present(')))

    def test_no_new_frame_statistics_queries_or_vrr_changes(self):
        for api in (11, 12):
            path = f"port/runtime/gx/gx_d3d{api}.cpp"
            s = source(path)
            self.assertEqual(s.count('GetFrameStatistics('), baseline(path).count('GetFrameStatistics('))
            self.assertIn('if ((frames_presented_ & 31) == 0) update_variable_refresh();', s)
            self.assertIn('if (!vrr_readable_) { vrr_.restart(); return; }', s)
        win = source('port/runtime/gx/pacing_diagnostics_win.h')
        self.assertNotIn('->GetFrameStatistics(', win)
        self.assertIn('GetFrameStatisticsMedia(&media)', win)

    def test_timer_requests_and_masks_are_unchanged(self):
        path = 'port/app/main.cpp'
        s, old = source(path), baseline(path)
        for pattern in (r'(?<![\w:])time(?:Begin|End)Period\([^)]*\)',
                        r'SetProcessInformation\([^;\n]*?sizeof throttling\)',
                        r'throttling\.(?:ControlMask|StateMask|Version) = [^;]+;'):
            # The #else branch is the byte-for-byte original non-Windows fallback.
            normalized = re.sub(r'"(?:[^"\\]|\\.)*"', '""', s).replace('#else\n      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling);\n#endif', '#endif')
            self.assertEqual(re.findall(pattern, normalized), re.findall(pattern, re.sub(r'"(?:[^"\\]|\\.)*"', '""', old)))
        self.assertIn('if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling)) {\n#ifdef _WIN32\n      timer_resolution_error = GetLastError();', s)

    def test_log_file_selected_before_new_startup_lines(self):
        s = source('port/app/main.cpp')
        self.assertGreater(s.index('host::log("timing: timeBeginPeriod'), s.index('host::options.log_file = argv[++i]'))
        self.assertIn('TokenIsAppContainer', s)
        self.assertIn('CloseHandle(diagnostic_token);', s)

    def test_settings_event_is_windows_only_and_preserves_toggle(self):
        text = source('port/runtime/gx/pc_settings.cpp')
        start = text.index('const bool diagnostic_vsync_before')
        toggle = text.index('settings_toggle("VSync (prevents tearing)", &options.vsync)', start)
        check = text.index('if (options.vsync != diagnostic_vsync_before)', toggle)
        log = text.index('host::log("present: vsync %s (settings)"', check)
        self.assertIn('#ifdef _WIN32', text[toggle:check])
        self.assertLess(check, log)

    def test_observer_has_no_wait_flush_present_or_configuration_writes(self):
        win = source('port/runtime/gx/pacing_diagnostics_win.h')
        for forbidden in ('->Present(', 'SetMaximumFrameLatency(', 'SetFullscreenState(', 'SetWaitableTimer(',
                          'WaitForSingleObject(', 'Sleep(', 'Flush(', 'ResizeBuffers(', 'opts_.'):
            self.assertNotIn(forbidden, win)
        self.assertIn('present_seconds - minute_ < 60.0', win)
        self.assertIn('present_seconds - second_ >= 1.0', win)
        pure = source('port/runtime/gx/pacing_diagnostics.h')
        self.assertNotIn('std::vector', pure)
        self.assertNotIn('new ', pure)
        self.assertIn('DXGI_ERROR_FRAME_STATISTICS_DISJOINT', win)

    def test_test_registration_precedes_linux_return_and_build_helper_includes_it(self):
        cmake = source('CMakeLists.txt')
        self.assertLess(cmake.index('add_executable(port_pacing_diagnostics_test'), cmake.index('  return()'))
        self.assertIn("targets = ['all']", source('tools/build_linux.py'))


if __name__ == '__main__':
    unittest.main()

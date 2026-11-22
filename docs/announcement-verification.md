# Frame rate and rollback verification

Measured on an Apple M1 Pro running macOS 26.4.1, using x86-64 hosts under Rosetta and an NTSC 1.02 disc. The game library was built from the fork. The upstream version under comparison was 1.0.5.

| Claim | Observation |
| --- | --- |
| Game logic runs at 60 Hz | A paced, headless game ran 1,900 retraces in 31.668 seconds, averaging 16.667 ms per retrace. The scripted match produced the same RNG, fighter states and other digest fields in an unpaced run and a hidden graphics run requested at 144 FPS. All 76 fields agreed for all 572 overlapping match frames. |
| Intermediate frames are drawn | Eight synthetic 60 Hz source ticks produced 8, 20, 33 and 63 presentations at 60, 144, 240 and 480 FPS targets respectively. At 60 FPS there were zero re-posed draws. At the three higher settings each presentation had a re-posed draw. The diagnostic uses a hidden OpenGL window and a moving triangle with an authored animation track. Its counters measure draws submitted by the renderer. |
| Monitor rate, 480 FPS and uncapped gameplay | The full host accepts fixed, monitor and unlocked settings. Both Mac gameplay entry points disable the threaded presenter. The diagnostic exercises it independently. Physical display cadence and sustained 480 FPS gameplay were unmeasured. |
| Subframe input is on by default | A fresh configuration has early inputs off. The setting persists under `early_inputs2`. Starting rollback immediately on an early contradiction is a separate opt-in through `MELEE_EARLY_ROLLBACK=1`. |
| Inputs are read every millisecond and sent between frames | With early inputs enabled, the production sampling thread read a synthetic controller 1,705 times. Median interval was 1.277 ms and p95 was 1.431 ms. All 120 button changes reached the transport callback before the next simulated game poll, with 10.570 ms median lead. The sampler sleeps for 1 ms between reads, so scheduler overhead adds to the interval. This measures the host path through the callback. |
| Wrong predictions are corrected a frame sooner | In a controlled native prediction test, a late A press contradicted frame 100. With defaults, the game triggered rollback on tick 101 when the confirmed pad arrived. With `MELEE_EARLY_ROLLBACK=1`, it triggered on tick 100. Both rewound to frame 100. Real matches depend on arrival timing and peer support. |

In a separate loopback game with early inputs enabled and 8 ms of scripted lead, one client sent 154 early-pad packets. Its peer received all 154, used them for four predictions and confirmed all four. The other direction delivered 66 of 67 packets. Both recorded 969 frames, the same winner and zero desyncs. Neither client needed to start a rollback from an early packet during this game.

## Local Ranked sets

Two full macOS clients entered Ranked from the game's online menu using separate synthetic accounts and loopback ports. Both completed game 1, returned to setup for counterpicking, completed game 2 and recorded the same winner. The first run recorded 1,224 and 1,213 game frames. A second run recorded 1,344 and 1,229. One peer rolled back three times in its first game of the second run, with maximum depth one. All four games completed with zero reported desyncs. Both clients suppressed result submission for these local fixtures.

To reproduce with the packaged Mac build, run the harness with your own disc. It retains the logs and both fixture profiles in the output directory.

```sh
app=build-mac-full/MeleeUnlocked.app/Contents
python3 tools/check_local_ranked_set.py --fast \
  --exe "$app/MacOS/MeleeUnlocked" --sys-dir "$app/Resources/Sys" \
  --iso "/path/to/Melee.iso" --output /tmp/ranked-set-run
```

The optional `--fast` mode advances the game's network timeout counters faster than wall time. One early unpaced trial hit a peer stall and abandoned the set. Omit the option to run at the normal game clock.

## Reproduce

Build the sampler from the repository root on an x86-64 host. On Apple Silicon, add `-arch x86_64` to the compiler invocation.

```sh
c++ -std=c++17 -O2 -pthread -Iport/runtime/host -Iport/runtime/hle -Iport/runtime/ppc \
  tools/probe_early_sampler.cpp port/runtime/host/early_input.cpp -o /tmp/probe_early_sampler
/tmp/probe_early_sampler
```

The probe alternates two buttons six milliseconds after each 16.667 ms poll. It records the sampler's actual read times and when its callback receives each target frame and pad.

The native correction fixture in `tools/probe_early_rollback.c` uses the game's own prediction buffers and functions. Compile it with the same definitions and include paths as the native game's `shim/mu_online.c`, adding `-ffunction-sections -fdata-sections -Wl,--gc-sections`. Run it once with default environment and once with `MELEE_EARLY_ROLLBACK=1`.

Run the Mac offline host with `MELEE_MAC_DEV_SPIKE=1` and `--presenter-self-test --fps 144` for the GPU diagnostic. Repeat at 60, 240 and 480. For simulation parity, run a separate instance for each mode against the same local disc and game library. Use fresh card directories, `--vanilla-game --no-gc-adapter --time-base 0 --rng-seed 0x12345678 --script port/scripts/parity_vs.txt --frames 1900`, and supply a different `--state-digest` path for each run. The three modes were paced `--headless`, unpaced `--headless --fast`, and `--hidden --fps 144`. Compare the CSVs with `python3 port/mac/compare_digest.py <reference.csv> <observed.csv>`.

The relevant paths are [the presenter fixture](../port/linux/renderer_presenter.cpp), [the full Mac gameplay renderer selection](../port/app/main.cpp), [the sampler](../port/runtime/host/early_input.cpp), [the settings loader](../port/runtime/gx/pc_settings.cpp), and [the native prediction logic](../sourceport/game/shim/mu_online_prediction.inc).

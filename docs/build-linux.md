# Build and run on Linux

The native x86-64 Source Port runs on Linux and can use your existing
Slippi Launcher sign-in. You'll need an AVX2/FMA-capable CPU for this build
and your own clean Melee NTSC 1.02 ISO. Expand an NKit image to a full ISO
with NKit first.

## Build

Install Git, Python 3, CMake 3.25 or later, Ninja, pkg-config, and GCC 14
with the `gcc-14` and `g++-14` commands on your PATH. Install the
development packages for SDL2, OpenGL, GLEW, libusb, libcurl, OpenSSL, and
FFmpeg (`libavformat`, `libavcodec`, `libswscale`, `libswresample`, and
`libavutil`). Install the `glslangValidator` and `spirv-cross` command-line
tools. The example launch uses a Vulkan driver and an X11 session or
XWayland.

Run these commands from the checkout root. Set `iso` to an absolute path.

```sh
iso="/path/to/melee-ntsc-1.02.iso"
git submodule update --init sourceport/extern/melee
python3 tools/prepare_native_sources.py
mkdir -p build-linux-inputs
python3 tools/extract_dol.py "$iso" build-linux-inputs/main.dol
python3 port/recomp/recomp.py --dol build-linux-inputs/main.dol \
  --gct-base 0x8065CC80 --out build-linux-inputs/generated
python3 tools/build_linux.py --port-gen build-linux-inputs/generated --jobs 4
```

Passing `--port-gen` builds the full `melee_source` target and the launcher.

## Run

Put the launcher, game library, and runtime data in one directory. The
deltas let the game generate any missing Slippi menu assets from your ISO.

```sh
host_dir=build-linux/port/linux/full
run_dir="$PWD/build-linux/run"
mkdir -p "$run_dir/bin" "$run_dir/deltas" "$run_dir/Sys" "$run_dir/SysPlayback/GC"
install -m 755 "$host_dir/melee_source" "$host_dir/launcher/MeleeUnlocked" "$run_dir/"
install -m 644 build-linux-game/melee_game.so build-linux-game/melee_game.snapexcl "$run_dir/"
cp -R "$host_dir/Sys/." "$run_dir/Sys/"
cp -R port/slippi_sys_playback/. "$run_dir/SysPlayback/"
install -m 644 "$host_dir/Sys/GC/dsp_coef.bin" "$run_dir/SysPlayback/GC/"
install -m 644 port/sys_deltas/manifest.json port/sys_deltas/*.vcdiff "$run_dir/deltas/"
install -m 644 port/third_party/dxc/linux/libdxcompiler.so "$run_dir/bin/"
cp -R lang/. "$run_dir/lang/"

cd "$run_dir"
SDL_VIDEODRIVER=x11 ./MeleeUnlocked --play --iso "$iso" --backend vulkan
```

You can omit `--backend vulkan` to use the default OpenGL 4.5 renderer.

Sign in through the official Slippi Launcher with an account that has
Ranked access, then choose **Online > Ranked** in the game.

## Validation

Version 1.0.6 was built and run on CachyOS x86-64. The game rendered on an
RX 5700 XT with Vulkan, opened the ShanWan adapter through a local SDL
mapping, loaded the existing Slippi sign-in, and reached Training character
select. Those checks ran with networking disabled. Live Ranked
matchmaking remains untested.

# Experimental macOS build

The full host targets x86-64 on macOS 15 or later. Apple Silicon uses Rosetta. Supply a Melee NTSC 1.02 ISO, an x86-64 SDL2 development distribution under `../deps/sdl2`, and a matching x86-64 Linux game library built from the fork. Set `MELEE_GAME_LIBRARY` to the path of `melee_game.so`. Its `melee_game.snapexcl` and `Sys/GC/dsp_coef.bin` must be beside the library in the layout produced by the guest build.

Build the native game library on x86-64 Linux with GCC and Ninja. The host checks the library's Squad Strike flag at startup. The macOS host uses the off setting. Its ELF loader requires the Linux stack canary calls to be disabled.

```sh
git submodule update --init sourceport/extern/melee
python3 tools/prepare_native_sources.py
cmake -S sourceport/game -B build-mac-guest -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMELEE_SQUAD_STRIKE=OFF \
  -DCMAKE_C_FLAGS=-fno-stack-protector -DCMAKE_CXX_FLAGS=-fno-stack-protector
cmake --build build-mac-guest --target melee_game
```

Copy `melee_game.so` and `melee_game.snapexcl` from that build to the Mac. Place the matching runtime `Sys` directory beside them and set `MELEE_GAME_LIBRARY` to the copied ELF file.

Generate the host headers from your own disc, then compile the host.

```sh
mkdir -p build-mac-full/gen
python3 tools/extract_dol.py "/path/to/Melee.iso" build-mac-full/gen/main.dol
python3 port/recomp/recomp.py --dol build-mac-full/gen/main.dol --gct-base 0x8065CC80 --out build-mac-full/gen/port-generated
python3 port/mac/build_full.py --host-only
```

To package the full app, install `glslangValidator` and `spirv-cross` under `../deps/tools/bin` and run `python3 port/mac/package_full.py`. The bundle includes the host, guest library and runtime assets. The player supplies the ISO separately.

When you open the app in Finder, press **F1** (or **Fn + F1** on a Mac keyboard) to open the in-game settings. In a Training match, press **Tab** to open the Practice menu.

The offline graphics diagnostic has a separate build script, `port/mac/build-app.sh`. It exercises the renderer without loading the full online host.

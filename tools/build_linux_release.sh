#!/bin/bash
# Build and package the standalone x86-64 Linux release on Ubuntu 24.04.
set -euo pipefail
cd "$(dirname "$0")/.."
git submodule update --init --depth 1 sourceport/extern/melee
python3 tools/prepare_native_sources.py
export MELEE_PORT_GEN="$PWD/windows-build/headers"
python3 tools/build_linux.py
python3 tools/build_linux.py --cpu SSE2 --build-dir build-linux-compat --game-build-dir build-linux-game-compat
python3 tools/gecko_functions.py --check --linux --objdump objdump build-linux-game/melee_game.dbg build-linux-game-compat/melee_game.dbg

# Supply the original packager with the released AI models and optional Lab content.
upstream=MeleeUnlocked-1.0.22
cache=build/linux-release-inputs
mkdir -p "$cache" release/content-1 build/lab port/third_party/onnxruntime/bin
archive="$cache/$upstream-linux-x86_64.tar.gz"
lab=release/content-1/MeleeUnlocked-Lab.zip
if [[ ! -f "$archive" ]]; then
  curl -fLsS --retry 3 "https://github.com/Hero88go/melee-unlocked/releases/download/v1.0.22/$upstream-linux-x86_64.tar.gz" -o "$archive"
fi
echo "99ffcc0a80d96ae402b8f2ab38831dd6b36c561599f9c3e460a26f662fb86b94  $archive" | sha256sum -c -
tar -xf "$archive" -C "$cache" "$upstream/ProAI" "$upstream/lib/libonnxruntime.so"
cp "$cache/$upstream/lib/libonnxruntime.so" port/third_party/onnxruntime/bin/
if [[ ! -f "$lab" ]]; then
  curl -fLsS --retry 3 "https://github.com/Hero88go/melee-unlocked/releases/download/content-1/MeleeUnlocked-Lab.zip" -o "$lab"
fi
echo "6fcb60586ee0b73a42b3b7521546b554d3b105299bc97d74720b849075febb75  $lab" | sha256sum -c -
unzip -oq "$lab" -d build/lab
python3 tools/package_linux_release.py --pro-ai-dir "$cache/$upstream/ProAI"

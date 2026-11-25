# Melee Unlocked: Slippi Ranked

This fork of [Melee Unlocked](https://github.com/Hero88go/melee-unlocked) removes the YouGame integration and adds Slippi Ranked back.

## Play

[Download Melee Unlocked for Windows, Mac, or Linux.](https://github.com/0xburn/melee-unlocked-slippi-ranked/releases/latest) You need your own Melee NTSC 1.02 ISO.

- **Windows x64:** Extract the zip, open `MeleeUnlockedLauncher.exe`, click **Browse** to select your ISO, then click **Play**. You can also drag the ISO onto `MeleeUnlocked.bat`.
- **Mac:** Extract the zip, open Melee Unlocked, and select your ISO. Requires macOS 15 or later and Rosetta on Apple Silicon.
- **Linux x86-64:** Extract the tar.gz, open a terminal in the extracted folder, and run `./MeleeUnlocked`. Select your ISO and choose **Play**.

For Ranked, sign in to Slippi Launcher with an account that has Ranked access. Choose **Online > Ranked**, or press **Tab** during Training and choose **Search > Ranked**.

## Build

- **Windows:** From a pushed checkout, run `python windows-build/release.py --iso "path/to/melee.iso"`. The command uses LLVM, Microsoft C++ headers, Python 3.11+ and GitHub CLI access to your fork. It builds the translated game code locally, runs the original Windows packager in Actions, and downloads both CPU builds to `release/`.
- **Mac:** Follow the [Mac build instructions](port/mac/README.md) to build the game library and package the app.
- **Linux x86-64:** On Ubuntu 24.04, install the dependencies listed in the [Linux workflow](.github/workflows/linux-release.yml) and run `bash tools/build_linux_release.sh`. The archive is written to `release/`.

For controls, features, and other documentation, see the [original Melee Unlocked repository](https://github.com/Hero88go/melee-unlocked).

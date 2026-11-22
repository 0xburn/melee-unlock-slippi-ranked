# Melee Unlocked: Slippi Ranked

This fork of [Melee Unlocked](https://github.com/Hero88go/melee-unlocked) removes the YouGame integration and adds Slippi Ranked back.

## Play

[Download the Windows or Mac zip.](https://github.com/0xburn/melee-unlock-slippi-ranked/releases/latest) You need your own Melee NTSC 1.02 ISO.

- **Windows x64:** Extract the zip, open `MeleeUnlockedLauncher.exe`, click **Browse** to select your ISO, then click **Play**. You can also drag the ISO onto `MeleeUnlocked.bat`.
- **Mac:** Extract the zip, open Melee Unlocked, and select your ISO. Requires macOS 15 or later and Rosetta on Apple Silicon.

For Ranked, sign in to Slippi Launcher with an account that has Ranked access. Choose **Online > Ranked**, or press **Tab** during Training and choose **Search > Ranked**.

## Build

- **Windows:** In your GitHub fork, open **Actions > Build Windows release > Run workflow**. Download the `Melee-Unlocked-Windows-x64` artifact from the completed run.
- **Mac:** Follow the [Mac build instructions](port/mac/README.md) to build the game library and package the app.

For controls, features, and other documentation, see the [original Melee Unlocked repository](https://github.com/Hero88go/melee-unlocked).

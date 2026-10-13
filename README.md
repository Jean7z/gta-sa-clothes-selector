# Clothes Selector for GTA:SA Android

A small tab at the top of the screen that opens a clothes picker in GTA: San
Andreas for Android.

![a tab hanging off the top edge of the screen, with a clothes hanger on it]

## What it does

You get a wardrobe. Tap the tab, pick a category, browse the items and try them
on before you buy.

It works without installing CLEO or any other script loader — the mod is a
single `.so` file that drops into your mods folder.

## Credit

The clothes selector itself — the menus, the item lists, and the two-column
preview — is a mod by **Alexander Blade**. This project is only the launcher
that puts a button on screen and starts it.

If you want that mod, it is his. This repository is here because his script can
be started from a normal AML mod without needing CLEO installed, and we wanted
to share how.

## Install

1. Build it, or grab a release if there is one.
2. Copy the `.so` into your AML mods folder:

   ```
   Android/data/com.rockstargames.gtasa/mods/
   ```

3. Launch the game. The tab appears at the top of the screen.

To remove it, delete the file.

## For developers

The script is a plain GTA:SA script — every opcode it uses exists in the normal
mobile opcode table, so nothing has to be emulated. The game already has a
function that runs scripts, `CTheScripts::StartNewScript`, and this mod just
resolves it by name and hands over the bytecode. No hardcoded offsets, so it
survives the game being rebuilt.

There is one catch worth knowing if you fork it. `StartNewScript` forgets to set
the script's base address, and the game's jump handler needs it — without one
line the game crashes on the first instruction. The fix is in `main.cpp` with a
comment explaining the disassembly behind it.

The interface is [Menu Kit](https://github.com/Jean7z/gta-sa-menu-kit),
compiled directly into this mod so there is nothing extra to install.

## Build

```sh
/path/to/android-ndk/ndk-build clean
/path/to/android-ndk/ndk-build
```

Output: `libs/arm64-v8a/libAML_PSDK_SelectorLauncher.so`

arm64-v8a only.

## Credits

- **Alexander Blade** — the clothes selector mod.
- [gta-sa-menu-kit](https://github.com/Jean7z/gta-sa-menu-kit) — the interface layer.
- [aml-psdk](https://github.com/AndroidModLoader/aml-psdk) — the SDK headers.
# Clothes Selector for GTA:SA Android

Browse every outfit in the game and try them on, without a script loader.

## What it does

You get a wardrobe.

A small tab with a clothes hanger on it sits at the **top of the screen**. Tap
it and a clothes picker opens — pick a category, browse the items, and see them
on your character.

Two things the tab does so it stays out of your way:

- **It disappears while you browse.** The picker has its own navigation, so the
  tab steps aside instead of sitting on top of it. It comes back when you close
  the picker.
- **It turns amber when you get in a car.** The picker cannot run while you are
  driving, so the colour tells you to step out. It is green when you are ready.

No CLEO or any other script loader to install — one `.so` file in your mods
folder.

## Install

Copy `libAML_PSDK_SelectorLauncher.so` into your AML mods folder:

```
Android/data/com.rockstargames.gtasa/mods/
```

Launch the game and the tab appears at the top of the screen.

To remove it, delete the file.

There are no prebuilt releases yet. If you do not already build Android mods,
[the build instructions are below](#build).

## For developers

The picker is a plain GTA:SA script — every opcode it uses exists in the normal
mobile opcode table, so nothing has to be emulated. The game already has a
function that runs scripts, `CTheScripts::StartNewScript`, and this mod resolves
it by name and hands over the bytecode. No hardcoded offsets, so it survives the
game being rebuilt.

There is one catch worth knowing if you fork it. `StartNewScript` forgets to set
the script's base address, and the game's jump handler needs it — without one
line the game crashes on the first instruction. The fix is in `main.cpp`, with a
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

- **Alexander Blade** — the clothes selector itself: the menus, the item lists,
  and the two-column preview. This project is only the launcher that puts a tab
  on screen and starts it. The script is embedded here unmodified, 4882 bytes.
  If you want that mod it is his to give; this repository exists to show it can
  be started from a normal AML mod without CLEO, and to share how.
- [gta-sa-menu-kit](https://github.com/Jean7z/gta-sa-menu-kit) — the interface layer (MIT).
- [aml-psdk](https://github.com/AndroidModLoader/aml-psdk) — the SDK headers (GPL-3.0).

## License

MIT for this launcher — see [LICENSE](LICENSE).

The embedded script bytecode in `mod/selector_script.h` is Alexander Blade's
work and is **not** covered by that license.
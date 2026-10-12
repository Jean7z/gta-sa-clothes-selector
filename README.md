# SA Selector Launcher

An Android Mod Loader (AML) mod for GTA:SA mobile that puts a single **SELECTOR**
button on screen. Pressing it starts the CLEO-Android script
`selector de ropa.csi` inside the game's own script VM.

## How it works

The script uses **only vanilla GTA:SA mobile opcodes** — all 29 of them are
present in the native opcode table, and there are no CLEO extensions to
emulate. So the game can parse and run it directly; no interpreter is needed.
All the mod has to do is hand the bytecode to a loader, and the game already
exports one:

```cpp
CTheScripts::StartNewScript(unsigned char const*)
```

It is resolved **by mangled name** through AML (`aml->GetSym`), never by a
hardcoded offset, so the mod survives libGTASA.so rebuilds:

```cpp
g_pStartNewScript = (StartNewScript_t)aml->GetSym(
    aml->GetLib("libGTASA.so"),
    "_ZN11CTheScripts14StartNewScriptEPh");
```

Note this is the single-argument overload. The two-argument
`...StartNewScriptEPht` is a separate symbol and is not used. The return type is
declared `void`: the ARM64 ABI makes ignoring the real `int` return safe, and
the mod has no reason to depend on the value.

## Menu Kit

Menu Kit is **compiled into this library** as source — there is no runtime
dependency on the standalone Menu Kit mod. `Android.mk` sets
`-DMENUKIT_EMBEDDED`, which hides `GetMenuAPI` from the dynamic symbol table so
this library is never mistaken for the real Menu Kit, and `main.cpp` uses the
embedded entry point:

```cpp
g_menuKit = MenuKit_InitEmbedded(aml);
```

The `mod/` directory is vendored from [gta-sa-menu-kit](https://github.com/Jean7z/gta-sa-menu-kit).

## Launching only once

`StartNewScript` **registers** the script with the game's script VM. Calling it
twice with the same buffer would register a duplicate script, so `OnRelease` is
guarded by a `static bool g_launched` and the script is started at most once per
process. The flag is only set when the loader actually resolved and was called.

`StartNewScript` copies the bytecode into a `CRunningScript`-owned buffer, so
pointing it at the `static const` array is safe.

## The vehicle hint

The script terminates itself immediately if the player is not driving a vehicle,
which would otherwise look like a dead button. The `SetTick` callback draws a
status line under the button:

| State | Text |
|---|---|
| Not in a vehicle | `GET IN A VEHICLE FIRST` |
| In a vehicle, not launched | `READY - PRESS SELECTOR` |
| Launched | `SELECTOR RUNNING` |

Detection uses the psdk's `FindPlayerVehicle(0, false)`. That accessor is
declared with `DECL_FASTCALL_SIMPLE_GLO`, which resolves the mangled symbol
through `aml->GetSym` — no baked-in address, so it is safe. If the symbol is
missing, the call returns `NULL` and the hint is shown, which is the
conservative fallback.

## Layout

```
main.cpp                  mod entry point, button, launch logic
mod/                      vendored Menu Kit + logger + amlmod headers
mod/selector_script.h     generated: script bytecode as a byte array
tools/gen_script_header.js generates selector_script.h from the .csi
Android.mk / Application.mk
jni -> .                  symlink; ndk-build uses it to find the project root
psdk/                     git submodule: aml-psdk
```

## Regenerating the script header

```sh
node tools/gen_script_header.js \
    "/path/to/selector de ropa.csi" mod/selector_script.h
```

(The script filename contains spaces — always quote it.)

## Building

```sh
/data/data/com.termux/files/home/tools/android-ndk-r29/ndk-build clean
/data/data/com.termux/files/home/tools/android-ndk-r29/ndk-build
```

Output: `libs/arm64-v8a/libAML_PSDK_SelectorLauncher.so`

arm64-v8a only, `APP_PLATFORM := android-24`. Drop the `.so` into the AML mods
directory.

## Submodule

```sh
git submodule add https://github.com/AndroidModLoader/aml-psdk.git psdk
```
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
twice while one is alive would register a duplicate, so `OnRelease` checks the
game's live-script list first. Unlike a once-per-process flag, this releases as
soon as the script ends — which it does on its own the moment the player enters
a vehicle — so **the button stays re-runnable**.

### Identity check: instruction pointer, not script name

The script bytecode declares `03A4 SCRIPT_NAME "HAF"`, but **`m_szName` is not
usable as an identity check.** Disassembling `StartNewScript` shows it hardcodes
the field to `"noname"` and never reads the name out of the buffer:

```asm
mov  w9, #0x6f6e          ; w9 = 0x616e6f6e
mov  w8, #0x656d
str  w9, [x19, #0x10]     ; m_szName[0..3] = 'n','o','n','a'
strh w8, [x19, #0x14]     ; m_szName[4..5] = 'm','e'
strb wzr, [x19, #0x16]    ; m_szName[6]    = 0
```

So the script is identified by its instruction pointer instead. The same
function stores the buffer we passed in as the initial IP:

```asm
str  x20, [x19, #0x20]    ; m_pCurrentIP = our buffer
stp  x9, xzr, [x19]       ; link into CTheScripts::pActiveScripts
```

The IP stays inside that buffer for the script's whole life, so any live
`CRunningScript` whose `m_pCurrentIP` falls within `[SELECTOR_SCRIPT,
SELECTOR_SCRIPT + LEN)` is ours.

`CTheScripts::pActiveScripts` is resolved by name too — note the mangling is
`_ZN11CTheScripts14pActiveScriptsE` (**14**, the length of `pActiveScripts`, not
16). It is an `OBJECT GLOBAL` at `0x9a2028` in `.bss`, i.e. a `CRunningScript*`,
so the resolved symbol is dereferenced to obtain the list head.

`CRunningScript`'s layout was cross-checked against the game's own DWARF:
`pNext` @ `0x00`, `pPrevious` @ `0x08`, `ScriptName` @ `0x10` (`Char[8]`),
`BaseAddressOfThisScript` @ `0x18`, `PCPointer` @ `0x20`; struct size `0x130` —
matching the psdk declaration used here.

## The vehicle hint

The script checks whether the player is driving and **stops as soon as they get
into a vehicle**, so driving is exactly the state in which it cannot run. The
`SetTick` callback draws a status line under the button:

| State | Text |
|---|---|
| In a vehicle, script not running | `STEP OUT OF VEHICLE TO RUN` |
| On foot, script not running | `PRESS SELECTOR` |
| Script running | `SELECTOR RUNNING` |

Detection uses the psdk's `FindPlayerVehicle(0, false)`. That accessor is
declared with `DECL_FASTCALL_SIMPLE_GLO`, which resolves the mangled symbol
through `aml->GetSym` — no baked-in address, so it is safe. If the symbol is
missing, the call returns `NULL` and the "press" hint is shown, which is the
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
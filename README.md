# SA Selector Launcher

An Android Mod Loader (AML) mod for GTA:SA mobile that shows a small tab on the
top edge of the screen. Tapping it runs the CLEO-Android script
`selector de ropa.csi` inside the game's own script VM.

> **This is an adaptation of another author's mod.**
> The script `selector de ropa.csi` — and everything it draws: the category
> menus, the item lists, the two-column camera preview, the clothing preview —
> is the work of **Alexander Blade** (CLEO / CLEO-Android). It is embedded here
> byte for byte, 4882 bytes, unmodified.
>
> What this repository adds is only the **loader**: the AML mod that puts a tab
> on screen, finds the game function that runs scripts, and hands the bytecode
> over. None of the selector's actual behaviour is implemented in this codebase.
>
> Alexander Blade's own `.fxt` asks people to contact him about his mods rather
> than redistribute them, so **if you want this script, get it from him**. This
> repository exists to show that the script can be driven from AML without
> installing CLEO, and to document the two non-obvious things that makes
> possible. If you are the author and would rather it not be here, say so and it
> comes down.

## How it works

The script uses **only vanilla GTA:SA mobile opcodes** — all 29 distinct ones it
uses are present in the native opcode table, and there are no CLEO extensions to
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

This is the single-argument overload; `...StartNewScriptEPht` is a separate
symbol and is not used. It returns `CRunningScript*` (not `void` — see below).

## The two non-obvious things

Everything else in here is plumbing. These two are the whole reason this mod
works, and both were found by reading `libGTASA.so`.

### 1. `StartNewScript` leaves `m_pBaseIP` NULL, and jumps need it

Without this fix the game **crashes with SIGSEGV on the very first instruction**.

The script's first instruction is a jump over its own header:

```
02 00 01 F1 FF FF FF     GOTO, operand type 01, operand -15
A4 03 0E 03 48 41 46     03A4 SCRIPT_NAME "HAF"
00                        padding
0F                       first real instruction
```

The GOTO handler is not a plain relative jump. It branches on the sign:

```asm
0x3f4cf4: ldrsw x8, [x8]            ; operand, sign-extended
0x3f4cf8: tbnz w8, #0x1f, 0x3f4e0c  ; bit 31 set -> use the script's own base
0x3f4cfc: ldr  x9, [x9]             ; else -> CTheScripts::ScriptSpace
0x3f4d04: add  x8, x9, x8           ;     newIP = &ScriptSpace + operand
0x3f4e0c: ldr  x9, [x19, #0x18]     ; x19 + 0x18 = CRunningScript::m_pBaseIP
0x3f4e10: sub  x8, x9, x8           ;     newIP = m_pBaseIP - operand
0x3f4e18: str  x8, [x19, #0x20]     ; m_pCurrentIP = newIP
```

**A negative operand is not corruption — it selects the base-relative path.**
`|operand|` is the absolute offset, and `newIP = m_pBaseIP + |operand|`. Scripts
inside `mainV1.scm` legitimately use positive offsets because they live in
`ScriptSpace`; a script in an external buffer must use negative ones. Both forms
are correct for their own case.

`StartNewScript` sets `m_pCurrentIP` but `memset`s `m_pBaseIP` to zero and never
writes it back — the game only fills that field for scripts it loads from
`ScriptSpace`. With a NULL base, the first instruction computes:

```
0 - (-15) = 15 = 0xF
```

and the VM then tries to read an opcode from address `0xF`. That is the crash:
`Fault address: 0xF`, `SEGV_MAPERR`, in `CRunningScript::Process`.

The fix is one line, using the pointer `StartNewScript` hands back:

```cpp
script->m_pBaseIP = (unsigned char*)SELECTOR_SCRIPT;
```

`0x18` is not a guess — the game's own DWARF names it `BaseAddressOfThisScript`,
type `UInt8 *`, `script.h:417`.

### 2. `StartNewScript` hardcodes the script name to `"noname"`

The bytecode declares `03A4 SCRIPT_NAME "HAF"`, but that name never reaches
`m_szName`:

```asm
mov  w9, #0x6f6e          ; w9 = 0x616e6f6e
mov  w8, #0x656d
str  w9, [x19, #0x10]     ; m_szName[0..3] = 'n','o','n','a'
strh w8, [x19, #0x14]     ; m_szName[4..5] = 'm','e'
strb wzr, [x19, #0x16]    ; m_szName[6]    = 0
```

So `m_szName` is useless as an identity check — matching on `"HAF"` would be
false for every script, always. The same function stores the buffer we passed in
as the initial IP:

```asm
str  x20, [x19, #0x20]    ; m_pCurrentIP = our buffer
stp  x9, xzr, [x19]       ; link into CTheScripts::pActiveScripts
```

The IP stays inside that buffer for the script's whole life, so any live
`CRunningScript` whose `m_pCurrentIP` falls within `[SELECTOR_SCRIPT,
SELECTOR_SCRIPT + LEN)` is ours.

`CTheScripts::pActiveScripts` is resolved by name too — note the mangling is
`_ZN11CTheScripts14pActiveScriptsE` (**14**, the length of `pActiveScripts`, not
16). It is an `OBJECT GLOBAL` of size 8 in `.bss`, i.e. a `CRunningScript*`, so
the resolved symbol is dereferenced to obtain the list head.

`CRunningScript`'s layout was cross-checked against the game's own DWARF:
`pNext` @ `0x00`, `pPrevious` @ `0x08`, `ScriptName` @ `0x10` (`Char[8]`),
`BaseAddressOfThisScript` @ `0x18`, `PCPointer` @ `0x20`; struct size `0x130`,
matching the psdk declaration used here.

## Launching only once

`StartNewScript` **registers** the script with the game's script VM. Calling it
twice while one is alive would register a duplicate, so the tap handler walks
the live-script list first (see above). Unlike a once-per-process flag this
releases as soon as the script ends — which it does on its own the moment the
player enters a vehicle — so **the tab stays re-runnable**.

There is a dead-man's switch: if the script is still listed 20 s later, no game
menu is open and the pointer has been idle for 5 s, it is treated as a zombie and
the tab is shown again, with a log line. This is a safety net, not the normal
path — the normal way out is the script's own back button.

## The tab

A small rounded tab hanging off the top edge, centred horizontally, 44–72 px wide
depending on screen size. Drawn entirely with Menu Kit's immediate-mode canvas,
no game widget.

It disappears while the script runs, because the script's own menus provide their
own navigation. Vehicle detection uses the psdk's `FindPlayerVehicle(0, false)`,
which is declared with `DECL_FASTCALL_SIMPLE_GLO` and resolves by mangled name —
no baked-in address.

Two things worth knowing if you edit the drawing code:

- **Never use `DrawCircle(..., filled=0)`.** This canvas strokes the full 360° and
  closes it, so the seam renders as a wedge — a "pacman". Menu Kit's own docs and
  the sibling music-player both work around it by building arcs from open
  polylines instead.
- **Translucent pieces must not overlap.** `L_BODY` is drawn at alpha `0x66`; if
  two pieces cover the same pixel, alpha compositing happens twice and the
  overlap shows up as a dark lobe. The tab's quarter discs are sized to abut the
  slab, not to overlap it.

## Menu Kit

Menu Kit is **compiled into this library** as source — there is no runtime
dependency on the standalone Menu Kit mod. `Android.mk` sets `-DMENUKIT_EMBEDDED`,
which hides `GetMenuAPI` from the dynamic symbol table so this library is never
mistaken for the real Menu Kit, and `main.cpp` uses the embedded entry point:

```cpp
g_menuKit = MenuKit_InitEmbedded(aml);
```

`mod/` is vendored from [gta-sa-menu-kit](https://github.com/Jean7z/gta-sa-menu-kit)
(MIT).

## Known limitation: saving while the script runs

The script lives in this library's read-only data, **not** in the game's
`ScriptSpace`. If the game serialises it while saving, the save file carries a
script whose code lives outside the space it expects, and loading that save can
crash inside `CRunningScript::Process`.

This has been observed once and not reproduced; no Save hook is installed. If you
hit it, the diagnosis is a garbage `m_pCurrentIP` in a `CRunningScript` during
`CTheScripts::Process`.

## Layout

```
main.cpp                  mod entry point, tab, launch logic
mod/                      vendored Menu Kit + logger + amlmod headers
mod/selector_script.h     generated: the .csi as a byte array
tools/gen_script_header.js generates selector_script.h from the .csi
Android.mk / Application.mk
jni -> .                  symlink; ndk-build uses it to find the project root
psdk/                     git submodule: aml-psdk (GPL-3.0)
```

## Regenerating the script header

```sh
node tools/gen_script_header.js \
    "/path/to/selector de ropa.csi" mod/selector_script.h
```

(The script filename contains spaces — always quote it.) The generator verifies
the input's SHA-256, so it will refuse a file that is not the expected one.

## Building

```sh
/path/to/android-ndk/ndk-build clean
/path/to/android-ndk/ndk-build
```

Output: `libs/arm64-v8a/libAML_PSDK_SelectorLauncher.so`

arm64-v8a only, `APP_PLATFORM := android-24`. Drop the `.so` into
`Android/data/com.rockstargames.gtasa/mods/`.

## Credits

- **Alexander Blade** — the `selector de ropa.csi` script and everything it draws.
  This is an adaptation of his mod; the script is his, unmodified.
- [gta-sa-menu-kit](https://github.com/Jean7z/gta-sa-menu-kit) — the embedded UI
  layer (MIT).
- [aml-psdk](https://github.com/AndroidModLoader/aml-psdk) — the SDK headers (GPL-3.0).
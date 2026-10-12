/*
 *  SA Selector Launcher
 *
 *  Adds a single "SELECTOR" button to the game screen. Pressing it hands the
 *  embedded CLEO-Android script "selector de ropa.csi" to the game's own
 *  script VM via CTheScripts::StartNewScript.
 *
 *  Why this works without an interpreter: the script uses only vanilla GTA:SA
 *  mobile opcodes (all 29 are present in the native opcode table), so there are
 *  no CLEO extensions to emulate. The game parses and runs it as-is. We only
 *  need a loader, and the game already exports one.
 *
 *  CTheScripts::StartNewScript is resolved BY NAME through AML rather than by a
 *  hardcoded offset, so the mod keeps working across libGTASA.so builds.
 */

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/menu-api.h>

#include <stdint.h>

// Vehicle detection. FindPlayerVehicle is declared with DECL_FASTCALL_SIMPLE_GLO,
// which resolves the mangled symbol through AML's GetSym (dlsym) -- no baked-in
// address, so it is safe to use here.
#include <aml-psdk/game_sa/entity/PlayerPed.h>

#include <mod/selector_script.h>

MYMOD(net.psdk.samod.selectorlauncher, SA Selector Launcher, 1.0, Jean7z)

/* CTheScripts::StartNewScript(unsigned char const*) -> int.
 *
 * Itanium mangled name: _ZN11CTheScripts14StartNewScriptEPh
 *
 * The return value is deliberately declared void: the ARM64 ABI makes ignoring
 * a real int return safe, and the script slot's identity is not something we
 * want to depend on. Note this is the single-argument overload; the two-argument
 * `...EPht` overload is a different symbol and is not used.
 *
 * StartNewScript copies the bytecode into a CRunningScript-owned buffer, so
 * pointing it at the static const array below is safe. */
typedef void (*StartNewScript_t)(const uint8_t*);

static StartNewScript_t    g_pStartNewScript  = NULL;
static const MenuKitAPI*   g_menuKit          = NULL;
static void*               g_button           = NULL;

/* The script registers itself with the game's script VM on start. Starting it a
 * second time from the same buffer would register a duplicate script, so the
 * launch is guarded to fire at most once per process. */
static bool g_launched = false;

static void OnRelease(void*)
{
    if(g_launched)
    {
        // Already running. Starting it again would double-register the script.
        return;
    }

    if(!g_pStartNewScript)
    {
        logger->Error("CTheScripts::StartNewScript is unresolved; cannot start the selector script");
        return;
    }

    g_pStartNewScript(SELECTOR_SCRIPT);
    g_launched = true;

    logger->Info("Selector script started (CTheScripts::StartNewScript, %u bytes)", SELECTOR_SCRIPT_LEN);
}

/* Returns true when the player is currently driving. */
static bool IsInVehicle(void)
{
    return FindPlayerVehicle(0, false) != NULL;
}

/* Runs on the game's processing thread every frame, so it stays short: one
 * rect query and one text draw. */
static void OnTick(void*)
{
    if(!g_menuKit)
        return;

    /* The script bails out immediately if the player is not driving, which
     * looks like a dead button. Surface that requirement as a hint instead. */
    static const char* msgHint    = "GET IN A VEHICLE FIRST";
    static const char* msgRunning = "SELECTOR RUNNING";

    const char* msg;
    uint32_t    color;
    if(g_launched)
    {
        msg   = msgRunning;
        color = 0x38BDF8FFu;
    }
    else if(IsInVehicle())
    {
        msg   = "READY - PRESS SELECTOR";
        color = 0x9BE36BFFu;
    }
    else
    {
        msg   = msgHint;
        color = 0xF5A623FFu;
    }

    /* Anchor the hint just under the button when its rect is known, otherwise
     * fall back to a fixed spot on screen. */
    float x = 24.0f, y = 24.0f;
    float l, t, r, b;
    if(g_button && g_menuKit->GetRect(g_button, &l, &t, &r, &b))
    {
        x = l;
        y = b + 6.0f;
    }

    g_menuKit->DrawText(x, y, msg, 2, color);
}

ON_MOD_LOAD()
{
    logger->SetTag("SelectorLauncher");
    logger->Info("SA Selector Launcher loading");

    /* Resolve the game's script loader by name (version independent). */
    g_pStartNewScript = (StartNewScript_t)aml->GetSym(
        aml->GetLib("libGTASA.so"),
        "_ZN11CTheScripts14StartNewScriptEPh"
    );

    if(!g_pStartNewScript)
    {
        logger->Error("Failed to resolve _ZN11CTheScripts14StartNewScriptEPh in libGTASA.so");
    }
    else
    {
        logger->Info("Resolved CTheScripts::StartNewScript");
    }

    /* Menu Kit is compiled into this library; use the embedded entry point
     * rather than looking up a separate Menu Kit mod. */
    g_menuKit = MenuKit_InitEmbedded(aml);
    if(!g_menuKit)
    {
        logger->Error("MenuKit embedded initialization failed");
        return;
    }

    g_button = g_menuKit->AddButton(0, "_shoot", 320, 380, 20, OnRelease, NULL, NULL);
    if(g_button)
    {
        g_menuKit->SetText(g_button, "SELECTOR");
        g_menuKit->SetSize(g_button, 220, 90);
    }

    g_menuKit->SetTick(OnTick, NULL);
}

ON_MOD_UNLOAD()
{
    MenuKit_Shutdown();
}
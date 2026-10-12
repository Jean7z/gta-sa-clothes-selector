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
#include <aml-psdk/game_sa/engine/RunningScript.h>

#include <mod/selector_script.h>

MYMOD(net.psdk.samod.selectorlauncher, SA Selector Launcher, 1.0, Jean7z)

/* CTheScripts::StartNewScript(unsigned char const*)
 *
 * Itanium mangled name: _ZN11CTheScripts14StartNewScriptEPh
 *
 * The return type does not appear in the mangled name. The function actually
 * returns the CRunningScript* it just created (mov x0, x19 before ret), which is
 * required: StartNewScript does NOT fully initialise the script (see OnRelease).
 *
 * Note this is the single-argument overload. The two-argument `...EPht` variant
 * is a different symbol, used for reloading an existing script slot, and has
 * the same initialisation behaviour.
 */
typedef CRunningScript* (*StartNewScript_t)(const uint8_t*);

static StartNewScript_t    g_pStartNewScript  = NULL;
static const MenuKitAPI*   g_menuKit          = NULL;
static void*               g_button           = NULL;

/* CTheScripts::pActiveScripts -- the head of the game's list of live
 * CRunningScript objects. psdk does not declare CTheScripts, so the symbol is
 * resolved directly by name. It is a `CRunningScript*` global living in .bss,
 * so this variable holds its ADDRESS; dereference to get the list head.
 *
 * Itanium mangled name: _ZN11CTheScripts14pActiveScriptsE  (14 == strlen("pActiveScripts")) */
static CRunningScript**    g_ppActiveScripts  = NULL;

/* Is our script currently live in the game's script VM?
 *
 * NOTE ON THE NAME: CRunningScript::m_szName (offset 0x10, Char[8]) is NOT a
 * usable identity check here. StartNewScript hardcodes it to "noname":
 *
 *     mov  w9, #0x6f6e            ; 0x616e6f6e
 *     mov  w8, #0x656d
 *     str  w9, [x19, #0x10]       ; m_szName[0..3] = 'n','o','n','a'
 *     strh w8, [x19, #0x14]       ; m_szName[4..5] = 'm','e'
 *     strb wzr, [x19, #0x16]      ; m_szName[6]    = 0
 *
 * so the `03A4 SCRIPT_NAME "HAF"` in the bytecode never reaches the field.
 *
 * Instead, identify the script by its instruction pointer. StartNewScript does
 *     str  x20, [x19, #0x20]      ; m_pCurrentIP = the buffer we passed in
 * and the IP stays inside that buffer for the script's whole life, so any live
 * CRunningScript whose m_pCurrentIP falls within SELECTOR_SCRIPT is ours. */
static bool IsSelectorRunning()
{
    if(!g_ppActiveScripts)
        return false;

    const unsigned char* pBegin = SELECTOR_SCRIPT;
    const unsigned char* pEnd   = SELECTOR_SCRIPT + SELECTOR_SCRIPT_LEN;

    for(CRunningScript* script = *g_ppActiveScripts; script; script = script->m_pNext)
    {
        const unsigned char* ip = (const unsigned char*)script->m_pCurrentIP;
        if(ip >= pBegin && ip < pEnd)
            return true;
    }

    return false;
}

static void OnRelease(void*)
{
    if(!g_pStartNewScript)
    {
        logger->Error("CTheScripts::StartNewScript is unresolved; cannot start the selector script");
        return;
    }

    /* StartNewScript registers the script in the game's script VM, so refuse to
     * add a second copy while one is alive. Unlike a once-per-process flag this
     * releases as soon as the script ends, which it does on its own the moment
     * the player gets into a vehicle -- so the button stays re-runnable. */
    if(IsSelectorRunning())
    {
        logger->Info("Selector script is already running; not starting a second copy");
        return;
    }

    CRunningScript* script = g_pStartNewScript(SELECTOR_SCRIPT);
    if(!script)
    {
        logger->Error("CTheScripts::StartNewScript returned NULL; selector script not started");
        return;
    }

    /* Set the script base pointer, which StartNewScript leaves NULL.
     *
     * StartNewScript does `memset(&script + 0x18, 0, 0x52)` and then writes only
     * m_pCurrentIP (0x20) = our buffer. It never sets m_pBaseIP (0x18), and
     * neither does CRunningScript::Init(). Scripts inside main.scm get their base
     * from the scheduler when the game loads them, so the game never needed to
     * do it here -- but this is the path for an externally supplied buffer.
     *
     * The vanilla jump handler requires it. In
     * CRunningScript::ProcessCommands0To99, the shared jump path at 0x3f4cec:
     *
     *     ldrsw x8, [x8]              ; x8 = sign-extended operand
     *     tbnz  w8, #0x1f, negative    ; sign selects the base
     *     ...  add  x8, x9, x8         ; operand >= 0: &CTheScripts::ScriptSpace + op
     *     ...  sub  x8, x9, x8         ; operand <  0: m_pBaseIP - op  == base + |op|
     *     str  x8, [x19, #0x20]       ; m_pCurrentIP = result
     *
     * A NEGATIVE operand is therefore not corrupt -- it is the selector for the
     * script-base-relative path, and |operand| is the absolute file offset. Every
     * jump in this .csi is negative for exactly that reason.
     *
     * With m_pBaseIP == NULL the first instruction (GOTO, operand -15) computes
     * 0 - (-15) == 15 == 0xF, and the VM segfaults fetching the next opcode at
     * 0xF. Setting the base makes it SELECTOR_SCRIPT + 15 == +0x0F, which is the
     * first real instruction, past the `03A4 SCRIPT_NAME "HAF"` header.
     *
     * Offset 0x18 is not guessed. The game ships DWARF for this exact member:
     *   BaseAddressOfThisScript, type `UInt8 *`, script.h:417
     * which is the member named m_pBaseIP here. CHECKSIZE(CRunningScript, 0x100,
     * 0x130) pins the arm64 layout against the game's own DWARF byte size.
     *
     * Done in the same call as StartNewScript, before returning to the game loop,
     * so the scheduler cannot observe the script with a NULL base. */
    script->m_pBaseIP = (unsigned char*)SELECTOR_SCRIPT;

    logger->Info("Selector script started (CTheScripts::StartNewScript, %u bytes, base set)", SELECTOR_SCRIPT_LEN);
}

/* Returns true when the player is currently driving. */
static bool IsInVehicle(void)
{
    return FindPlayerVehicle(0, false) != NULL;
}

/* Runs on the game's processing thread every frame, so it stays short: one
 * short list walk and one text draw. */
static void OnTick(void*)
{
    if(!g_menuKit)
        return;

    /* The script checks whether the player is driving and stops as soon as they
     * get into a vehicle, so driving is the state in which it CANNOT run. Show
     * that requirement as a hint instead of letting the button look dead. */
    const char* msg;
    uint32_t    color;
    if(IsSelectorRunning())
    {
        msg   = "SELECTOR RUNNING";
        color = 0x38BDF8FFu;
    }
    else if(IsInVehicle())
    {
        msg   = "STEP OUT OF VEHICLE TO RUN";
        color = 0xF5A623FFu;
    }
    else
    {
        msg   = "PRESS SELECTOR";
        color = 0x9BE36BFFu;
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

    /* Resolve the head of the live-script list. Without this the mod can start
     * the script but cannot tell whether it is still running, and would happily
     * register duplicate copies on every press. */
    g_ppActiveScripts = (CRunningScript**)aml->GetSym(
        aml->GetLib("libGTASA.so"),
        "_ZN11CTheScripts14pActiveScriptsE"
    );

    if(!g_ppActiveScripts)
    {
        logger->Error("Failed to resolve _ZN11CTheScripts14pActiveScriptsE in libGTASA.so");
    }
    else
    {
        logger->Info("Resolved CTheScripts::pActiveScripts");
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
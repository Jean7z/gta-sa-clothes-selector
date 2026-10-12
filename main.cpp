/*
 *  SA Selector Launcher
 *
 *  Pone un boton-redondo en pantalla que, al tocarlo, arranca el script
 *  "selector de ropa.csi" dentro de la propia VM de scripts del juego.
 *
 *  Como funciona sin interprete: el script usa SOLO opcodes vanilla de GTA:SA
 *  mobile (los 29 estan en la tabla nativa), asi que no hay extensiones CLEO que
 *  emular. El juego lo parsea y lo ejecuta tal cual. Solo hace falta un cargador,
 *  y el juego ya exporta uno: CTheScripts::StartNewScript, que se resuelve por
 *  nombre de simbolo (no por offset) a traves de AML.
 *
 *  La UI es lienzo puro: no hay ningun widget visible. Todo (boton, glifo,
 *  texto de estado) se dibuja en el canvas desde SetTick y el toque se lee con
 *  GetPointer. El unico widget que se crea es una sonda invisible que mide el
 *  render real y se devuelve al pool enseguida.
 */

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/menu-api.h>

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <time.h>

// FindPlayerVehicle va declarado con DECL_FASTCALL_SIMPLE_GLO, que resuelve el
// simbolo por AML (dlsym). Sin direcciones fijadas a mano, asi que es seguro.
#include <aml-psdk/game_sa/entity/PlayerPed.h>
#include <aml-psdk/game_sa/engine/RunningScript.h>

#include <mod/selector_script.h>

MYMOD(net.psdk.samod.selectorlauncher, SA Selector Launcher, 1.0, Jean7z)

/* ------------------------------------------------------------------ */
/* Constantes de la UI                                                 */
/* ------------------------------------------------------------------ */

/* Lanzador. Abajo a la DERECHA: abajo-izquierda esta el radar y estorba. El
   margen y el radio se calculan en tiempo real desde el tamano medido del
   render, nunca como constantes, porque cada dispositivo tiene el suyo. */
#define LAUNCH_R_MIN   34.0f    /* radio minimo, en pantallas pequenas */
#define LAUNCH_R_MAX   40.0f    /* radio maximo, en pantallas grandes */
#define LAUNCH_R_K     0.052f   /* radio = min(screenW,screenH) * esto, acotado */

/* Lo que toca es un dedo y no un raton, asi que el acierto se ensancha. */
#define BTN_TOUCH_PAD  10.0f

/* Cuanto se encoge el disco al pulsarlo y con que rapidez vuelve: el valor
   persigue al destino cada frame en vez de saltar. */
#define PRESS_SCALE  0.94f
#define PRESS_EASE   0.25f

/* Respiracion en reposo: seno lentisimo sobre el alfa del halo. La amplitud es
   pequena a proposito, para que se note vivo sin robarle sitio a la partida. */
#define BREATHE_AMP  0.10f
#define BREATHE_MS   6000.0f

/* Texto de estado, centrado respecto al boton y NUNCA encima del disco. */
#define HINT_SCALE    2
#define HINT_GAP     16.0f    /* separacion entre el borde del disco y el texto */
#define HINT_MARGIN   8.0f    /* margen minimo del texto contra el borde */

/* Fundido por inactividad: PASA UN RATO sin tocar y se atenua hasta un PISO, no
   hasta 0. El boton no llega a desaparecer nunca, y esa es toda la garantia de
   que siga siendo alcanzable: si se fuera a alfa 0 no habria forma de tocarlo
   para despertarlo, y habria que inventar un rescate aparte.

   El piso no es una puerta: el hit-test NO mira el alfa, asi que un icono
   atenuado se toca igual que uno brillante. El apagado es estetica, no bloqueo. */
#define ICO_FADE_DELAY 6000.0f  /* ms sin presion antes de empezar a apagarse */
#define ICO_FADE_TIME   900.0f  /* ms que dura el fundido */
#define ICO_FADE_MIN    0.38f  /* piso de opacidad */

/* Tope de espera de la sonda antes de asumir un tamano. */
#define PROBE_TIMEOUT 1500u

/* Colores en 0xRRGGBBAA (rojo en el byte alto, ALFA en el bajo). MenuKit
   desempaqueta asi en Canvas_Push, y es el mismo convenio para DrawCircle,
   DrawLine y DrawText. */
#define L_BODY       0x0B0F1A66u  /* cuerpo: negro azulado translucido, alfa 0x66 */

/* Los tres colores de estado son los MISMOS que los del texto de abajo, para
   que el anillo insinue lo mismo que dice la frase. */
#define C_READY      0x9BE36BFFu  /* a pie, sin lanzar */
#define C_VEHICLE    0xF5A623FFu  /* conduciendo: el script no puede correr */
#define C_RUNNING    0x38BDF8FFu  /* selector en marcha */
#define L_PRESS      0xFFFFFFFFu  /* glifo pulsado: blanco, mas claro */

typedef CRunningScript* (*StartNewScript_t)(const uint8_t*);

static StartNewScript_t    g_pStartNewScript  = NULL;
static const MenuKitAPI*   g_menuKit          = NULL;

/* CTheScripts::pActiveScripts -- cabeza de la lista de CRunningScript vivos del
   juego. psdk no declara CTheScripts, asi que se resuelve por nombre. Es un
   `CRunningScript*` global en .bss, asi que esta variable guarda su DIRECCION;
   hay que desreferenciarla para sacar la cabeza. */
static CRunningScript**    g_ppActiveScripts  = NULL;

/* Sonda de tamano de pantalla: widget cuadrado, centrado en el centro del
   espacio virtual (320,224) que el motor proyecta al centro EXACTO del render, y
   APARCADO (SetVisible 0 = ni se dibuja ni se toca). OnTick lee su rect una vez
   y lo devuelve al pool. */
static void*               g_probe            = NULL;
static float               g_screenW          = 0.0f;
static float               g_screenH          = 0.0f;
static uint64_t            g_probeAt          = 0;

/* Geometria del frame actual (la escribe LayoutLauncher). El hit-test necesita
   las coordenadas y el radio igual que el dibujo, y ambos salen de aqui, para
   que en una pantalla grande no se dibujen grande y se toquen pequeno. */
static float               g_launchX = 0.0f, g_launchY = 0.0f, g_launchR = 0.0f;

/* Escala del disco, 1.0 en reposo y PRESS_SCALE mientras se pulsa. Persigue al
   destino cada frame: por eso hace falta GUARDARLA entre frames. */
static float               g_pressK = 1.0f;

/* Instante del ultimo toque SOBRE el lanzador. El reloj del fundido corre a
   partir de aqui. */
static uint64_t            g_idleAt  = 0;

/* Flanco del dedo: sin el estado ANTERIOR, un dedo que se queda apoyado sobre
   el icono lo pulsaria en cada frame. */
static bool                g_wasDown = false;
static bool                g_press   = false;
static uint64_t            g_tickPrev = 0;

static uint64_t NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Modula el ALFA de un color 0xRRGGBBAA. El alfa es el byte bajo, asi que se
   cambia solo ese byte y el RGB se queda como estaba. */
static unsigned int FadeAlpha(unsigned int c, float k)
{
    const unsigned int a = (unsigned int)((float)(c & 0xFFu) * k);
    return (c & 0xFFFFFF00u) | (a & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* Arranque del script                                                 */
/* ------------------------------------------------------------------ */

/* Esta en la VM del juego ahora mismo?
 *
 * OJO CON EL NOMBRE: CRunningScript::m_szName (offset 0x10, Char[8]) NO sirve
 * como identidad. StartNewScript lo fija a "noname" a pelo:
 *
 *     mov  w9, #0x6f6e            ; 0x616e6f6e
 *     mov  w8, #0x656d
 *     str  w9, [x19, #0x10]       ; m_szName[0..3] = 'n','o','n','a'
 *     strh w8, [x19, #0x14]       ; m_szName[4..5] = 'm','e'
 *     strb wzr, [x19, #0x16]      ; m_szName[6]    = 0
 *
 * o sea que el `03A4 SCRIPT_NAME "HAF"` del bytecode nunca llega al campo.
 *
 * En su lugar se identifica por el puntero de instruccion: StartNewScript hace
 *     str  x20, [x19, #0x20]      ; m_pCurrentIP = el buffer que le pasamos
 * y el IP se mantiene dentro de ese buffer toda la vida del script, asi que
 * cualquier CRunningScript vivo cuyo m_pCurrentIP caiga dentro de
 * SELECTOR_SCRIPT es el nuestro. */
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

static void LaunchSelector(void)
{
    if(!g_pStartNewScript)
    {
        logger->Error("CTheScripts::StartNewScript is unresolved; cannot start the selector script");
        return;
    }

    /* StartNewScript REGISTRA el script en la VM del juego, asi que no se admite
     * una segunda copia mientras viva una. A diferencia de un flag de "una vez
     * por proceso", esto se libera en cuanto el script acaba, que ocurre solo en
     * cuanto el jugador se mete en un vehiculo, de modo que el boton sigue
     * siendo reutilizable. */
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

    /* Pone el puntero base del script, que StartNewScript deja a NULL.
     *
     * StartNewScript hace `memset(&script + 0x18, 0, 0x52)` y luego escribe solo
     * m_pCurrentIP (0x20) = nuestro buffer. Nunca pone m_pBaseIP (0x18), y
     * CRunningScript::Init() tampoco. Los scripts de main.scm reciben su base
     * del planificador cuando el juego los carga, asi que el juego nunca ha
     * necesitado hacerlo aqui, pero este es el camino para un buffer externo.
     *
     * El gestor de saltos vanilla lo necesita. En
     * CRunningScript::ProcessCommands0To99, el camino comun de salto en 0x3f4cec:
     *
     *     ldrsw x8, [x8]              ; x8 = operando con signo
     *     tbnz  w8, #0x1f, negativo    ; el SIGNO elige la base
     *     ...  add  x8, x9, x8         ; op >= 0: &CTheScripts::ScriptSpace + op
     *     ...  sub  x8, x9, x8         ; op <  0: m_pBaseIP - op  == base + |op|
     *     str  x8, [x19, #0x20]       ; m_pCurrentIP = resultado
     *
     * Un operando NEGATIVO no esta corrupto: es el selector del camino relativo
     * a la base del script, y |operando| es el offset absoluto dentro del
     * fichero. Todos los saltos de este .csi son negativos por eso exactamente.
     *
     * Con m_pBaseIP == NULL la primera instruccion (GOTO, operando -15) calcula
     * 0 - (-15) == 15 == 0xF y la VM peta al leer el siguiente opcode en 0xF.
     * Poner la base hace que salga SELECTOR_SCRIPT + 15 == +0x0F, que es la
     * primera instruccion de verdad, pasado el `03A4 SCRIPT_NAME "HAF"`.
     *
     * El offset 0x18 no esta adivinado. El juego trae DWARF para ese miembro
     * exacto:  BaseAddressOfThisScript, tipo `UInt8 *`, script.h:417, que es el
     * miembro que aqui se llama m_pBaseIP. CHECKSIZE(CRunningScript, 0x100, 0x130)
     * fija el layout de arm64 contra el tamano que declara el propio DWARF.
     *
     * Se hace en la misma llamada que StartNewScript, antes de volver al bucle
     * del juego, para que el planificador no pueda ver el script con base NULL. */
    script->m_pBaseIP = (unsigned char*)SELECTOR_SCRIPT;

    logger->Info("Selector script started (CTheScripts::StartNewScript, %u bytes, base set)", SELECTOR_SCRIPT_LEN);
}

/* Devuelve true si el jugador esta conduciendo. */
static bool IsInVehicle(void)
{
    return FindPlayerVehicle(0, false) != NULL;
}

/* ------------------------------------------------------------------ */
/* Sonda de pantalla                                                   */
/* ------------------------------------------------------------------ */

/* Mide el render real con un widget sonda y lo devuelve al pool.
   OS_ScreenGetWidth() no sirve: dio 1024x600 con un render de 1600x720. El
   widget va centrado en el centro del espacio virtual (320,224), que el motor
   proyecta al centro del render, asi que con el borde izquierdo de un cuadrado
   de 100 px sale el ancho (W = (left + 50) * 2) y con el superior el alto. Se
   descarta en cuanto se logra y, si el motor no llegara a construirlo, se cae al
   tamano medido en este dispositivo para que la UI siga pintandose en vez de
   desaparecer. */
static void ProbeScreen(void)
{
    float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
    uint64_t now = NowMs();

    if(g_screenW > 0.0f) return;

    if(g_probe)
    {
        if(g_menuKit->GetRect(g_probe, &l, &t, &r, &b))
        {
            g_screenW = (l + 50.0f) * 2.0f;
            g_screenH = (t + 50.0f) * 2.0f;
            g_menuKit->RemoveWidget(g_probe);
            g_probe = NULL;
            logger->Info("SelectorLauncher: render medido %.0fx%.0f px", g_screenW, g_screenH);
            return;
        }
        if(now - g_probeAt < PROBE_TIMEOUT) return;
    }

    g_screenW = 1600.0f;
    g_screenH = 720.0f;
    if(g_probe) { g_menuKit->RemoveWidget(g_probe); g_probe = NULL; }
    logger->Error("SelectorLauncher: la sonda no devolvio rect; se asumen %.0fx%.0f",
                  g_screenW, g_screenH);
}

/* ------------------------------------------------------------------ */
/* Layout, hit-test y dibujo                                           */
/* ------------------------------------------------------------------ */

/* Radio del disco, escalado por el lado corto de la pantalla y acotado para que
   ni se pierda en un movil pequeno ni se tragase el juego en una tablet. */
static float LaunchRadius(void)
{
    const float side = g_screenW < g_screenH ? g_screenW : g_screenH;
    float r = side * LAUNCH_R_K;
    if(r < LAUNCH_R_MIN) r = LAUNCH_R_MIN;
    if(r > LAUNCH_R_MAX) r = LAUNCH_R_MAX;
    return r;
}

/* Centro exacto de la pantalla. Se recalcula cada frame con el tamano medido por
   la sonda, nunca con un valor fijo.

   Choca a proposito con los menus de tienda del propio script, que tambien
   aparecen en el centro; por eso el lanzador se va a alfa 0 pasado un rato sin
   toques, y vuelve con el primero que haya en cualquier parte de la pantalla. */
static void LayoutLauncher(void)
{
    g_launchR = LaunchRadius();
    g_launchX = g_screenW * 0.5f;
    g_launchY = g_screenH * 0.5f;
}

/* Un rectangulo de toque alrededor del centro. Cuadrado y no circulo porque es
   lo que espera un dedo, y con BTN_TOUCH_PAD de margen el acierto no exige
   punteria. Usa g_launchR, el MISMO radio que dibuja el disco, asi que en
   cualquier tamano de pantalla el toque y el dibujo no se separan. */
static bool HitTest(float px, float py)
{
    const float r = g_launchR + BTN_TOUCH_PAD;
    return px >= g_launchX - r && px <= g_launchX + r &&
           py >= g_launchY - r && py <= g_launchY + r;
}

/* Color de estado. Son los mismos tres que usan las frases de abajo, para que el
   anillo insinue lo mismo que dice el texto. */
static unsigned int StateColor(void)
{
    if(IsSelectorRunning()) return C_RUNNING;
    if(IsInVehicle())      return C_VEHICLE;
    return C_READY;
}

/* Cuanto se ve el lanzador ahora mismo: 1.0 recien tocado, bajando hasta
   ICO_FADE_MIN pasado ICO_FADE_DELAY sin que se toque nada.

   NUNCA llega a 0. Ese piso es lo que garantiza que el boton siga siendo
   tocable: sin el habria que añadir una regla de rescate aparte, y no hace
   falta porque el hit-test no depende del alfa. */
static float IconFade(uint64_t now)
{
    float t;
    if(now <= g_idleAt || now - g_idleAt <= (uint64_t)ICO_FADE_DELAY) return 1.0f;
    t = (float)(now - g_idleAt - (uint64_t)ICO_FADE_DELAY) / ICO_FADE_TIME;
    if(t > 1.0f) t = 1.0f;
    return 1.0f - (1.0f - ICO_FADE_MIN) * t;
}

/* Percha: glifo de ropa, no de audio, porque esto abre un armario. Gancho en
   arco (DrawCircle sin relleno), viga, dos cuestas diagonales y barra
   horizontal. Cinco llamadas baratas: corre cada frame en el hilo del juego. */
static void DrawHangerGlyph(float cx, float cy, float r, unsigned int c)
{
    const float barY  = cy + r * 0.46f;
    const float halfW = r * 0.82f;
    const float peakY = cy - r * 0.32f;

    g_menuKit->DrawCircle(cx, cy - r * 0.82f, r * 0.24f, c, 0, 12);   /* gancho */
    g_menuKit->DrawLine(cx, cy - r * 0.58f, cx, peakY, c, 1.0f);      /* viga */
    g_menuKit->DrawLine(cx, peakY, cx - halfW, barY, c, 1.0f);        /* cuestas */
    g_menuKit->DrawLine(cx, peakY, cx + halfW, barY, c, 1.0f);
    g_menuKit->DrawLine(cx - halfW, barY, cx + halfW, barY, c, 1.0f);  /* barra */
}

/* Lanzador: disco translucido en el centro de la pantalla, con halo y anillo del
   color de estado. */
static void DrawLauncher(uint64_t now)
{
    const float cx = g_launchX, cy = g_launchY;
    const unsigned int st = StateColor();
    const float k = IconFade(now);          /* fundido por inactividad */
    const float br = 1.0f + BREATHE_AMP *
              sinf((float)(now % (uint64_t)BREATHE_MS) * (6.2831853f / BREATHE_MS));
    unsigned int a;
    float r;
    int i;

    /* Recuperacion suave del pulsado: el valor se guarda y se acerca al destino
       en vez de saltar de golpe. */
    g_pressK += ((g_press ? PRESS_SCALE : 1.0f) - g_pressK) * PRESS_EASE;
    r = g_launchR * g_pressK;

    /* Halo: tres discos concentricos RELLENOS con alfa decreciente y respirando
       despacio. Este API no tiene desenfoque, asi que el halo se simula apilando
       aros. Se dibujan ANTES del cuerpo para que este los tape por dentro. */
    for(i = 0; i < 3; ++i)
    {
        const float hr = r + 3.0f + (float)i * 3.0f;
        const unsigned int base = (unsigned int)(0x16u * br);
        a = (st & 0xFFFFFF00u) |
            (unsigned int)((base * (unsigned int)(3 - i)) / 3u) & 0xFFu;
        g_menuKit->DrawCircle(cx, cy, hr, FadeAlpha(a, k), 1, 32);
    }

    /* Cuerpo: negro azulado translucido. Se ve el juego por detras. */
    g_menuKit->DrawCircle(cx, cy, r, FadeAlpha(L_BODY, k), 1, 40);
    /* Anillo: acento fino, y es lo que dice en que estado estamos. */
    g_menuKit->DrawCircle(cx, cy, r - 0.5f, FadeAlpha(st, k), 0, 40);

    /* Glifo: blanco mientras se pulsa, del color de estado si no. */
    DrawHangerGlyph(cx, cy, r * 0.55f, FadeAlpha(g_press ? L_PRESS : st, k));
}

/* Texto de estado, centrado respecto al boton y por ENCIMA de el: nunca se
   superpone al disco. Se atenua con el MISMO factor que el disco, para que se
   vayan y vuelvan juntos. */
static void DrawHint(uint64_t now)
{
    const char* msg;
    unsigned int color;
    float w, x;

    /* El script comprueba si el jugador conducia y PARA en cuanto se mete en un
       vehiculo, asi que conducir es justo el estado en el que NO puede correr.
       Se enseña ese requisito como pista para que el boton no parezca muerto. */
    if(IsSelectorRunning())
    {
        msg   = "SELECTOR RUNNING";
        color = C_RUNNING;
    }
    else if(IsInVehicle())
    {
        msg   = "STEP OUT OF VEHICLE TO RUN";
        color = C_VEHICLE;
    }
    else
    {
        msg   = "PRESS SELECTOR";
        color = C_READY;
    }

    /* El avance de la fuente 5x7 es 6 * escala. Centrado sobre el boton y luego
       CORTADO contra los bordes, por si el texto se sale de la pantalla. */
    w = (float)strlen(msg) * 6.0f * HINT_SCALE;
    x = g_launchX - w * 0.5f;
    if(x < HINT_MARGIN) x = HINT_MARGIN;
    if(x + w > g_screenW - HINT_MARGIN) x = g_screenW - HINT_MARGIN - w;

    g_menuKit->DrawText(x,
                        g_launchY - g_launchR * g_pressK - HINT_GAP - 7.0f * HINT_SCALE,
                        msg, HINT_SCALE, FadeAlpha(color, IconFade(now)));
}

/* ------------------------------------------------------------------ */
/* Entrada y tick                                                      */
/* ------------------------------------------------------------------ */

/* Entrada y accion.

   El hit-test NO mira el alfa: el icono atenuado se toca igual que el brillante.
   Un toque abre siempre, este o no apagado, porque pedir dos golpes para lo
   mismo es engorroso. El apagado es estetica, no una puerta. Y como el piso es
   0.38 y no 0, nunca hay un boton invisible al que no se pueda llegar. */
static void HandleInput(uint64_t now)
{
    float px = 0.0f, py = 0.0f;
    int down = 0, id = 0;

    if(!g_menuKit->GetPointer(&px, &py, &down)) { g_wasDown = down; g_press = false; return; }

    /* Con el menu del juego abierto el dedo es suyo, no nuestro. El raw touch
       sigue llegando aqui mientras el mapa esta abierto, asi que sin esta puerta
       el toque con el que se cierra el mapa se lee como un tap en nuestro icono
       y nos abre el selector. GetMenuUp lee la pila de screens del propio juego,
       asi que es su definicion de "ocupado", no una heuristica.

       Salimos sin interpretar nada y resincronizamos el flanco con el estado
       real del dedo, para que el toque de cierre no quede pendiente. */
    if(g_menuKit->GetMenuUp && g_menuKit->GetMenuUp() == 1)
    {
        g_wasDown = down;
        g_press = false;
        g_tickPrev = now;
        return;
    }

    /* Respaldo para un framework viejo que no exponga GetMenuUp (-1): si el pump
       dejo de correr, el juego abrio su propia UI mientras tanto y el flanco que
       vemos no es nuestro. */
    if(g_menuKit->GetMenuUp && g_menuKit->GetMenuUp() < 0
       && now > g_tickPrev && now - g_tickPrev > 250u) g_wasDown = 1;
    g_tickPrev = now;

    if(down)
    {
        if(HitTest(px, py)) id = 1;
    }

    if(down && !g_wasDown && id)
    {
        /* Cualquier toque sobre nuestra UI reinicia el fundido. */
        g_idleAt = now;
        LaunchSelector();
    }

    g_press = id;
    g_wasDown = down;
}

/* Corre en el hilo de proceso del juego cada frame, asi que se mantiene corto y
   sin asignaciones por frame: todo el estado son escalares estaticos. */
static void OnTick(void*)
{
    uint64_t now;
    if(!g_menuKit)
        return;

    now = NowMs();

    ProbeScreen();
    if(g_screenW <= 0.0f) return;   /* aun no se sabe cuanto mide la pantalla */

    /* El menu del juego manda: con un menu abierto no se procesa entrada ni se
       pinta nada, para no dejar un boton encima de su UI. HandleInput lleva la
       misma puerta por su cuenta, para que siga siendo correcta aunque se
       llame desde otro sitio. */
    if(g_menuKit->GetMenuUp && g_menuKit->GetMenuUp() == 1)
    {
        g_wasDown = 0;
        g_press   = false;
        g_tickPrev = now;
        return;
    }

    LayoutLauncher();
    HandleInput(now);
    DrawLauncher(now);
    DrawHint(now);
}

ON_MOD_LOAD()
{
    logger->SetTag("SelectorLauncher");
    logger->Info("SA Selector Launcher loading");

    /* Resolve el cargador de scripts del juego por nombre (independiente de la
       version del binario). */
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

    /* Resuelve la cabeza de la lista de scripts vivos. Sin esto el mod puede
       arrancar el script pero no puede saber si sigue en marcha, y se
       apuntaria una copia duplicada en cada pulsacion. */
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

    /* MenuKit va compilado dentro de esta libreria; se usa el punto de entrada
       embebido en vez de buscar el mod MenuKit aparte. */
    g_menuKit = MenuKit_InitEmbedded(aml);
    if(!g_menuKit)
    {
        logger->Error("MenuKit embedded initialization failed");
        return;
    }

    /* Sonda de tamano de pantalla: cuadrada, centrada y APARCADA (SetVisible 0 =
       ni se dibuja ni se toca). OnTick la lee una vez y la devuelve al pool.
       No hay ningun widget visible: la UI entera es lienzo. */
    g_probe = g_menuKit->AddButton(0, "_shoot", 320.0f, 224.0f, 20.0f, NULL, NULL, NULL);
    if(g_probe)
    {
        g_menuKit->SetSize(g_probe, 100.0f, 100.0f);
        g_menuKit->SetVisible(g_probe, 0);
        g_probeAt = NowMs();
    }
    else
    {
        logger->Error("SelectorLauncher: no free widget slots for the screen probe");
    }

    g_menuKit->SetTick(OnTick, NULL);
}

ON_MOD_UNLOAD()
{
    MenuKit_Shutdown();
}
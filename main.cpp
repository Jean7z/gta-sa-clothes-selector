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
/* Pestaña "uñita": pegada al BORDE SUPERIOR, centrada en horizontal, colgando
   hacia dentro como un marcador. El borde superior en y = 0 sin margen, para que
   se lea como parte de la pantalla y no como un boton puesto encima.

   Proporciones sale del lado corto medido y se acotan, para que en un movil
   pequeno siga siendo pulsable y en una tablet no quede enorme. */
#define TAB_W_K       0.11f    /* ancho = min(screenW,screenH) * esto */
#define TAB_W_MIN    44.0f     /* ancho minimo */
#define TAB_W_MAX    72.0f     /* ancho maximo */
#define TAB_H_K       0.5f     /* alto = ancho * esto */
#define TAB_H_MIN    20.0f     /* alto minimo */
#define TAB_H_MAX    34.0f     /* alto maximo */

/* Radio de las esquinas INFERIORES. Las superiores van rectas a proposito: se
   pierden contra el borde de la pantalla, que es justo lo que se busca. */
#define TAB_CORNER_K  0.45f    /* radio = alto * esto */

/* Glifo: la percha ocupa esto del alto de la pestaña. */
#define TAB_GLYPH_K   0.45f

/* Holgura del golpeo por DEBAJO de la pestaña. Una pestaña diminuta es un blanco
   pequeño, y un dedo no apunta con precisión: el area sensible se extiende hacia
   abajo sin salirse del ancho dibujado. */
#define TAB_TOUCH_EXTEND 30.0f

/* El estado de "en vehiculo" (ambar) se queda sin texto que lo explique: es lo
   que se pidio. Sigue viendose por el color de la percha y del borde inferior. */

/* Sin fundido: la pestaña esta SIEMPRE a opacidad 1.0. No hay IconFade, ni
   reloj de inactividad, ni piso de opacidad: el usuario pidio dejarla siempre
   encendida. */

/* Tope de espera de la sonda antes de asumir un tamano. */
#define PROBE_TIMEOUT 1500u

/* El lanzador se esconde mientras el script corre: el menu del selector ya trae
   su propio boton de regreso y tener los dos es redundante. Como eso deja al
   boton invisible justo cuando no debe pulsar, hace falta red de seguridad.

   Desatasco: si el script sigue en la lista mucho tiempo, no hay menu abierto y
   el puntero lleva rato sin actividad, esta clarisimo que se ha quedado
   colgado en la lista (un cierre de menu que no lo termina). Se le devuelve el
   boton al usuario. NO salta en los primeros segundos tras lanzar, que es
   cuando el arranque es lento yaria por las ramas. Es red, no el camino normal:
   el cierre de verdad es el boton de regreso del propio script. */
#define ZOMBIE_MS    20000.0f   /* cuanto lleva "corriendo" antes de sospechar */
#define ZOMBIE_IDLE   5000.0f   /* y ademas, puntero sin actividad este rato */

/* Colores en 0xRRGGBBAA (rojo en el byte alto, ALFA en el bajo). MenuKit
   desempaqueta asi en Canvas_Push, y es el mismo convenio para DrawCircle,
   DrawLine y DrawText. */
#define L_BODY       0x0B0F1A66u  /* cuerpo: negro azulado translucido, alfa 0x66 */

/* Los dos colores de estado son los MISMOS que los del texto de abajo, para que
   el anillo insinue lo mismo que dice la frase. Con el boton escondido mientras
   el script corre no hace falta un tercero para ese caso. */
#define C_READY      0x9BE36BFFu  /* a pie, sin lanzar */
#define C_VEHICLE    0xF5A623FFu  /* conduciendo: el script no puede correr */
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

/* Geometria de la pestana del frame actual (la escribe LayoutTab). El hit-test
   usa las MISMAS variables que el dibujo, para que el toque y lo que se ve no
   se separen en ningun tamano de pantalla. */
static float               g_tabX = 0.0f, g_tabW = 0.0f, g_tabH = 0.0f;

/* Instante del ultimo arranque con exito. Lo usa el desatasco: no se sospecha de
   un script que acaba de arrancar, porque el arranque es lento. 0 = nunca se ha
   lanzado nada, y entonces el desatasco no puede dispararse. */
static uint64_t            g_runSince = 0;

/* Instante del ultimo toque en CUALQUIER parte, acierte o no. Tambien lo usa el
   desatasco, para no declarar zombi a un script con el dedo encima. */
static uint64_t            g_pokeAt  = 0;

/* El lanzador estaba visible en el frame anterior. Al volver a verse se reinicia
   el reloj del fundido, para que reaparezca con su opacidad completa y no a media
   tinta. */
static bool                g_wasVisible = false;

/* El desatasco ya ha soltado su aviso por este arranque, para no escribir en el
   log una vez por frame. */
static bool                g_zombieLogged = false;

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

    /* Se apunta cuando arranco para que el desatasco no lo declare zombi durante
       el arranque lento, y se rearma el aviso por si habia saltado antes. */
    g_runSince     = NowMs();
    g_zombieLogged = false;

    logger->Info("Selector script started (CTheScripts::StartNewScript, %u bytes, base set)", SELECTOR_SCRIPT_LEN);
}

/* Desatasco. El lanzador se esconde mientras el script corre, y si ese script se
   quedase clavado en la lista de CTheScripts el boton se quedaria escondido para
   siempre: el usuario perderia la funcion hasta reiniciar el juego.

   Se declara zombi SOLO si se cumple las tres cosas:
     - que lleve mucho rato en la lista (y no acabado de arrancar),
     - que no haya ningun menu del juego abierto (el selector ya se cerro),
     - y que el puntero lleve un rato sin actividad.
   El cierre de verdad es el boton de regreso del propio script; esto es solo la
   red por si ese boton no lo termina. */
static bool SelectorIsStuck(uint64_t now)
{
    if(!IsSelectorRunning()) return false;
    if(g_runSince == 0)       return false;                    /* nunca lanzado */
    if(now - g_runSince <= (uint64_t)ZOMBIE_MS) return false;  /* arranque lento */
    if(g_menuKit->GetMenuUp && g_menuKit->GetMenuUp() != 0) return false;
    if(g_pokeAt != 0 && now - g_pokeAt <= (uint64_t)ZOMBIE_IDLE) return false;

    if(!g_zombieLogged)
    {
        logger->Info("Selector script still listed after %.0f s with no menu and no touch; "
                     "treating as stuck and showing the launcher again",
                     (double)(now - g_runSince) / 1000.0);
        g_zombieLogged = true;
    }
    return true;
}

/* Hay que pintar el lanzador? Mientras el selector corre no: su menu ya trae
   boton de regreso y dos lanzadores a la vez sobran. */
static bool SelectorVisible(uint64_t now)
{
    return !IsSelectorRunning() || SelectorIsStuck(now);
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

/* Tamano de la pestaña: lado corto de la pantalla, acotado para que ni se pierda
   en un movil pequeno ni quede enorme en una tablet. */
static void TabSize(void)
{
    const float side = g_screenW < g_screenH ? g_screenW : g_screenH;
    g_tabW = side * TAB_W_K;
    if(g_tabW < TAB_W_MIN) g_tabW = TAB_W_MIN;
    if(g_tabW > TAB_W_MAX) g_tabW = TAB_W_MAX;

    g_tabH = g_tabW * TAB_H_K;
    if(g_tabH < TAB_H_MIN) g_tabH = TAB_H_MIN;
    if(g_tabH > TAB_H_MAX) g_tabH = TAB_H_MAX;

    g_tabX = g_screenW * 0.5f;
}

/* Pestaña del borde superior, centrada. Se recalcula cada frame con el tamano
   medido por la sonda, nunca con un valor fijo. */
static void LayoutTab(void)
{
    TabSize();
}

/* Golpeo: MISMO ancho y MISMA X que el dibujo, desde y = 0 hasta el alto de la
   pestaña mas una holgura generosa POR DEBAJO. Una pestaña diminuta es un blanco
   pequeño y un dedo no apunta con precisión, asi que el area sensible baja mas
   de lo que se ve, sin salirse del ancho dibujado.

   El hit-test no mira el alfa: con el icono atenuado se toca igual. */
static bool HitTest(float px, float py)
{
    const float halfW = g_tabW * 0.5f;
    return px >= g_tabX - halfW && px <= g_tabX + halfW &&
           py >= 0.0f && py <= g_tabH + TAB_TOUCH_EXTEND;
}

/* Color de estado. Son los mismos dos que usan las frases de abajo, para que el
   anillo insinue lo mismo que dice el texto.

   No hay color "corriendo" porque con el script en marcha el lanzador esta
   escondido y no se dibuja nada. */
static unsigned int StateColor(void)
{
    if(IsInVehicle()) return C_VEHICLE;
    return C_READY;
}

/* Cuarto de disco relleno, como en el music-player: el abanico del lienzo
   triangula desde el primer vertice, asi que basta con poner el CENTRO del arco
   delante y luego los puntos del arco. Llena el cuarto EXACTO y no se sale hacia
   dentro, que es lo que evita que las piezas translucidas se compongan dos
   veces. */
#define ARC_SEGS 8

static void FillQuarterDisc(float cx, float cy, float r, float a0, unsigned int c)
{
    float xy[2 * (ARC_SEGS + 2)];
    int i, n = 0;

    xy[0] = cx; xy[1] = cy; n = 1;                       /* centro: ancla del fan */
    for(i = 0; i <= ARC_SEGS; ++i)
    {
        const float a = a0 + (float)i * (3.14159265f * 0.5f / (float)ARC_SEGS);
        xy[n * 2]     = cx + r * cosf(a);
        xy[n * 2 + 1] = cy + r * sinf(a);
        ++n;
    }
    g_menuKit->DrawPoly(xy, n, c, 1, 1);
}

/* Percha: glifo de ropa, no de audio, porque esto abre un armario.

   El gancho es un punto RELLENO, no un arco. Dos motivos:
     - este canvas no dibuja arcos: DrawCircle(filled=0) traza los 360 grados,
       y el cierre se veia como un pacman dentro del icono. Un arco de verdad
       habria que hacerlo con DrawPoly abierto, y a este tamano (radio ~3-4 px)
       se lee como ruido, no como gancho.
     - un circulo relleno es convexo y no deja costura, mientras que una
       polilinea abierta fina a 4 px se rompe al cambiar de escala.
   La percha se reconoce por el triangulo (cuestas + barra) y el punto le da el
   remate de arriba; con eso basta. */
static void DrawHangerGlyph(float cx, float cy, float r, unsigned int c)
{
    const float barY  = cy + r * 0.46f;
    const float halfW = r * 0.82f;
    const float peakY = cy - r * 0.32f;

    g_menuKit->DrawCircle(cx, cy - r * 0.82f, r * 0.24f, c, 1, 10);   /* gancho */
    g_menuKit->DrawLine(cx, cy - r * 0.58f, cx, peakY, c, 1.0f);      /* viga */
    g_menuKit->DrawLine(cx, peakY, cx - halfW, barY, c, 1.0f);        /* cuestas */
    g_menuKit->DrawLine(cx, peakY, cx + halfW, barY, c, 1.0f);
    g_menuKit->DrawLine(cx - halfW, barY, cx + halfW, barY, c, 1.0f);  /* barra */
}

/* Pestaña del borde superior: rectangulo con las esquinas de ABAJO redondeadas y
   las de arriba rectas, que se pierden contra el borde.

   Las piezas NO se solapan, igual que en FillRoundRect del music-player. Antes se
   usaban circulos ENTEROS para el redondeo y se comian parte del rectangulo y
   del relleno central; con un cuerpo translucido eso compone el alfa dos veces y
   salen manchas oscuras. Ahora el redondeo son dos cuartos de disco, que se
   quedan dentro de su cuadrado de esquina y justos contra el rectangulo.

   Sin escalado al pulsar: pegada al borde no hay referencia para encogerla, y el
   escalado de un disco flotando no tiene sentido aqui. El toque solo aclara el
   color de la percha. */
static void DrawTab(void)
{
    const float cx = g_tabX;
    const float w  = g_tabW;
    const float h  = g_tabH;
    const float r  = h * TAB_CORNER_K;
    const float PI_2 = 3.14159265f * 0.5f;
    const float x0 = cx - w * 0.5f;
    const float x1 = cx + w * 0.5f;
    const unsigned int st = StateColor();

    /* Cuerpo: rectangulo hasta h - r y los dos cuartos inferiores. Alpha 1.0
       siempre, sin fundido: la pestaña esta siempre encendida. */
    g_menuKit->DrawRect(x0, 0.0f, w, h - r, L_BODY, 1);
    FillQuarterDisc(x0 + r, h - r, r, PI_2,       L_BODY);   /* esquina inf izq */
    FillQuarterDisc(x1 - r, h - r, r, 0.0f,       L_BODY);   /* esquina inf der */
    g_menuKit->DrawRect(x0 + r, h - r, w - r * 2.0f, r, L_BODY, 1);

    /* Una sola capa tenue bajo la pestaña, del color de estado. El halo de tres
       aros del disco no se traslada aqui: en algo tan pequeño solo quedaría
       sucio. Con esta linea basta para insinuar el estado sin texto. */
    g_menuKit->DrawRect(x0 + r * 0.5f, h, w - r, 1.5f,
                        (st & 0xFFFFFF00u) | 0x80u, 1);

    /* Percha dentro, al TAB_GLYPH_K del alto. El triangulo ya se lee y el punto
       de arriba le da la forma que la identifica como armario. */
    DrawHangerGlyph(cx, h * 0.5f, h * TAB_GLYPH_K, g_press ? L_PRESS : st);
}

/* ------------------------------------------------------------------ */
/* Entrada y tick                                                      */
/* ------------------------------------------------------------------ */

/* Entrada y accion.

   El hit-test NO mira el alfa: el icono atenuado se toca igual que el brillante.
   Un toque abre siempre, este o no apagado, porque pedir dos golpes para lo
   mismo es engorroso. El apagado es estetica, no una puerta. Y como el piso es
   0.38 y no 0, nunca hay un boton invisible al que no se pueda llegar.

   Lo que SI mira es la visibilidad: con el script en marcha el lanzador esta
   escondido y no se hace hit-test, que si no se volveria a pulsar un boton que
   no esta en pantalla. */
static void HandleInput(uint64_t now, bool visible)
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

    if(down && !g_wasDown)
    {
        /* Actividad real del puntero, acierte o no. El desatasco lo mira para no
           declarar zombi a un script con el dedo encima. */
        g_pokeAt = now;
    }

    /* Con el lanzador escondido no hay nada que tocar: se salta el hit-test,
       pero el puntero se sigue leyendo arriba para el desatasco. */
    if(down && visible)
    {
        if(HitTest(px, py)) id = 1;
    }

    if(down && !g_wasDown && id)
    {
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
    bool visible;
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

    /* Con el selector corriendo el lanzador se esconde: su menu ya trae boton de
       regreso. El desatasco puede devolverlo si el script se queda colgado. */
    visible = SelectorVisible(now);

    LayoutTab();
    HandleInput(now, visible);

    /* Al volver a verse se suelta el resaltado: si no, se quedaria con el
       glifo aclarado de un toque anterior. Sin fundido ya no hay nada mas que
       rearmar. */
    if(visible && !g_wasVisible)
        g_press = false;
    g_wasVisible = visible;

    if(visible)
        DrawTab();
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
#pragma once
// ---------------------------------------------------------------------------
// ayther_session.h — AytherSession, the motor's control facade (R2).
//
// AytherSession is the single, coherent surface the frontends drive instead of
// wiring the ~9 raw ayther_core handles by hand. It owns the whole deterministic
// pipeline — emulator host + tile/sprite/audio hashing + substitution + Lua
// scripting + HD audio output — behind one object, and exposes the *result* of
// each frame as a plain-data FrameView for the frontend to render.
//
// Motor / frontend boundary (see docs/architecture/ayther-engine.md §7):
//
//   AytherSession (motor, this object)        Frontend (ayther_play / ayther_lab)
//   ----------------------------------        -----------------------------------
//   run_frame() the libretro core             window + SDL events + input source
//   hash tiles/sprites/audio                   upload FrameView.fb -> VkTexture
//   fire Lua on_frame, resolve overrides       draw tile/sprite substitutions
//   resolve substitutions                      CRT post-process / present
//   play + mute + flush HD audio   <-- audio   Lab authoring UI / timeline
//   produce a FrameView  ----------------->    consume FrameView, render it
//
// Audio output lives entirely inside the session (the AudioPlayer is owned by
// the motor in R1): play/mute/flush happen in step(), nothing audio crosses the
// boundary. The frontend never touches Vulkan-from-the-motor or SDL-audio.
//
// No-throw: construction and fallible operations return ayther::Result (§4.1.1);
// nothing throws across the FFI users. Opaque handles are held as
// ayther::unique_handle inside the pimpl — no raw Rust pointer lives loose (§4.1).
//
// Threading: a session is single-owner and must be driven from one thread (the
// emulation thread), matching ayther_core's rule. Non-copyable; movable.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio_asset_level.h"    // AudioAssetLevel (: nivel medido de un asset)
#include "audio_match_rule.h"     // AudioMatchRule ( F3: reglas de match)
#include "ayther_animation.h"     // AnimationPlayer / AnimHdFrame / HdPose (C-S2)
#include "ayther_audio_events.h"  // AudioEventSubstitution / AudioEventAssignment (C-A2)
#include "ayther_core_ffi.h"   // AytherTileSub, AytherSpriteSub, *Occurrence (POD)
#include "ayther_layers.h"     // AytherLayerContent (: Acetatos del pack)
#include "ayther_mode3.h"      // Mode3Resolver / EntityInstance (Modo 3, RAM anchoring)
#include "ayther_result.h"     // ayther::Result / Error

namespace ayther {

struct AytherRecording;   // ayther_recording.h — deterministic .arp take (R7)

// Un tile de un plano del VDP (A/B/Window) presente este frame — deduplicado por
// contenido del patrón (no por celda). Identidad por hash del patrón (+ paleta),
// estable entre frames/sesiones (content-based, como sprites/tiles). Lo computa
// AytherSession leyendo la nametable + VRAM (Fase 2 del panel Capas). `cell_x/y`
// es una celda REPRESENTATIVA (la primera) — para datos/preview, no posición de
// pantalla (los planos scrollean).
struct PlaneTileOccurrence {
    uint64_t hash    = 0;
    uint16_t cell_x  = 0;   ///< celda de nametable representativa
    uint16_t cell_y  = 0;
    uint16_t pattern = 0;   ///< índice de patrón (0..2047) para decodificar
    uint8_t  plane   = 0;   ///< 0 = Plano A · 1 = Plano B · 2 = Window
    uint8_t  palette = 0;   ///< 0..3 (CRAM)
    uint8_t  hflip   = 0;
    uint8_t  vflip   = 0;
};

/// RE-PALETADO de un hash de tile de plano: devuelve el hash que tendría EL
/// MISMO patrón bajo otra línea de CRAM. Sin tocar VRAM y sin que la variante
/// esté en pantalla.
///
/// Se puede porque el hash es `FNV1a(32 bytes del patrón)` y la paleta se
/// mezcla AL FINAL — `h = (h_patrón ^ pal) * PRIME` — con un PRIME impar, o
/// sea invertible mod 2^64. Deshacer la última vuelta, cambiar la línea y
/// rehacerla es aritmética exacta, no una aproximación. (Ver el cálculo del
/// hash en ayther_session.cpp, `collect_plane_tiles`.)
///
/// Es lo que hace barato clonar un Carácter o un Juego de caracteres a otro
/// color: el clon recibe los hashes de la variante y, como el id del elemento
/// los mezcla, su identidad sale distinta sola.
///
/// Oráculo (paint_repalette_smoke): en cualquier frame, para todo par de
/// PlaneTileOccurrence con el MISMO `pattern` y distinta `palette`, tiene que
/// valer `repalette(a.hash, a.palette, b.palette) == b.hash`.
inline uint64_t ayther_plane_tile_hash_repalette(uint64_t h, uint8_t from,
                                                 uint8_t to) {
    constexpr uint64_t kPrime = 1099511628211ULL;
    // Inverso de kPrime mod 2^64 por Newton-Raphson (6 pasos bastan para 64
    // bits: cada iteración duplica los bits correctos).
    uint64_t inv = 1;
    for (int i = 0; i < 6; ++i) inv *= 2ULL - kPrime * inv;
    return ((h * inv) ^ (uint64_t)(from & 3u) ^ (uint64_t)(to & 3u)) * kPrime;
}

/// Las CUATRO lecturas de un hash de celda observado bajo la línea `pal`: la
/// misma celda tal como se habría hasheado bajo cada línea de CRAM. `out[0]` es
/// el hash tal cual, para que quien busque pueda probar el camino directo
/// primero y no pagar nada en el caso normal.
///
/// Sirve para comparar contra índices que guardan hashes SIN su paleta —
/// `PanoramaCell` es hash + posición, y ese es también el formato del pack— y
/// que si no se despegan cuando el juego reasigna la celda a otra línea (los
/// ciclos de día/noche que REPINTAN en vez de cambiar el contenido de la
/// línea). Las cuatro son exactas, no aproximaciones: ver `..._repalette`.
///
/// Oráculo: `plane_hash_variants` (round-trip, cobertura de las 4 líneas y
/// ausencia de colisión entre patrones distintos).
inline void ayther_plane_tile_hash_variants(uint64_t h, uint8_t pal,
                                            uint64_t out[4]) {
    out[0] = h;
    unsigned n = 1;
    for (uint8_t to = 0; to < 4; ++to)
        if (to != (pal & 3u))
            out[n++] = ayther_plane_tile_hash_repalette(h, pal, to);
}

// Una celda VISIBLE de un plano este frame, con su posición de PANTALLA ya
// resuelta scroll-aware (mismo inverso que el resolver HD de Fase 2c). A
// diferencia de PlaneTileOccurrence (deduplicada por contenido), acá hay UNA
// entrada por aparición en pantalla → el Lab sincroniza el viewport con el panel
// Capas: dibuja el recuadro de un tile de plano seleccionado y hace picking del
// fondo bajo el cursor. Vacío sin core con VRAM+VSRAM.
struct PlaneCellHit {
    uint64_t hash     = 0;   ///< identidad de contenido (== PlaneTileOccurrence.hash)
    int16_t  screen_x = 0;   ///< px de la esquina sup-izq del tile 8×8
    int16_t  screen_y = 0;
    uint8_t  plane    = 0;   ///< 0 = Plano A · 1 = Plano B · 2 = Window
    /// R-3 (): la FUENTE de esta celda (el word del nametable ya la trae) —
    /// sin esto el inventario de escena tendría que re-derivar patrón/paleta
    /// del hash, que es flip-invariante y por lo tanto ambiguo.
    uint16_t pattern  = 0;   ///< índice de patrón VRAM 0..2047
    uint8_t  palette  = 0;   ///< línea CRAM 0..3
    /// Flips/prioridad DE ESTA CELDA (del word del nametable, Fase C): bit0 =
    /// hflip · bit1 = vflip · bit2 = prioridad VDP. A diferencia de la
    /// occurrence (representativa por contenido), acá son los reales por
    /// aparición — la captura de Pintar los usa para el export fiel.
    /// R-3 (): bit3 = celda PARCIAL de borde (screen x/y negativos por
    /// scroll no alineado a 8). Las parciales van al FINAL de plane_cells,
    /// después de las celdas que participan de la firma del Cuadro — ese
    /// prefijo no cambió y las firmas autoradas siguen valiendo.
    uint8_t  flags    = 0;
};

// R-3 (): un elemento DIBUJABLE del frame en la lista única de la escena —
// celda de plano o sprite — con posición, fuente, capa, prioridad e identidad
// estable. El ORDEN en la lista es el orden de dibujo global (back→front):
// B pri0 · A pri0 · Window pri0 · sprites pri0 · ídem prioridad alta; los
// sprites dentro de su grupo van del fondo al frente (cadena SAT invertida).
// OJO (semántica VDP que la lista NO aplana): entre sprites gana el primero de
// la CADENA sin importar prioridad, y ese pixel único compite con los planos a
// SU prioridad — un compositor exacto pasa los sprites por un buffer
// primero-gana y mezcla después. R-5: FrameView la publica (scene) cuando el
// scene compose está ON — vive a nivel de namespace por eso.
struct SceneElement {
    uint64_t hash     = 0;    ///< identidad estable (hash flip-invariante); 0 = sin hash
    int16_t  x = 0, y = 0;    ///< px de pantalla (esquina sup-izq)
    uint8_t  w = 8, h = 8;    ///< tamaño en px
    uint16_t pattern  = 0;    ///< patrón VRAM (celda) / tile base (sprite)
    uint8_t  palette  = 0;    ///< línea CRAM 0..3
    uint8_t  flips    = 0;    ///< bit0 hflip · bit1 vflip
    uint8_t  layer    = 0;    ///< 0=Plano B · 1=Plano A · 2=Window · 3=Sprite
    uint8_t  priority = 0;    ///< bit de prioridad del VDP
    uint8_t  slot     = 0xFF; ///< sprite: slot SAT 0-79; planos: 0xFF
    /// Sprite: posición en la CADENA de links al parsear (menor = más al
    /// frente). Es la prioridad real entre sprites y es GLOBAL (cruza los
    /// grupos de prioridad). Planos: 0xFF.
    uint8_t  chain    = 0xFF;
    uint8_t  sub_kind = 0;    ///< fuente HD: 0=VRAM pura · 1=fv.sprite_subs · 2=fv.plane_tile_subs
    uint8_t  hidden   = 0;    ///< R-4: 1 = oculto (set_hidden_elements o el canal del Lab)
    /// R-5 (): 1 = un asset HD ya resuelto REEMPLAZA este elemento (sub
    /// directo, miembro reclamado de una pose, o celda consumida por un set)
    /// → el compose por elementos no dibuja el original con HD activo. Es lo
    /// que reemplaza a los canales de supresión: lo que no gana no se emite.
    uint8_t  claimed  = 0;
    /// R-6 (): EFECTOS del elemento (set_element_effects, por capa+hash) —
    /// con el pipeline indexado son un uniform por quad, no una lane. Tinte
    /// multiplicativo Q2.6 por canal (64 = neutro, mismo formato que el E1 de
    /// los sprites HD; >64 = más brillante), opacidad 0-255 (255 = opaco), y
    /// silueta de autoría (color AABBGGRR detrás del elemento; 0 = sin).
    uint8_t  fx_tint[3] = { 64, 64, 64 };
    uint8_t  fx_opacity = 255;
    uint32_t fx_outline = 0;
    ///  (runtime_enhancement): 1 = el elemento se dibuja con el escalador
    /// EPX sobre índices de paleta («Mejorar por software»). Se publica sólo
    /// si el elemento NO está reclamado por HD (el asset ganó). La identidad
    /// es (capa, hash), como hidden/fx: otras apariciones del mismo gráfico
    /// también se mejoran (semántica documentada, no se «arregla»).
    uint8_t  fx_enhance = 0;
    /// : intensidad del suavizado 0..255 (solo significa algo con
    /// fx_enhance = 1). 255 = vector limpio · 0 = pixel art apenas redondeado.
    uint8_t  fx_enhance_k = 255;
    int32_t  sub      = -1;   ///< índice en el array de subs correspondiente (-1 = ninguno)
};

// R-4 (): identidad de un elemento OCULTO del inventario — (capa, hash),
// no hash solo: el mismo gráfico puede existir como sprite Y como tile de
// plano (mismo hash flip-invariante en ambos dominios — lo cazó
// element_hidden_smoke), y ocultar el sprite no debe ocultar las celdas.
struct HiddenElement {
    uint64_t hash  = 0;
    uint8_t  layer = 3;   ///< SceneElement.layer: 0=B · 1=A · 2=W · 3=Sprite
};

//  (runtime_enhancement): identidad de un elemento a MEJORAR por software
// — misma identidad (capa, hash) que el ocultado y los efectos. La sesión une
// dos fuentes: la lista viva del Lab (set_enhanced_elements) y la del pack
// ([[enhance]] de elements.toml, load_pack_into). El inventario la publica en
// SceneElement.fx_enhance y el compose indexado la aplica como un bit por quad.
struct EnhancedElement {
    uint64_t hash  = 0;
    uint8_t  layer = 3;   ///< SceneElement.layer: 0=B · 1=A · 2=W · 3=Sprite
    uint8_t  k     = 255; ///< : intensidad 0..255
};

// R-6 (): efecto asignado a un elemento del inventario — misma identidad
// (capa, hash) que el ocultado. Los campos son los de SceneElement.fx_*: el
// inventario los resuelve por elemento y el compose los aplica como uniform
// por quad (el punto de la épica: agregar un efecto es shader, no arquitectura).
struct ElementEffect {
    uint64_t hash    = 0;
    uint8_t  layer   = 3;              ///< 0=B · 1=A · 2=W · 3=Sprite
    uint8_t  tint[3] = { 64, 64, 64 }; ///< Q2.6 por canal (64 = neutro)
    uint8_t  opacity = 255;            ///< 0-255 (255 = opaco)
    uint32_t outline = 0;              ///< silueta AABBGGRR (0 = sin)
};

// ---------------------------------------------------------------------------
// FrameView — the motor -> frontend data boundary for exactly one stepped frame.
//
// Lifetime: every pointer below is owned by the session and valid only until the
// next call that mutates session state — step(), reset(), unserialize(),
// set_pack() or reload_pack(). The frontend must copy anything it needs to keep
// (e.g. a thumbnail) before the next step().
// ---------------------------------------------------------------------------
struct FrameView {
    // -- Emulator framebuffer: the base image to upload + present --------------
    // fb_pixels is null when the core duplicated the previous frame (no redraw);
    // the frontend then keeps last frame's texture.
    const void* fb_pixels   = nullptr;
    uint32_t    fb_width    = 0;
    uint32_t    fb_height   = 0;
    uint32_t    fb_pitch    = 0;       ///< bytes per row
    int         fb_format   = 0;       ///< RETRO_PIXEL_FORMAT_* (frontend maps to VkFormat)

    // -- Resolved tile substitutions: HD asset to draw per on-screen tile ------
    const AytherTileSub*   tile_subs        = nullptr;
    uint32_t               tile_sub_count   = 0;

    // -- Resolved sprite substitutions: HD alpha-blended overlay sprites -------
    const AytherSpriteSub* sprite_subs      = nullptr;
    uint32_t               sprite_sub_count = 0;
    const uint8_t*         sprite_sub_flips = nullptr;  ///< CU-AN-11: paralelo a sprite_subs
                                                        ///< (bit0 hflip, bit1 vflip) → auto-espejo
    /// E1 CROMÁTICO (fundido + cambio de paleta): tinte RGB por sub, 3 bytes
    /// por entrada (stride 3, paralelo a sprite_subs), punto fijo Q2.6
    /// (64 = 1.0, máx ~3.98). El renderer multiplica el color del HD por este
    /// tinte para que siga los fades Y los flashes/cambios de color del juego
    /// (un flash naranja tinta el HD, no solo lo oscurece; >64 = más brillante
    /// que lo normal → satura hacia el flash). Referencia por sub: la autorada
    /// (`ref_rgb` de la pose) o, sin ella, el peak-hold escalar por paleta
    /// (comportamiento E1 clásico, tinte gris).
    const uint8_t*         sprite_sub_tint  = nullptr;
    /// C8 (z-order entre HD superpuestos): slot SAT 0-79 por sub, paralelo a
    /// sprite_subs (el menor slot de las occs que solapan el sub = el miembro más
    /// al frente). El renderer dibuja los subs por slot DESCENDENTE (frontmost
    /// último) para respetar la oclusión sprite-vs-sprite. 255 = sin occ que
    /// solape (al fondo).
    const uint8_t*         sprite_sub_slot  = nullptr;
    /// Bit de PRIORIDAD VDP por sub (0/1), paralelo a sprite_subs — el del occ
    /// exacto o el del frontmost del bbox. El hardware ordena sprite pri-1 >
    /// plano A pri-1 (las letras del título de GA orbitan el isologotipo
    /// conmutando el bit): el renderer dibuja los subs pri-1 DELANTE del
    /// Primer plano HD, y los pri-0 detrás, POR FRAME.
    const uint8_t*         sprite_sub_prio  = nullptr;

    // -- Resolved PLANE-tile substitutions (Fase 2c): HD overlay de un tile de
    //    fondo, resuelto scroll-aware a posiciones de pantalla. Se reusa el struct
    //    de sprites (quad 1×1 con alpha) y el renderer los dibuja DEBAJO de los
    //    sprites reales. Vacío sin asignaciones / sin core con VSRAM.
    const AytherSpriteSub* plane_tile_subs      = nullptr;
    uint32_t               plane_tile_sub_count = 0;
    const uint8_t*         plane_tile_flips     = nullptr;  ///< paralelo: bit0 hflip, bit1 vflip
    uint32_t               plane_tile_sub_hi    = 0;        ///< [0,hi)=prio baja (bajo sprites); [hi,count)=alta (sobre)
    /// Tinte E1 por sub de plano (stride 3, Q2.6 — mismo contrato que
    /// sprite_sub_tint). Hoy sólo los quads de SET con referencia autorada
    /// (`ref` del elemento) salen de 64/64/64: live/ref POR CANAL de la línea
    /// CRAM del ancla, así el Objeto sigue los fundidos de paleta del juego
    /// (el isologotipo del título de GA aparecía a todo color sobre la CRAM
    /// en negro del pre-fade, reporte 2026-08-19). Sin referencia = neutro
    /// (byte-exacto con lo previo).
    const uint8_t*         plane_tile_sub_tint  = nullptr;

    // -- Modo 3 (RAM anchoring): sustitución HD POR INSTANCIA de entidad -------
    //    Un AytherSpriteSub por instancia anclada cuyo kind tiene asset asignado
    //    (assign_kind), en el bbox agregado de sus sprites SAT — desambigua
    //    entidades idénticas por world_pos leído de la RAM (perfil de juego).
    //    El renderer los dibuja en la lane de sprites. `entity_instances` lista
    //    TODA instancia localizada (con o sin asset) para el overlay de autoría
    //    del Lab (cajita + id). Vacío sin perfil / sin VSRAM (core stock).
    const AytherSpriteSub* entity_subs           = nullptr;
    uint32_t               entity_sub_count      = 0;
    const EntityInstance*  entity_instances      = nullptr;
    uint32_t               entity_instance_count = 0;

    // -- Animaciones C-S2: frames HD en fase --------------------------------
    //    Por cada occurrence en pantalla cuyo clip (anim_group_id) tiene una
    //    animación HD definida y cuya pose (hash) está en el sheet: el frame
    //    del sheet (sub-rect UV) en el bbox observado (Pop) o en el transform
    //    tweeneado (Nivel 1). El renderer los dibuja con VkSprite::draw_anim.
    const AnimHdFrame* anim_frames      = nullptr;
    uint32_t           anim_frame_count = 0;

    // -- Per-frame occurrences + catalog telemetry (authoring / Lab / overlay) -
    const AytherTileOccurrence*   tile_occs        = nullptr;  uint32_t tile_occ_count   = 0;
    const AytherSpriteOccurrence* sprite_occs      = nullptr;  uint32_t sprite_occ_count = 0;
    const AytherAudioOccurrence*  audio_occs       = nullptr;  uint32_t audio_occ_count  = 0;
    uint32_t unique_tile_count   = 0;
    uint32_t unique_sprite_count = 0;
    uint32_t unique_audio_count  = 0;

    // -- Raw sound-chip bus writes this frame (FM YM2612 + PSG SN76489), in
    //    temporal order — the basis of command-based audio events (workspace
    //    Audios). Replay-stable as a command sequence (the cycle timestamp is
    //    not — see AytherAudioWrite). Empty with a stock core (no id 0x109).
    //    Valid until the next step(); the motor owns the backing buffer.
    const AytherAudioWrite* chip_writes      = nullptr;
    uint32_t                chip_write_count = 0;

    // -- Sustitución de audio por evento (C-A3b): máscara de canales muteados este
    //    frame por eventos ASIGNADOS activos, y la lista de subs activos (para que
    //    el playback dispare el asset HD en sync). Sólo con el preview activo
    //    (set_audio_substitution_preview) sobre la toma analizada; 0 / vacío si no.
    uint32_t audio_mute_mask = 0;
    const AytherAudioActiveSub* audio_active_subs      = nullptr;
    uint32_t                    audio_active_sub_count = 0;

    // -- Cobertura de planos A/B/Window: celdas de la nametable con tile != 0.
    //    Para las lanes Plano A / Plano B / Window del timeline de Editar.
    //    Derivadas de los VDP regs (0x101) + VRAM; 0 si el core no las expone.
    uint32_t plane_a_count = 0;
    uint32_t plane_b_count = 0;
    uint32_t plane_w_count = 0;   ///< Window (HUD/marcador estático), reg $3

    // -- Elementos por tile de cada plano (Fase 2, panel Capas) — deduplicados
    //    por contenido del patrón. Para listar/nombrar/previsualizar los tiles de
    //    Plano A/B/Window. Vacío si el core no expone VRAM/regs.
    const PlaneTileOccurrence* plane_tile_occs      = nullptr;
    uint32_t                   plane_tile_occ_count = 0;

    // -- Celdas VISIBLES de los planos con su posición de pantalla (Fase 2c) —
    //    una por aparición (no deduplicada). El Lab las usa para el recuadro de
    //    selección y el picking del fondo (sync viewport↔Capas). Vacío sin VSRAM.
    const PlaneCellHit*        plane_cells      = nullptr;
    uint32_t                   plane_cell_count = 0;

    // -- Scroll whole-plane de este frame (px del VDP, 0..1023) — el insumo del
    //    BackgroundStitcher (Caso B/C): da la posición de la cámara en espacio de
    //    nivel para acumular tiles a lo largo de una toma. H muestreado en la línea
    //    0; V global (col 0). Window no scrollea → 0. 0 si el core no expone VSRAM.
    // -- R-5 (): la ESCENA por elementos — el insumo del render propio ----
    //    Publicada sólo con scene compose ON (set_scene_compose / env
    //    AYTHER_SCENE_COMPOSE): la lista única ordenada de R-3 + el estado
    //    crudo que el pipeline indexado necesita (VRAM/CRAM del fork, vista
    //    word-swapped del host) + el backdrop. Con esto el renderer COMPONE la
    //    escena y el frame del emulador deja de ser el lienzo (queda como
    //    fuente de verdad de qué hay, y para los hashers). Vacío con el
    //    compose apagado o sin core forkeado.
    const SceneElement* scene       = nullptr;
    uint32_t            scene_count = 0;
    const uint8_t* scene_vram = nullptr;  size_t scene_vram_size = 0;
    const uint8_t* scene_cram = nullptr;  size_t scene_cram_size = 0;
    uint16_t scene_backdrop   = 0;   ///< color de fondo (CRAM empaquetada, reg7&0x3F)
    uint8_t  scene_left_blank = 0;   ///< reg 0 bit 5: 8 px izq. al backdrop
    /// R-5: el frame NO se compone fiel desde la escena → el renderer cae al
    /// blit del emulador (el híbrido de R-1). bit0 = escrituras con efecto
    /// visual a mitad de pantalla (señal 0x10E del fork); bit1 = dim de
    /// Animación activo (efecto del produce sobre el fb, no modelado aún);
    /// bit2 = hscroll por línea/celda con variación real (cizalla sub-tile no
    /// modelada hasta que el pipeline dibuje strips — R-7).
    uint8_t  scene_dirty      = 0;

    int16_t plane_hscroll[3] = {0, 0, 0};   ///< [0]=A [1]=B [2]=Window
    int16_t plane_vscroll[3] = {0, 0, 0};
    uint16_t plane_wpx = 0;   ///< ancho del plano A/B en px (el scroll wrappea mod esto)
    uint16_t plane_hpx = 0;   ///< alto del plano A/B en px
    /// : vscroll POR COLUMNA (modo VS del VDP, reg 11 bit 2). Las nubes
    /// del título de GA scrollean en 16 tiras de 16 px con fase propia — el
    /// vscroll global de arriba es sólo la columna 0. [0]=A · [1]=B, columna
    /// s cubre la pantalla en x∈[s·16, s·16+16). Con vs_two_cell=false las
    /// columnas replican el global igual (el consumidor puede no chequear el
    /// flag), pero el flag es la señal de que el juego DE VERDAD scrollea por
    /// columna (la lane Custom ancla por tira sólo entonces).
    bool    vs_two_cell = false;
    int16_t plane_vscroll_col[2][20] = {};

    // -- Cámara en espacio de NIVEL (EM-1): scroll des-wrapeado ACUMULADO por
    //    plano ([0]=A · [1]=B; Window es fija). Tracking SECUENCIAL: válido
    //    mientras los frames avanzan de a 1 (juego en vivo / playback lineal);
    //    una discontinuidad (seek/scrub/catch-up) re-ancla la cámara en 0 y el
    //    flag cae hasta el próximo frame secuencial. El nivel_x de una celda =
    //    plane_cam_x + screen_x — CONSTANTE bajo scroll (la identidad de fondo
    //    source-side que pide EM-1; insumo del widescreen EM-8).
    int32_t plane_cam_x[2] = {0, 0};
    int32_t plane_cam_y[2] = {0, 0};
    bool    plane_cam_valid = false;

    // -- Firma de PANTALLA por plano (Cuadro · CU001) -------------------------
    //    Identidad de la pantalla que forman las capas de fondo, para
    //    reconocer una pantalla estática (título, menú, pantalla legal) y
    //    reemplazarla entera por un asset HD. Los sprites NO participan: esto
    //    sale del pick-list de planos.
    //
    //    Se acumula por PLANO ([0]=A · [1]=B · [2]=Window) con una combinación
    //    CONMUTATIVA, así que la firma de cualquier máscara de capas es la SUMA
    //    de sus planos — un Cuadro de "A+B" es `sig[0]+sig[1]`, sin recorrer
    //    las celdas de nuevo (ver `screen_signature`).
    //
    //    El término por celda usa la posición en CELDAS (screen_x >> 3), lo que
    //    la hace invariante a la fase sub-celda del scroll: un título que
    //    tiembla unos px no cambia la firma. Si la pantalla scrollea una celda
    //    entera la firma cambia — pero eso ya no es un Cuadro, es una
    //    Panorámica.
    //
    //    Vale 0 sin el core forkeado (sin VSRAM no hay pick-list de planos).
    uint64_t screen_plane_sig[3]   = {0, 0, 0};
    uint32_t screen_plane_cells[3] = {0, 0, 0};

    //    Cuadro vigente este frame (0 = ninguno) + con cuánta cobertura
    //    matcheó, para que la UI de autoría muestre el margen: un score al filo
    //    del umbral avisa que dos pantallas se están pisando.
    uint64_t screen_match_id    = 0;
    float    screen_match_score = 0.0f;
    float    screen_match_extra = 0.0f;
    ///  mecanismo 2: Cuadros cuyo CONTENIDO está presente este frame
    /// (>=60% de sus patrones distintos, por capa declarada, en cualquier
    /// posición). Invariante al scroll y al movimiento de los elementos —
    /// lo que el gate de un Acetato necesita durante una intro animada.
    uint64_t screen_presence_ids[8] = {};
    uint32_t screen_presence_count  = 0;
    /// El asset del Cuadro vigente, a pantalla completa (0 ó 1 entradas).
    const AytherSpriteSub* screen_subs = nullptr;
    uint32_t               screen_sub_count = 0;

    // -- CINEMÁTICA (CU004): la secuencia ordenada en curso -------------------
    //    `screen_subs` es COMPARTIDO: cuando hay Cinemática vigente y su paso
    //    trae asset propio, ese quad es el que sale por ahí — no se emiten los
    //    dos. Un quad full-screen opaco encima de otro deja el resultado a
    //    merced del orden de lane, que es lo que la escalera evita.
    uint64_t kinematic_id    = 0;   ///< cinemática en curso (0 = ninguna)
    uint32_t kinematic_step  = 0;   ///< paso vigente dentro de la secuencia
    uint32_t kinematic_steps = 0;   ///< largo de la secuencia (0 = ninguna)

    // -- VIDEO del paso () ------------------------------------------------
    //    Cuando el asset del paso es un `.ivf`, NO sale por `screen_subs`: sale
    //    por acá, en píxeles ya decodificados, y `screen_sub_count` queda en 0.
    //
    //    El motivo del camino separado es duro: `AytherSpriteSub` no tiene campo
    //    de tiempo (agregárselo rompe el ABI del FFI) y su `asset_path` es la
    //    CLAVE del cache de texturas de VkSprite, donde una textura entra a la
    //    lista de liberación diferida del staging () y a los pocos frames
    //    deja de aceptar uploads EN SILENCIO. Un video enrutado por ahí quedaría
    //    congelado en su primer frame sin ningún error. Ver ayther_video.h.
    //
    //    La sesión produce píxeles, el
    //    renderer los sube. Así el decoder queda del lado sin Vulkan y se puede
    //    verificar headless.
    /// : los TRES PLANOS I420 del decoder, sin copia (null = sin video). La
    /// conversión a RGB la hace el fragment shader; antes acá venía un BGRA8
    /// convertido en CPU, que era el 62% del costo por frame.
    /// Los strides NO son el ancho: libvpx alinea las filas.
    const void* video_y = nullptr;
    const void* video_u = nullptr;
    const void* video_v = nullptr;
    uint32_t    video_y_stride = 0, video_u_stride = 0, video_v_stride = 0;
    uint32_t    video_w = 0, video_h = 0;
    uint32_t    video_frame = 0;         ///< índice decodificado dentro del clip
    /// Cambia SÓLO si el contenido cambió. El renderer se saltea la re-subida
    /// cuando no cambió: sin esto se paga el memcpy completo por cada frame de
    /// interfaz estando en pausa.
    uint64_t    video_seq = 0;

    // -- PANORÁMICA (CU003): cámara ANCLADA POR CONTENIDO ---------------------
    //    Posición de la cámara en PÍXELES de nivel, votada por las celdas
    //    visibles contra la tira declarada. A diferencia de `plane_cam_*` (que
    //    es relativa y se re-ancla en cada seek), esta es ABSOLUTA respecto de
    //    la tira y sobrevive a un scrub, a una carga de savestate y a un corte
    //    de escena — es lo que permite dibujar la textura HD en el lugar
    //    correcto mientras el artista scrubbea.
    //
    //    `panorama_votes` / `panorama_cells` dan la confianza: pocos votos
    //    sobre muchas celdas = la cámara no se pudo fijar (una tira de puro
    //    cielo repetido no tiene hashes raros que anclen).
    int32_t  panorama_cam_x = 0, panorama_cam_y = 0;
    /// : máscara de planos del VIDEO de la Cinemática (bit0=A · bit1=B ·
    /// bit2=Window) — la UNIÓN de las máscaras de sus Cuadros. Con escena
    /// compuesta el video se dibuja INLINE en el pase del plano más alto de
    /// esta máscara, y así el Window y los Sprites VIVOS del juego componen
    /// encima en vez de quedar tapados. 0 = sin máscara conocida ⇒ el
    /// comportamiento de siempre (pantalla completa).
    uint8_t  video_plane_mask = 0;

    ///  decisión 3: 1 = el video va en la posición del FRENTE (el z de
    /// VdpFrente, sobre los pri-1 y sobre todo lo HD) en vez del z de su plano.
    ///
    /// Sale del DATO del VDP, no de una casilla que el autor tenga que
    /// acordarse de marcar: si las celdas vivas de los planos que el video
    /// cubre son mayoritariamente pri-1, el Cuadro es un primer plano —el
    /// flash a pantalla completa de una cinemática— y taparlo con el video en
    /// el z del fondo lo dejaría detrás de lo que justamente quiere cubrir.
    ///
    /// Sólo significa algo con `video_plane_mask != 0`: sin máscara el video ya
    /// va a pantalla completa por la lane global, que es la degradación vieja.
    uint8_t  video_front = 0;

    ///  fase 0: ANCHO LÓGICO del ensanchado, en píxeles del emulador.
    /// 0 = sin ensanchar (el frame nativo ocupa todo el canvas, que es lo de
    /// siempre). Con valor, el frame nativo se CENTRA y a los lados queda el
    /// área extendida: 398 px en píxel cuadrado sobre 224, 427 preservando el
    /// 4:3 mostrado — los dos los calcula `widescreen_target_width()`.
    ///
    /// El arte de los lados NO sale de la nametable: leerla de más devuelve
    /// arte de otro tramo del nivel (la nametable envuelve cada 512 px). Sale
    /// de la LÁMINA de la Panorámica, que es lo que cada posición mostró
    /// cuando estuvo en pantalla — por eso ensanchar es, en la práctica, dejar
    /// que la tira se dibuje más allá del borde.
    uint32_t wide_w = 0;
    uint64_t panorama_id    = 0;      ///< la Panorámica anclada (0 = ninguna)
    uint32_t panorama_votes = 0;      ///< celdas visibles que votaron por el ganador
    uint32_t panorama_cells = 0;      ///< celdas visibles que podían votar
    ///  EM-8.1: qué fracción (0-100) de las celdas visibles del plano
    /// EXPLICA la tira en la posición anclada. `panorama_valid` es el sí/no; el
    /// porcentaje importa porque el área EXTENDIDA le exige más que la nativa —
    /// la nativa se corrige sola con las celdas vivas que la tira no reclamó, y
    /// la extendida no tiene con qué corregirse. Por debajo del piso la tira se
    /// recorta al ancho nativo y los lados quedan vacíos.
    uint32_t panorama_cover = 0;
    /// : qué fracción (0-100) de las posiciones de la tira ANCLADA tiene un
    /// solo hash. Se publica porque sin este número el síntoma —cobertura 100 %
    /// con la lámina mostrando otro tramo del nivel— se lee como un defecto del
    /// ensanchado, que es donde se perdió una tarde. Es DIAGNÓSTICO y no un
    /// gate: medido, ninguna de las dos tiras del corpus llega a 1 % de
    /// limpieza (Golden Axe incluida, que extiende BIEN), así que la
    /// ambigüedad por sí sola no separa el caso bueno del malo — ver .
    uint32_t panorama_clean = 0;
    bool     panorama_valid = false;
    /// Los quads de la tira HD para este frame: un TRAMO por corrida horizontal
    /// de celdas contiguas que son la Panorámica, con su sub-rect UV dentro de
    /// la tira. No es un solo quad a pantalla completa a propósito — ver la nota
    /// larga en la emisión (ayther_session.cpp): el recorte por celda ES la
    /// máscara, y por eso un primer plano del OTRO plano no se borra.
    const AytherSpriteSub* panorama_subs = nullptr;
    uint32_t               panorama_sub_count = 0;
    /// Plano de la tira ANCLADA este frame (0=A · 1=B): el renderer dibuja los
    /// quads INLINE en el pase de ese plano (z correcto — la tira reemplaza
    /// celdas de SU capa, no tapa lo que va delante).
    uint8_t                panorama_plane = 0;
    /// Tinte Q2.6 del quad (stride 3, mismo canal E1 que los sprites): cociente
    /// POR CANAL de la CRAM viva contra la referencia de la definición, con las
    /// líneas ponderadas por lo que aportaban al definir. Sigue los fundidos y
    /// también los CAMBIOS DE COLOR — un atardecer que vira a naranja tiñe la
    /// tira, no sólo la oscurece — y puede aclarar (>64), para amaneceres y
    /// flashes. Con la referencia sin señal cromática cae al escalar por luma
    /// (comportamiento ). null sin tira.
    const uint8_t*         panorama_sub_tint = nullptr;

    /// Firma de la máscara de capas `plane_mask` (bit0=A · bit1=B · bit2=Window)
    /// y, opcionalmente, cuántas celdas la componen.
    uint64_t screen_signature(uint8_t plane_mask,
                              uint32_t* out_cells = nullptr) const noexcept {
        uint64_t s = 0; uint32_t n = 0;
        for (int p = 0; p < 3; ++p)
            if (plane_mask & (1u << p)) { s += screen_plane_sig[p]; n += screen_plane_cells[p]; }
        if (out_cells) *out_cells = n;
        return s;
    }


    // -- CRT / post-process params the Lua script produced this frame ----------
    AytherShaderParams shader_params{};

    // -- Timing: the motor measures its own per-stage cost ---------------------
    double   emu_fps    = 0.0;
    float    tile_ms    = 0.0f;
    float    sprite_ms  = 0.0f;
    float    audio_ms   = 0.0f;
    float    drc_ratio  = 1.0f;   ///< audio dynamic-rate-control ratio (1.0 = neutral)
    uint64_t frame_index = 0;
    /// : fps de TIMING del core (fijo por región, ~59.92/49.7) — la base
    /// del término temporal de los Acetatos (`frame_index * drift / fps`).
    /// NO confundir con `emu_fps`, que es el throughput MEDIDO del runner y
    /// cambia con la máquina: usarlo rompería el determinismo del replay.
    double   fps_timing = 0.0;
};

// ---------------------------------------------------------------------------
// Subsistemas sustituibles ( / )
// ---------------------------------------------------------------------------
//
// La lista de lo que AYTHER puede reemplazar, y la unidad con la que se
// enciende y se apaga. El ORDEN es un contrato: el core tiene la misma lista en
// `ayther_subsystem_name()` —es la que viaja en el `[systems]` del manifest— y
// un test compara las dos nombre por nombre. Dos listas paralelas sin nada que
// las ate se desincronizan en silencio, y ahí alguien apaga «música» y se le va
// la interfaz.
enum class Subsystem : uint8_t {
    Sprites = 0,   ///< sprites sueltos por hash
    Metasprites,   ///< poses / conjuntos de sprites (CU-AN)
    Tiles,         ///< tiles de plano por hash
    Planes,        ///< planos completos (fondos, tiras)
    Ui,            ///< interfaz del juego (Caracteres, Juegos de caracteres)
    Music,         ///< audio del bus de Música
    Sfx,           ///< audio del bus de Efectos
    Shaders,       ///< efectos de salida (CRT/LCD, )
    Count
};
constexpr uint32_t kSubsystemCount = static_cast<uint32_t>(Subsystem::Count);
constexpr uint32_t subsystem_bit(Subsystem s) {
    return 1u << static_cast<uint32_t>(s);
}

// ---------------------------------------------------------------------------
// Buses lógicos de audio ()
// ---------------------------------------------------------------------------
//
// La categoría de un sonido NO se deduce: el motor identifica timbres, y nada
// en el chip dice si algo es música, un efecto o una voz. Llega AUTORADA — el
// «Tipo» de cada Secuencia en Mezclar — o no llega.
//
// `Unclassified` no es un default perezoso sino un estado real: una Secuencia
// vieja no dijo nada. Lo que sí es una decisión de producto es dónde cae lo que
// NO tiene Secuencia (una asignación por firma suelta, que hoy es el camino más
// usado): va a **Efectos**, que es lo que un sonido suelto es hasta que alguien
// diga lo contrario. Ver `bus_of_signature`.
enum class AudioBus : uint8_t {
    Unclassified = 0,
    Music,
    Sfx,
    Voice,
    Count
};
constexpr uint32_t kAudioBusCount = static_cast<uint32_t>(AudioBus::Count);

/// Describes whether the active pack provides a subsystem.
///
/// Three states preserve the distinction between an explicit absence and
/// missing metadata. A frontend can therefore avoid presenting an unknown
/// capability as either supported or unavailable.
enum class SubsystemAvailability : uint8_t {
    Unknown = 0,  ///< No pack is active, or the pack does not declare this subsystem.
    Present,      ///< The active pack explicitly provides this subsystem.
    Absent,       ///< The active pack explicitly declares this subsystem absent.
};

// ---------------------------------------------------------------------------
// AytherSession
// ---------------------------------------------------------------------------
/// @brief Single-owner facade for deterministic engine orchestration.
///
/// A session owns the emulator host, Rust core handles, optional pack state,
/// scripting, rewind, recording, and audio runtime. Create instances through
/// create(); construction is transactional and returns a typed error.
///
/// @par Thread safety
/// Not thread-safe. Drive the complete lifecycle and every mutating operation
/// from one owning thread.
///
/// @par Borrowed results
/// Views returned by frame-producing operations remain valid only until the
/// next operation that advances, resets, rewinds, reloads, or destroys the
/// session. Copy any data that must cross that boundary.
class AytherSession {
public:
    // -- Configuration passed to create() --------------------------------------
    struct Config {
        std::string core_path;             ///< libretro core DLL (the emulator)
        std::string rom_path;              ///< the ROM — BYOR (Bring Your Own ROM)
        std::string pack_path;             ///< optional .ay HD pack ("" = none)
        bool        enable_audio = true;   ///< open the SDL audio device + AudioPlayer
        /// When pack_path is empty, derive "<core stem>.ay" next to the core
        /// (player convention). The Lab disables this: a project session must
        /// not pick up stray dev packs sitting next to the core.
        bool        derive_core_pack = true;
        /// Libretro core options as `(key, value)` pairs. They are applied once,
        /// before the core reads its configuration during initialization.
        ///
        /// Changing an option requires a new session. A live setter would imply
        /// behavior the underlying core cannot provide safely.
        ///
        /// Available keys are core-specific. Discover them through
        /// core_options_declared() instead of maintaining a frontend list.
        std::vector<std::pair<std::string, std::string>> core_options;
        /// Optional user-supplied IPS/BPS patch. The patch is applied to the ROM
        /// buffer in memory and never modifies the source file. Empty means no
        /// patch.
        ///
        /// Patch failure aborts session creation; silently starting unpatched
        /// would violate the caller's requested configuration.
        std::string patch_path;
    };

    // -- Lifecycle (no-throw) --------------------------------------------------
    // Opens the core + ROM, creates every motor handle (runner, hashers,
    // substitutors, script, optional pack + audio) wrapped in unique_handle.
    // On failure returns an Error (bad core/ROM/pack) — never throws.
    static Result<std::unique_ptr<AytherSession>> create(const Config& cfg);

    ~AytherSession();                                  ///< RAII frees every handle
    AytherSession(const AytherSession&)            = delete;
    AytherSession& operator=(const AytherSession&) = delete;
    AytherSession(AytherSession&&) noexcept;
    AytherSession& operator=(AytherSession&&) noexcept;

    /// Returns the options declared by the loaded core as `(key,
    /// "Description; a|b|c")` pairs. An empty result means the core declared
    /// none; it does not establish a universal lack of option support.
    std::vector<std::pair<std::string, std::string>> core_options_declared() const;

    // -- Content: the HD pack (hot-reloadable) ---------------------------------
    Result<void> set_pack(const std::string& pack_path);  ///< load / replace the pack
    Result<void> reload_pack();                           ///< re-open the current path (hot-reload)
    bool         has_pack() const noexcept;

    // -- Input: per port, libretro JOYPAD button bitmask -----------------------
    void set_input(int port, uint16_t buttons) noexcept;

    // -- Frame stepping --------------------------------------------------------
    // Advance exactly one emulation frame and run the full deterministic
    // pipeline: run_frame -> hash tiles/sprites/audio -> Lua on_frame -> apply
    // overrides -> resolve substitutions -> HD audio out. Returns this frame's
    // data boundary (see FrameView lifetime note).
    const FrameView& step();

    // -- Determinism: savestate round-trip (the foundation of .arp recordings) -
    size_t       serialize_size() const;
    Result<void> serialize(std::vector<uint8_t>& out) const;
    Result<void> unserialize(const std::vector<uint8_t>& in);
    void         reset();

    // -- Rewind + fast-forward (R6) --------------------------------------------
    // enable_rewind allocates a zstd-compressed ring of `seconds` of savestates
    // (captured each step() while enabled; zero cost when off). rewind_step()
    // walks the emulation back one frame (false when the buffer can't go back).
    void   enable_rewind(bool on, int seconds = 10);
    // Rewind one frame: restores the previous state and re-renders it, returning
    // that frame's view (null when the buffer can't go back — disabled / empty).
    const FrameView* rewind_step();
    bool   rewind_enabled()      const noexcept;
    size_t rewind_frames()       const noexcept;   ///< states currently buffered
    size_t rewind_memory_bytes() const noexcept;   ///< compressed buffer size
    // Fast-forward multiplier (frontend reads speed() and steps N times/frame).
    void   set_speed(float mult) noexcept;
    float  speed() const noexcept;
    /// Sets the global device-output mute. This is a no-op when the session was
    /// created without audio. The setting is session-local.
    void   set_audio_muted(bool m) noexcept;
    bool   audio_muted() const noexcept;
    /// Replaces the persistent set of individually muted audio identities.
    /// Passing an empty set restores all identities. This policy is independent
    /// of global output mute and replacement-audio suppression.
    void   set_audio_mute_hashes(const uint64_t* hashes, uint32_t n) noexcept;

    /// Vista previa "en contexto" de un audio del juego (panel Capas): ubica por
    /// el `.arp` (v7) el primer frame donde sonó ese `hash` grabado, re-simula un
    /// tramo corto y captura la MEZCLA de ese momento, reproduciéndola one-shot —
    /// SIN mover el playhead (serializa/restaura estado + cursor) y SIN tocar el
    /// mute. NO aísla el sonido: el audio del replay no es byte-reproducible (la
    /// fase del FM diverge tras el save/load), así que filtrar por hash no sirve;
    /// se reproduce la mezcla real de ese frame (como al scrubbear ahí). La captura
    /// no necesita device (testeable headless); sólo suena si la sesión abrió audio.
    /// Devuelve los cuadros estéreo capturados (0 = el hash no aparece / sin v7).
    size_t preview_audio(const AytherRecording& rec, uint64_t hash);
    /// Vista previa de audio EN UN FRAME dado (el playhead): captura la MEZCLA de
    /// ese momento y la reproduce one-shot, SIN mover el playhead. A diferencia de
    /// `preview_audio(rec, hash)`, NO busca por hash — el frontend ya está parado en
    /// el frame del sonido (panel Capas) y los hashes de audio del REPLAY no
    /// coinciden con los grabados (la fase del FM diverge tras el load), así que
    /// localizar por hash grabado no funciona desde la UI. `frame` se acota a la
    /// toma. Devuelve los cuadros estéreo capturados (0 = frame inválido / sin v7).
    size_t preview_audio_at(const AytherRecording& rec, uint32_t frame);
    /// Preview AUDIBLE del audio ORIGINAL de un span [start, end] (one-shot) — el play
    /// del timeline de Secuencia: re-simula el span y reproduce su mezcla. Aislar:
    /// `member_sigs` (firmas de la Secuencia) = POR EVENTO (dinámico: cada frame solo
    /// suenan los canales con un evento miembro activo — un golpe ajeno que comparte
    /// canal ya no se cuela); `solo_mask` = fallback por canal; 0/null = mezcla
    /// completa. Tope 60 s. No mueve el playhead. Sólo suena con device abierto.
    /// `foreign_rec` (2026-08-22): la rec NO es la toma analizada (el ▶ original
    /// del header de Secuencia desde OTRA toma). Fuerza la re-sim de ESA rec con
    /// aislamiento POR CANAL — el espejo y el aislamiento por evento salen del
    /// análisis de la toma cargada y mentirían sobre una ajena.
    void   preview_audio_span(const AytherRecording& rec, uint32_t start, uint32_t end,
                              uint32_t solo_mask = 0,
                              const std::vector<uint64_t>* member_sigs = nullptr,
                              bool foreign_rec = false);
    /// Estado del TRANSPORTE (la app lo setea por frame): los HD asignados
    /// (sustitución por evento/Secuencia) solo DISPARAN reproduciendo — al
    /// scrubear en pausa se marca sin sonar. La transición a pausa CORTA el
    /// gameplay ENTERO (): los HD en el aire, el staging del frame y el
    /// PCM original/router/SF2 ya encolado en los streams continuos (el
    /// colchón DRC seguía sonando tras el botón). Los previews explícitos de
    /// autoría no se cortan. Un seek no-secuencial (scrub) también corta los
    /// HD. Historia: sin el corte, el one-shot a velocidad normal se
    /// superponía (eco) y seguía sonando con el cabezal detenido (2026-07-23).
    void   set_transport_playing(bool playing) noexcept;
    /// Salida de audio AUDIBLE (la app lo setea por frame): con false, el
    /// produce DESCARTA el PCM del emulador en vez de mandarlo al device y no
    /// dispara subs/HD — los produce INTERNOS de cargar una toma/pose (seeks,
    /// invalidates, regeneraciones) "chillaban" un frame de audio (reporte
    /// 2026-07-24). La app lo enciende solo REPRODUCIENDO o con el usuario
    /// SCRUBEANDO un timeline. Default true (runtime/Play no lo tocan);
    /// independiente del mute global (device gain) y de replay_quiet.
    void   set_audio_audible(bool on) noexcept;
    /// Audio del EXPORT MP4: escribe a WAV S16/44.1k la mezcla de [start, end).
    /// `hd=false`: mezcla ORIGINAL pura del chip (versión Original del video).
    /// `hd=true`: original con los MUTES dinámicos del playback aplicados por
    /// frame (Secuencias sustituidas + asignaciones per-firma + ocurrencias
    /// deshabilitadas + manual) y los assets HD vigentes (audio_seq_subs +
    /// audio_event_assign) MEZCLADOS encima en cada ancla con su gain — «como
    /// suena en Mezclar» (el gate audio_sub_preview se fuerza durante la
    /// captura). Requiere analyze_audio_events previo para hd. El corte por
    /// ausencia (~1 s) NO se replica: es heurística del detector EN VIVO; el
    /// modelo replay/pack deja sonar la ventana completa. Tope duro ~15 min.
    /// No mueve el playhead. Bloqueante (re-simula el rango una vez).
    bool export_mixdown_wav(const AytherRecording& rec, uint32_t start,
                            uint32_t end, const char* wav_path, bool hd);
    /// Exporta a WAV (S16 estéreo 44100) el audio del emulador de la ventana
    /// [start_frame, end_frame] (+ `tail` frames de cola para SFX que decaen; 0 =
    /// exacto al span) — la REFERENCIA de tiempos para que el artista haga el HD
    /// (handoff, workspace Audios C-A4). Re-sim sin mover el playhead; el audio del
    /// replay no es byte-exacto pero sirve de guía. Aislamiento igual que
    /// preview_audio_span: `member_sigs` = POR EVENTO (Secuencias — solo suenan
    /// los eventos miembro aunque otro sonido comparta canal); `solo_mask` =
    /// fallback por canal; 0/null = mezcla completa. Devuelve true si escribió
    /// el archivo.
    bool   export_audio_event_wav(const AytherRecording& rec, uint32_t start_frame,
                                  uint32_t end_frame, const char* wav_path,
                                  uint32_t tail = 20, uint32_t solo_mask = 0,
                                  const std::vector<uint64_t>* member_sigs = nullptr);
    /// Mezclar (diálogo Detalle de canal): re-simula [start, start+win) con SOLO
    /// los canales de `solo_mask` audibles (bits 0-5 FM · 6-9 PSG; el resto se
    /// mutea con el mute por canal del fork durante la ventana) y copia el PCM
    /// S16 estéreo 44100 capturado a `out`. Devuelve los cuadros estéreo (0 =
    /// inválido). No mueve el playhead ni suena; tope ~10 s del capture.
    size_t capture_channel_pcm(const AytherRecording& rec, uint32_t start,
                               uint32_t win, uint32_t solo_mask,
                               std::vector<int16_t>& out);
    /// Como capture_channel_pcm pero aislando por EVENTO y no por canal: cada
    /// frame deja sonar sólo los canales donde una de las `member_sigs` está
    /// activa. Copia el PCM S16 estéreo 44100 a `out`; devuelve los cuadros.
    ///
    /// Existe para el A/B de la Biblioteca de SoundFonts (): comparar un
    /// preset contra «el original» exige que el original sean LAS MISMAS NOTAS.
    /// Aislar por canal traía además todo lo que comparte ese canal —en el Mega
    /// Drive, la música entera—, así que un timbre esparcido se comparaba contra
    /// un pasaje completo y parecía que el preset no sonaba.
    size_t capture_events_pcm(const AytherRecording& rec, uint32_t start,
                              uint32_t win,
                              const std::vector<uint64_t>& member_sigs,
                              std::vector<int16_t>& out);
    /// Exporta a WAV el audio de [start, end] (+ `tail` frames de cola) con SOLO
    /// los canales de `solo_mask` audibles — el sample por sonido del paquete DAW
    /// (Mezclar). Re-sim sin mover el playhead; tope 600 frames (~10 s). Devuelve
    /// true si escribió el archivo.
    bool   export_channel_wav(const AytherRecording& rec, uint32_t start,
                              uint32_t end, uint32_t solo_mask, uint32_t tail,
                              const char* wav_path);
    /// Reproduce PCM S16 estéreo 44100 crudo (one-shot) — el diálogo Detalle de
    /// canal reproduce el PCM ya capturado sin volver a re-simular. No-op sin
    /// device de audio abierto.
    void   play_pcm(const int16_t* pcm, size_t stereo_frames);
    void   stop_audio_preview();
    /// True mientras la vista previa de audio sigue sonando (el botón del diálogo
    /// conmuta Reproducir/Detener con esto y vuelve a Reproducir al terminar).
    bool   audio_preview_playing() const;

    // -- Eventos de audio por comandos de chip (C-A2, workspace Audios) ---------
    // Analiza una toma reproduciéndola frame a frame (replay_seek secuencial =
    // produce cada frame) y alimentando el AudioEventDetector con el log de
    // escrituras FM/PSG (FrameView.chip_writes). Produce bloques de actividad por
    // canal con FIRMA estable (mismo SFX = misma firma), replay-deterministas (no
    // dependen del PCM). Recording-céntrico como el resto del Lab. El análisis es
    // silencioso (no suena) y deja el cursor de replay al final de la toma — el
    // caller re-busca su playhead después. Requiere un core con el log instrumentado
    // (id 0x109); con core stock devuelve 0. Devuelve la cantidad de eventos.
    uint32_t analyze_audio_events(const AytherRecording& rec);
    /// Eventos del último analyze_audio_events (válidos hasta el próximo análisis
    /// o clear). Punteros estables mientras no se reanalice.
    const AytherAudioEvent* audio_events() const noexcept;
    uint32_t                audio_event_count() const noexcept;
    void                    clear_audio_events() noexcept;

    // -- Sustitución de audio por evento (C-A3b) -------------------------------
    // Asigna un asset HD a una FIRMA de evento: TODOS los eventos con esa firma se
    // sustituyen — sus canales se mutean durante su ventana [start,end] y el
    // playback dispara el asset (FrameView.audio_active_subs). El mute lo aplica el
    // motor (id 0x10D, replay-safe). Sólo surte efecto con el PREVIEW activo,
    // reproduciendo la toma que se analizó con analyze_audio_events. La lógica de
    // *qué* canal mutear es determinista y verificable headless; el disparo del
    // audio HD suelto (decodificar OGG/FLAC) lo hace la capa de playback del Lab.
    void     assign_audio_event(uint64_t signature, const char* asset_path);
    void     unassign_audio_event(uint64_t signature);
    void     clear_audio_event_assignments() noexcept;
    uint32_t audio_event_assignment_count() const noexcept;
    /// Asset HD asignado a una firma de evento ("" si no hay). Para que el panel
    /// muestre la asignación actual del evento seleccionado.
    std::string audio_event_asset(uint64_t signature) const;
    /// Una sustitución de audio por evento: firma → asset + canales involucrados.
    /// duration/looping ≠ 0 = entrada de SECUENCIA (Mezclar): ventana relativa
    /// al trigger con range-mute + HD (ver audio_events.toml).
    struct AudioEventSub {
        uint64_t    signature;
        std::string asset;
        uint32_t    channels;
        uint32_t    duration_frames = 0;
        bool        looping         = false;
        /// : frames que el HD puede seguir tras end_frame (0 = corte
        /// exacto). UINT32_MAX = ilimitado/no autorado (legacy: el non-loop
        /// drena entero) — el writer del pack solo lo escribe si es finito.
        uint32_t    tail_frames     = UINT32_MAX;
        /// : cuadros de DESVANECIMIENTO tras end_frame (0 = sin fade;
        /// manda la política de ). Alternativa a `tail`, no acumulable.
        uint32_t    fade_frames     = 0;
        ///  F3: regla de match + identidad del timbre (el writer del pack
        /// solo las escribe con regla ≠ exacta — legacy byte-idéntico).
        AudioMatchRule match_rule       = AudioMatchRule::kExact;
        uint64_t       match_instrument = 0;
        uint8_t        match_pitch      = kAudioNoPitch;
    };
    /// Lista de las asignaciones (firma, asset, canales) — para la entrega .ay.
    /// Los canales salen de los eventos analizados o, si no, del mapa cargado.
    std::vector<AudioEventSub> audio_event_subs() const;
    ///  F3: regla de MATCH de una asignación per-firma — opt-in, persiste
    /// en audio_events.toml (`match`). kExact borra la regla (vuelve al legacy).
    /// La identidad del timbre se toma de los eventos analizados o de lo
    /// aprendido por el detector live; sin ella la regla no puede armarse (y
    /// kInstrumentPitch además exige nota) → false y no cambia nada. Alcanza a
    /// TODOS los caminos que hoy matchean por firma exacta: live, replay,
    /// máscaras bare y export. (Las Secuencias de autoría llevan su regla en
    /// la propia AudioSeqSub — set_audio_sequence_subs.)
    bool set_audio_event_match_rule(uint64_t signature, AudioMatchRule rule);
    /// Regla vigente de una asignación + identidad persistida (kExact si no hay).
    AudioMatchRule audio_event_match_rule(uint64_t signature,
                                          uint64_t* instrument = nullptr,
                                          uint8_t* pitch = nullptr) const noexcept;
    ///  F3: identidad del TIMBRE de una firma — de los eventos analizados
    /// (Mezclar) o de lo aprendido por el detector live (Capturar). false =
    /// aún desconocida (reproducir el pasaje / analizar la toma primero).
    /// Con esto el Lab arma reglas para Secuencias (cuyo disparador no es una
    /// asignación per-firma).
    bool audio_signature_identity(uint64_t signature, uint64_t* instrument,
                                  uint8_t* pitch) const noexcept;
    /// Activa/desactiva la sustitución por evento durante el playback (el Lab lo
    /// enciende en el workspace Audios). Apagado por defecto → reproduce normal.
    void     set_audio_substitution_preview(bool on) noexcept;

    // -- Sustitución de audio EN VIVO (runtime, C-A4 paso 3) -------------------
    // Activa la sustitución por evento en tiempo real (Ayther Play / juego en
    // vivo, NO replay): cada frame alimenta un detector con las escrituras del
    // chip, y los canales cuya firma está asignada se mutean + disparan su HD. Hay
    // 1 frame de lag inherente (las escrituras existen recién tras run_frame). Las
    // asignaciones vienen del audio_events.toml del pack (se cargan con set_pack).
    void     set_audio_runtime_substitution(bool on) noexcept;
    ///  Fase 3: BYPASS de Assets en un workspace vivo — el detector y el
    /// bookkeeping (ventanas/instancias/flancos) siguen corriendo pero la
    /// máscara cae a 0 (suena el juego original) y ningún HD dispara. Volver
    /// de un bypass re-entra por el mismo camino que reanudar una pausa: el
    /// offset del reloj emulado, sin esperar un key-on nuevo. Distinto de
    /// set_audio_runtime_substitution(false), que es el cierre EXPLÍCITO
    /// (cambio de workspace) y tira todas las instancias.
    void     set_audio_live_bypass(bool bypass) noexcept;
    /// Canales de audio key-on AHORA (sólo con runtime sub activo). Para tooling /
    /// una futura visualización en vivo. Devuelve la cantidad escrita en `out`.
    uint32_t audio_live_active(AytherAudioActive* out, uint32_t cap) const;
    /// Mute por canal MANUAL (timeline del workspace Audios): máscara u16 (bits 0-5
    /// FM, 6-9 PSG) que se aplica SIEMPRE en el produce (OR con el mute de
    /// sustitución). Para auditar canales sueltos. 0 = nada muteado.
    void     set_audio_manual_mute(uint32_t mask) noexcept;
    /// Mute DINÁMICO por INSTRUMENTO (panel Sonidos, Mezclar): a diferencia de
    /// set_audio_manual_mute (por canal, incondicional TODO el frame), este mute
    /// sólo aplica en los frames donde CAE un evento de audio_events (analyze_
    /// audio_events) cuyo `instrument` está en el set — así un instrumento que
    /// rota de canal no mutea sonidos AJENOS que usan ese canal en otros tramos.
    /// SILENCIAR ES SILENCIAR (): el mute alcanza también a lo que
    /// REEMPLAZA a ese sonido — el asset HD (por firma o por Secuencia) y el
    /// timbre de SoundFont. Hasta 2026-07-28 era «ortogonal a la sustitución» y
    /// se veía como que el altavoz apagaba el original y dejaba sonando el
    /// reemplazo, que es justo al revés de lo que el artista pide.
    /// nullptr/0 = limpiar.
    /// La ventana se extiende ~15 frames (COLA DE RELEASE): el key-off no calla
    /// al FM, lo pone a soltar, y cerrar el mute ahí destapaba esa cola como un
    /// clic. La cola cede ante un evento ajeno en el mismo canal; la ventana
    /// propia no. Medido en tools/mute_silence_probe.
    void     set_audio_instrument_mute(const uint64_t* instruments, uint32_t n) noexcept;
    /// Telemetría del mute sobre la SUSTITUCIÓN (): disparos de HD que no
    /// sonaron por estar silenciados, y streams que hubo que CORTAR porque el
    /// artista silenció con el asset ya sonando. Los dos se ven idénticos desde
    /// afuera («no se oye el reemplazo») pero el segundo es el que faltaba: sin
    /// el corte, silenciar a mitad de un asset largo no hace nada hasta la
    /// próxima repetición. Punteros opcionales. Oráculo:
    /// tools/mute_replacement_probe.
    void     audio_mute_stats(uint64_t* hd_muted, uint64_t* hd_cut) const noexcept;
    /// : anclas (starts de ventana) de la sub de Secuencia `key` en la
    /// toma analizada — la tabla conjunta con reclamo. Diagnóstico.
    std::vector<uint32_t> audio_seq_anchors(uint64_t key);
    /// Telemetría del FALLBACK transaccional (): ocurrencias donde sonó el
    /// ORIGINAL porque el HD asignado no pudo (asset ausente/roto o arranque
    /// fallido), y fallos de arranque SDL con el asset ya listo. El observable
    /// de la regla «asignado ≠ reproducible»: crece y se oye el juego = el
    /// fallback funciona; crece con silencio = hay un mute fuera del handshake.
    void     audio_fallback_stats(uint64_t* fallbacks,
                                  uint64_t* start_fails) const noexcept;
    /// Telemetría de la REANUDACIÓN live (): streams re-armados con
    /// offset tras una pausa o un bypass de Assets, instancias vencidas
    /// descartadas (no se reiniciaron desde cero), y cuadros de offset
    /// acumulados — el observable de «reanudar continúa, no reinicia».
    void     audio_resume_stats(uint64_t* resumed, uint64_t* finished,
                                uint64_t* offset_frames) const noexcept;
    /// Telemetría  Fase 0: voces activas/arrancadas del mixer y el atraso
    /// de colocación acumulado/máximo en muestras (0 sostenido = la fase es
    /// exacta; crece = algún camino programa contra un bloque ya flusheado).
    void     audio_unified_stats(uint64_t* voices, uint64_t* started,
                                 uint64_t* skew, uint64_t* max_skew) const noexcept;
    ///  F4: clasificación del match live, por frame-ocurrencia. `exact` =
    /// la firma activa está asignada (o es de una Secuencia); `rule` = resuelta
    /// por una regla de match ( F3 — fragmentación CUBIERTA por la regla);
    /// `variant` = sin match pero con el MISMO instrumento que una asignada
    /// (la firma fragmentada del issue: «ese sonido» está autorado y aun así
    /// suena el original); `unmatched` = sonido ajeno a la autoría.
    void     audio_live_match_stats(uint64_t* exact, uint64_t* rule,
                                    uint64_t* variant,
                                    uint64_t* unmatched) const noexcept;
    /// Limpia contadores y registro del match live (ventanas de medición
    /// finas — el registro acotado se llenaba con las notas del pasaje
    /// anterior). Lo APRENDIDO (firma→instrumento) se conserva.
    void     audio_live_match_reset() noexcept;
    /// Registro (acotado) de firmas activas SIN match distintas, con su
    /// historia — el dato que convierte «se cuelan sonidos en la transición»
    /// en una lista de firmas concretas con instrumento y canal.
    struct AudioLiveUnmatched {
        uint64_t signature     = 0;
        uint64_t instrument    = 0;   ///< 0 = aún desconocido
        uint64_t first_frame   = 0;
        uint64_t frames_active = 0;
        uint8_t  chip = 0, channel = 0;
        bool     variant = false;     ///< mismo instrumento que una asignada
    };
    size_t   audio_live_unmatched(AudioLiveUnmatched* out, size_t cap) const;
    /// Por qué un asset HD no está listo ("missing"/"empty"/"unsupported"/
    /// "corrupt") o nullptr si está listo / nunca se intentó. Para el estado
    /// por asignación en el Lab. No dispara IO.
    const char* audio_asset_error(const char* asset_path) const;
    /// Streams SFX vivos = los HD one-shot que están sonando AHORA (el disparo
    /// de una Secuencia sustituida, en replay o en vivo). Es el observable de
    /// «¿el HD realmente disparó?» sin escuchar: 0 con el gate cerrado, >0
    /// mientras suena. Lo consume audio_health (MCP) — un mudo de sustitución
    /// se diagnostica mirando este número, no el mixdown.
    size_t   audio_sfx_count() const noexcept;
    /// Telemetría del sintetizador SF2 (): frames de síntesis avanzados,
    /// cortes por SALTO de frame (un seek o un catch-up apagan todas las notas
    /// en vuelo), note_on/note_off emitidos, notas no disparadas por timbre
    /// silenciado y frames sin PCM del emulador (el sintetizador cae a un largo
    /// estimado). «Se escucha degradado» se ve idéntico desde afuera venga de
    /// cualquiera de esos, y cada uno se arregla distinto. Punteros opcionales.
    void     synth_stats(uint64_t* ticks, uint64_t* jumps, uint64_t* note_on,
                         uint64_t* note_off, uint64_t* muted,
                         uint64_t* no_pcm) const noexcept;

    // -- RE-SÍNTESIS CON SOUNDFONT () ------------------------------------
    /// Una asignación timbre → preset. `patch` es el `instrument` del detector:
    /// el hash del patch FM sin frecuencia, sin canal y SIN VOLUMEN — identidad
    /// de TIMBRE. Medido: 30 timbres cubren 60 s de música, así que una banda
    /// sonora entera son 10-30 asignaciones, no miles de eventos.
    struct InstrumentAssign {
        uint64_t    patch;
        const char* soundfont;   ///< basename; el pack lo trae recortado
        uint16_t    bank, preset;
        int8_t      transpose;
        float       gain;
    };

    /// Reemplaza el catálogo de asignaciones. Los SoundFonts se cargan del pack
    /// por basename; los que no estén se ignoran (ese timbre suena con su chip,
    /// que es la degradación correcta).
    void set_instrument_assigns(const InstrumentAssign* a, uint32_t n);

    /// Corta las notas en vuelo y vacía lo encolado. El motor ya lo hace solo
    /// en los saltos de frame; esto es para los cortes que el frontend conoce y
    /// el motor no (cerrar una toma, cambiar de proyecto).
    void synth_panic() noexcept;

    // -- ROUTER DE CANALES POR VOZ () ------------------------------------
    /// Da vuelta el modelo de sustitución: en vez de tapar el chip con una
    /// máscara derivada de ventanas de eventos, el chip queda MUDO y todo lo que
    /// se oye lo produce un router de 10 voces, donde cada una se toma su canal
    /// desde el key-on del propio chip hasta el fin de su cola.
    ///
    /// Eso hace estructural lo que antes era afinación: no hay ventana en la que
    /// equivocarse, así que los huecos entre nota y nota () y la juntura
    /// entre Secuencias () no pueden ocurrir.
    ///
    /// PUESTO POR DEFECTO desde el 2026-07-28 (). Estuvo detrás de un switch
    /// mientras el camino viejo era el de producción; se sacó cuando quedó claro
    /// que el modelo sustractivo no se termina de arreglar — sus dos fugas
    /// conocidas (, ) no se corrigieron: dejaron de poder ocurrir.
    ///
    /// Costo medido (tools/fm_resynth_spike, Golden Axe): correlación de
    /// envolvente 0,9906 y 0,343 ms/frame de los 16,7. A nivel de muestra NO
    /// nula — dos emulaciones distintas del YM2612 nunca nulan— así que el
    /// veredicto fue de oído.
    ///
    /// AYTHER_VOICE_ROUTER=0 restituye el camino viejo entero, sin recompilar:
    /// la salida de emergencia mientras el router sea nuevo.
    ///  fase 0: pide el ENSANCHADO. `logical_w` en píxeles del emulador
    /// (0 = apagado, el default). El frame nativo se centra y a los lados
    /// queda el área extendida, que llena la lámina de la Panorámica.
    ///
    /// El ancho sale de `widescreen_target_width()`: 398 en píxel cuadrado
    /// sobre 224 px, 427 preservando el 4:3 mostrado. Cuál es el default del
    /// pack sigue abierto (ver docs/design/widescreen-safe-zone.md).
    void set_widescreen(uint32_t logical_w) noexcept;
    uint32_t widescreen() const noexcept;

    ///  EM-8.2: el gate del ensanchado, desde el texto de
    /// `widescreen.toml`. El pack declara HASTA DÓNDE ensanchar y BAJO QUÉ
    /// CONDICIÓN; gana la primera regla que se cumple.
    ///
    /// El gate no es un refinamiento: el área extendida sale de la lámina, y la
    /// lámina sólo existe donde el juego RECORRIÓ. Medido con
    /// `widescreen_spike`: en una toma quieta la racha dibujable es 0 por los
    /// cuatro lados. En un menú, ensanchar no muestra el nivel — muestra el
    /// vacío. Por eso el gate es obligatorio para que el ensanchado sirva.
    ///
    /// Texto vacío, o sin `[[widescreen]]`, DESARMA el gate y devuelve el
    /// control a `set_widescreen()` — no apaga el ensanchado. La diferencia
    /// importa: los packs ya horneados no declaran nada.
    void set_widescreen_gate(const std::string& toml);
    /// Si el pack está opinando sobre el ancho en este momento.
    bool widescreen_gated() const noexcept;
    /// El ancho EFECTIVO del último frame producido: el del gate si opinó, si
    /// no el pedido. Es lo que viaja en `FrameView.wide_w`.
    uint32_t widescreen_effective() const noexcept;

    void set_voice_router(bool on) noexcept;
    bool voice_router() const noexcept;

    /// Telemetría del router: frames de router avanzados, frames de chip
    /// consumidos (con el catch-up son varios por tick), cebados tras un seek,
    /// y bloques en que el resampler no tuvo entrada suficiente. Ese último es
    /// el detector de pacing que a  le faltó durante diez causas raíz.
    /// Punteros opcionales.
    /// `substituted`: key-ons que NO cayeron en copia — o sea voces que el
    /// router calló o reemplazó. Es la única forma de VER desde afuera que un
    /// mute llegó al router: con el chip mudo, audio_mute_mask es 0x3FF siempre
    /// y no dice nada ().
    void voice_router_stats(uint64_t* ticks, uint64_t* chip_frames,
                            uint64_t* primes, uint64_t* starved,
                            uint64_t* substituted = nullptr) const noexcept;
    /// Mute dinámico por OCURRENCIA exacta (claves chip<<56|canal<<48|start,
    /// las de audio_event_key del Lab): el canal se silencia durante la
    /// ventana de cada ocurrencia marcada. Secuencias DESHABILITADAS con el
    /// ojo — ni su HD ni su sonido ORIGINAL deben oírse; otras apariciones del
    /// mismo sonido siguen sonando. nullptr/0 = limpiar.
    /// El HD calla por el motor (): que la app excluya además la sub es
    /// defensa en profundidad, no el mecanismo — el mute vale igual para un
    /// asset que reemplace esa ocurrencia desde OTRA Secuencia o asignación.
    void     set_audio_occurrence_mute(const uint64_t* keys, uint32_t n) noexcept;

    // -- Sustitución por SECUENCIA (grupo de eventos → 1 HD, workspace Audios) ---
    // Una Secuencia agrupa varios canales bajo un único HD, disparado por la firma
    // DISPARADORA (trigger_signature — el evento de menor start_frame de la
    // Secuencia, desempate por chip/canal) con una ventana RELATIVA a cada
    // ocurrencia de esa firma (duration_frames = span autoral del grupo o
    // asset_frames, lo que sea mayor) — MISMO modelo que usa pack_bake.cpp para el
    // pack exportado (audio_events.toml → core/src/audio_event.rs), así el preview
    // del Lab coincide con lo que hace el juego real: re-dispara en CADA repetición
    // de la firma disparadora, no sólo la primera (2026-07-22, bug reportado: con
    // un rango [start,end] fijo por Secuencia sólo sonaba la 1ª ocurrencia).
    // Sólo surte efecto con el preview de sustitución activo y device de audio abierto.
    struct AudioSeqSub {
        uint64_t    trigger_signature = 0;   ///< firma que abre la ventana en cada ocurrencia
        uint32_t    duration_frames   = 1;   ///< ventana RELATIVA al inicio del trigger
        /// Span de los EVENTOS de la Secuencia (sin el largo del HD): el paso
        /// de SEGMENTACIÓN de las ocurrencias del disparador. Un loop musical
        /// que repite cada `span` con un HD más largo re-ancla en CADA pasada
        /// (el HD reinicia, como el juego) — segmentar por duration_frames se
        /// tragaba las repeticiones alternas («suena la 1ª y la 3ª, no la 2ª
        /// ni la 4ª», reporte 2026-07-23). 0 = usar duration_frames.
        uint32_t    span_frames       = 0;
        float       gain              = 1.0f;  ///< volumen del HD (0..2)
        uint32_t    channel_mask = 0;   ///< 0-5 FM · 6-9 PSG · 10-17 PCM (fallback si signatures está vacío)
        uint64_t    key          = 0;   ///< id de la Secuencia (dedup del one-shot)
        std::string asset;              ///< ruta del HD (vacío = no sustituye)
        /// Firmas de los eventos MIEMBRO: dentro de la ventana se mutea SOLO el
        /// evento activo cuya firma esté acá (por su propio span), no el canal
        /// completo — otros sonidos que compartan canal siguen sonando
        /// (reporte 2026-07-23). Vacío = comportamiento viejo (channel_mask).
        std::vector<uint64_t> signatures;
        /// : CABEZA — las firmas que arrancan en el MISMO frame que el
        /// disparador (incluido). Con ≥ 2, la mayoría de la cabeza ancla
        /// aunque el disparador sea una variante (ver audio_seq_anchor.h).
        std::vector<uint64_t> head_signatures;
        bool                  looping = false;   ///< : continuación en el empate
        /// /: política de fin de la ventana. `tail_frames` =
        /// UINT32_MAX es «no autorado» (legacy: el non-loop drena entero);
        /// `fade_frames` > 0 gana sobre el tail — son alternativas.
        uint32_t    tail_frames = UINT32_MAX;
        uint32_t    fade_frames = 0;
        ///  F3: regla de MATCH del disparador — con kInstrument, cualquier
        /// voz del MISMO timbre ancla la Secuencia (la fragmentación de la
        /// transición: el mismo SFX con el chip en otro estado es OTRA firma).
        /// La identidad viene AUTORADA (persistida), no inferida.
        AudioMatchRule match_rule       = AudioMatchRule::kExact;
        uint64_t       match_instrument = 0;
        uint8_t        match_pitch      = kAudioNoPitch;
        /// : el BUS al que pertenece lo que suena en esta Secuencia — el
        /// «Tipo» que el autor eligió en Mezclar. No se deduce (el motor
        /// identifica timbres, no categorías), así que llega autorado o no
        /// llega. `AudioBus::Unclassified` = el autor no lo dijo.
        AudioBus       bus = AudioBus::Unclassified;
    };
    void     set_audio_sequence_subs(std::vector<AudioSeqSub> subs);
    const std::vector<AudioSeqSub>& audio_seq_subs() const noexcept;   //  diagnóstico
    /// Reproduce un archivo de audio (HD) one-shot — preview AISLADO de una Secuencia
    /// (el play de su timeline). No mueve el playhead ni toca la toma. Sólo suena con
    /// device de audio abierto. Ruta vacía = no-op.
    void     preview_asset_file(const char* path, float gain = 1.0f);
    /// Duración de un asset HD de disco EN FRAMES de juego (a timing_fps) — para
    /// dimensionar el span del timeline de Secuencia cuando el HD es más largo que
    /// los eventos. 0 si no se puede leer/decodificar. No necesita device de audio.
    uint32_t audio_asset_frames(const char* path) const;

    // -- Persistencia audio_events.toml (C-A5) ---------------------------------
    // Serializa las asignaciones firma→asset (+ la máscara de canales de los
    // eventos detectados, para el runtime) a texto TOML; y las recarga. El Lab
    // escribe el texto en el .ay / proyecto y lo relee al cargar. Los eventos en
    // sí no se guardan (son re-derivables re-analizando la toma, deterministas).
    std::string audio_events_toml() const;
    void        load_audio_events_toml(const char* text);
    /// Carga las asignaciones desde el `audio_events.toml` del pack actual (runtime).
    /// El Lab usa el TOML del PROYECTO; el runtime usa el del PACK — por eso es
    /// explícito (no se hace en set_pack, para no pisar las del proyecto).
    void        load_audio_events_from_pack();

    // -- Recording + replay (R7) -----------------------------------------------
    // record_start() snapshots the current state as the take's initial frame and
    // logs each subsequent step()'s port-0 input. take_recording() moves out the
    // finished take (input stream + initial state).
    void            record_start();
    void            record_stop();
    bool            recording()        const noexcept;
    size_t          recorded_frames()  const noexcept;
    AytherRecording take_recording();

    /// Renderiza `frame` de `rec` re-simulando lo mínimo: arranca del cursor vivo
    /// si va por delante, del keyframe cacheado más cercano ≤ frame, o del estado
    /// inicial; corre el resto con run_frame "bare" y cachea keyframes en el
    /// camino. Scrubbing determinista (lab.md §7.3), ya sin el O(frame) por clic.
    /// `quiet` (scrub del usuario): el fast-forward corto DESCARTA el PCM de los
    /// frames intermedios en vez de conservarlo — conservarlo es correcto solo
    /// para el catch-up del playback (); en un scrub encola audio a 1× y la
    /// reproducción se desfasa del cabezal (reporte 2026-07-21). El frame
    /// visible conserva su audio (blip de scrub).
    const FrameView* replay_seek(const AytherRecording& rec, uint32_t frame,
                                 bool quiet = false);

    /// Vacía el cache de replay (keyframes + cursor). Llamar cuando un objeto
    /// AytherRecording se reusa con OTRO contenido (cargar otra toma, split): el
    /// puntero no cambia y el chequeo de identidad de replay_seek no lo detecta.
    void            replay_reset();

    /// Invalida el cursor de replay para que el PRÓXIMO replay_seek re-renderice
    /// aunque sea al MISMO frame (replay_seek normalmente hace early-return ahí).
    /// Para re-aplicar cambios que sólo afectan el render — p.ej. la máscara de
    /// capas — sobre un frame pausado.
    void            replay_invalidate();
    /// : cuántos keyframes RUNTIME (replay_keys, crudos) acumuló el
    /// playback/seek de la toma actual. Con keyframes HORNEADOS en la toma es
    /// siempre 0 por diseño (R7e: los horneados cubren el rango).
    size_t          replay_key_count() const;

    /// Progreso de un seek troceado (replay_seek_chunk).
    struct SeekStep {
        const FrameView* view = nullptr;  ///< frame del target (sólo con done=true)
        bool             done = false;    ///< true al alcanzar el target
        float            progress = 0.0f; ///< 0..1 mientras done==false
    };

    /// Frames que replay_seek(rec, frame) re-simularía (distancia al mejor
    /// arranque). 0 = inmediato. El frontend decide con esto entre seek directo
    /// o seek en chunks con loader.
    uint32_t        replay_seek_cost(const AytherRecording& rec, uint32_t frame) const;

    /// Seek en chunks: re-simula ≤ `budget` frames por llamada hacia `frame`,
    /// para no congelar la UI en saltos largos (toma fría). El caller lo bombea
    /// cada frame y muestra el progreso hasta done. Resultado idéntico a
    /// replay_seek; sólo reparte el costo entre frames de UI.
    SeekStep        replay_seek_chunk(const AytherRecording& rec, uint32_t frame,
                                      uint32_t budget);

    /// Migración R7e: hornea keyframes en una toma SIN ellos, troceado (≤ budget
    /// frames/llamada) para no congelar. Bombear hasta done; al terminar,
    /// `rec.keyframes` está poblado y el caller re-guarda el .arp. No-op (done) si
    /// la toma ya tiene keyframes o es más corta que un intervalo.
    SeekStep        replay_bake_step(AytherRecording& rec, uint32_t budget);

    /// Migración .arp v8: re-hornea la HISTORIA de hashes de sprites de una toma
    /// vieja (rec.hash_algo < kSpriteHashAlgo) con el hasher VIGENTE, troceado
    /// (≤ budget frames/llamada — produce CADA frame, más caro que el warm bare
    /// de R7e). Bombear hasta done; al terminar rec.sprite_hashes/hash_offsets
    /// están reconstruidos, rec.hash_algo actualizado, y si la toma no tenía
    /// keyframes también quedan horneados (el barrido los captura gratis) — el
    /// caller re-guarda el .arp. Silencioso (replay_quiet) y con las máscaras/
    /// overrides del Lab stasheados: la historia registra el juego COMPLETO.
    SeekStep        replay_rebake_history_step(AytherRecording& rec, uint32_t budget);

    /// Fase C: divide `rec` en head=[0,frame) y tail=[frame,end). Re-simula
    /// [0,frame) desde initial_state para capturar el savestate del tail
    /// (estado PRE-frame `frame` -- replay_seek+serialize quedaria post-F).
    /// CLOBBEREA el estado vivo del emulador: el caller debe re-seekear luego.
    /// false si frame==0, frame>=frame_count() o falla la (de)serializacion.
    bool split_recording(const AytherRecording& rec, uint32_t frame,
                         AytherRecording& head, AytherRecording& tail);

    /// Corte destructivo (Recortar): `out` = sub-toma [begin, end) rebasada a 0.
    /// Con begin>0 re-simula para capturar el savestate inicial (mismo camino
    /// post-frame que split_recording) y CLOBBEREA el estado vivo del emulador:
    /// el caller debe re-seekear luego. false si el rango es invalido o falla
    /// la (de)serializacion.
    bool crop_recording(const AytherRecording& rec, uint32_t begin, uint32_t end,
                        AytherRecording& out);

    // -- Modo avanzado (Lab): Work RAM + cheats ---------------------------------
    // Lectura pasiva de la Work RAM del core (64KB del 68k en Genesis) para el
    // Memory Explorer; cheats GG/PAR via retro_cheat_set (GPX los implementa).
    const uint8_t* work_ram(size_t* size) const;
    void cheat_set(unsigned index, bool enabled, const char* code);
    void cheat_reset();

    /// VRAM del VDP (64KB: tiles + tilemaps + SAT) — null si el core no la
    /// expone (requiere el fork _vram). Lectura pasiva (mapa multi-espacio).
    const uint8_t* video_ram(size_t* size) const;
    /// CRAM del VDP (128 bytes = 64 colores de 9 bits como u16 host-endian,
    /// layout GPX 0000BBB0GGG0RRR0) — null sin el fork. Lectura pasiva.
    const uint8_t* color_ram(size_t* size) const;
    /// /: firma de contenido de la línea de paleta `line` (0-3) sobre
    /// los `slots` marcados (bitmask; 0xFFFF = toda la línea), desde la CRAM
    /// viva — envuelve ayther_palette_signature (la fn del runtime). 0 sin CRAM.
    uint64_t       palette_signature(uint8_t line, uint16_t slots) const;
    /// Los 32 registros del VDP (reg[0x20]) — null sin el fork. Para derivar
    /// las bases de las name tables de planos (tilemap viewer, M9.3).
    const uint8_t* vdp_regs(size_t* size) const;
    /// Lista CRUDA de sprites parseados por el VDP este frame (ids 0x10B/0x10C del
    /// fork): entradas de 8 bytes {yr u16, xr u16, attr u16, w u8, h u8} (valores
    /// crudos del SAT: yr/xr con offset +128; attr = tile|flips|pal|pri). Fuente
    /// autoritativa de "qué dibujó el VDP" (robusta a rewrites mid-frame del SAT).
    /// count = entradas. nullptr si el core no la expone. Diagnóstico/sondas.
    const uint8_t* parsed_sprites_raw(uint8_t* count) const;

    /// : resuelve un export arbitrario del core CARGADO (p.ej.
    /// ayther_recompose_frame del fork — la recomposición del frame desde el
    /// estado final del VDP, para el spike de fidelidad del render propio).
    /// nullptr si el core no lo exporta (stock). El caller castea al tipo real.
    void* core_export(const char* name) const;

    // -- E-7 (): las capas nativas del VDP, en UNA llamada ---------------
    // Plano B, plano A, ventana y sprites tal como los dibuja el VDP —con los
    // efectos raster del frame ya aplicados— más el composite, que es el frame
    // entero y sirve para verificar la extracción sin un segundo render.
    //
    // POR QUE ESTO NO ES LO QUE EL RENDERER USA. El pipeline de capas del
    // renderer recompone en la GPU POR CELDA (R-2/R-3), que es lo que permite
    // sustituir un tile por un asset HD. Esto devuelve BITMAPS ya rasterizados:
    // no lo reemplaza ni compite con él. Sirve para lo que la celda no puede —
    // componer fuera de la ventana 4:3 (), sacar una capa limpia para
    // autorarla, y ser la referencia CPU contra la que se valida la GPU.
    //
    // Los buffers son de la sesión y se reusan entre frames: el caller NO los
    // libera y valen hasta la próxima llamada. `pitch` es `width` (compacto).
    struct Layers {
        const uint16_t* bg_b      = nullptr;   ///< RGB565, width*height
        const uint16_t* bg_a      = nullptr;
        const uint16_t* window    = nullptr;
        const uint16_t* sprites   = nullptr;
        const uint16_t* composite = nullptr;   ///< el frame entero
        uint32_t width = 0, height = 0;
        bool ok() const { return composite != nullptr; }
    };

    /// Extrae las capas del ÚLTIMO frame producido. Llamar tras step()/seek.
    ///
    /// Devuelve `ok() == false` sin ruido cuando el core no puede: core stock,
    /// sin la capability, sin suscripción, o un modo gráfico que la
    /// recomposición no soporta. El motivo queda en `layers_error()`, y se
    /// loguea UNA vez por motivo — no una vez por frame, que es la diferencia
    /// entre un diagnóstico y una inundación del log a 60 Hz.
    ///
    /// TRANSACCIONAL (el criterio de  aplicado al video): o están las cinco
    /// capas o no está ninguna. Nunca se publica un composite sin sus capas:
    /// suprimir el original por un HD que después no va a poder dibujarse es
    /// exactamente el defecto que  corrigió en el audio.
    Layers recompose_layers();

    /// Por qué la última llamada no pudo, como texto listo para el log
    /// («el juego no está en modo gráfico 5», «interlace doble»…). Cadena vacía
    /// si la última salió bien o si no se llamó nunca.
    const char* layers_error() const noexcept;

    /// R-5 (): alias — la definición vive a nivel de namespace (FrameView
    /// publica la escena y se declara antes que esta clase).
    using SceneElement = ayther::SceneElement;

    /// Construye el inventario del ÚLTIMO frame producido (lee la FrameView
    /// viva — llamar tras produce/seek; válido hasta el próximo step). Junta
    /// las celdas de plano (walk scroll-aware de la Fase C) con los sprites
    /// del hasher (+ patrón desde la SAT parseada del fork) y anota qué
    /// elemento reemplaza un asset HD ya resuelto. Devuelve el total emitido.
    /// Vacío sin core forkeado (sin VRAM no hay celdas ni SAT parseada).
    size_t scene_inventory(std::vector<SceneElement>& out) const;
    /// : telemetría del juez de framebuffer del último `scene_inventory`
    /// (sprites juzgados · descartados · muestras opacas · coincidencias).
    void scene_judge_stats(uint32_t* occs, uint32_t* dropped,
                           uint32_t* opaque, uint32_t* hits) const;

    /// R-4 (): alias — definición a nivel de namespace (junto a
    /// SceneElement, cuya identidad de ocultado comparte).
    using HiddenElement = ayther::HiddenElement;

    /// R-4 (): oculta ELEMENTOS del inventario, unificado — la sesión
    /// rutea por capa a los dos canales por-elemento existentes (3=Sprite →
    /// ocultado compuesto; 0-2 → tiles de plano). Aplica en el PRODUCE del
    /// mismo frame (sin la latencia del canal de supresión del core) y no
    /// pisa los ojos del Lab (se une, no reemplaza). nullptr/0 = limpiar. El
    /// inventario refleja el estado en SceneElement.hidden. Límite conocido:
    /// dentro de los planos el canal 0x105 oculta el GRÁFICO en A/B/W por
    /// igual (afinarlo por plano es posible — la máscara ya es por-plano —
    /// pero queda fuera de R-4).
    void set_hidden_elements(const HiddenElement* els, uint32_t n);

    /// R-6 (): asigna EFECTOS a elementos del inventario por (capa, hash)
    /// — tinte Q2.6, opacidad, silueta. Reemplaza la lista completa (nullptr/0
    /// = limpiar). El inventario los publica en SceneElement.fx_* y el compose
    /// los aplica por quad; los frames de fallback al blit no los muestran
    /// (mismo trato que la visibilidad — transiciones, documentado en R-5).
    /// Alias en la clase para el patrón de siempre:
    using ElementEffect = ayther::ElementEffect;
    void set_element_effects(const ElementEffect* fx, uint32_t n);

    ///  (runtime_enhancement): marca elementos del inventario para
    /// MEJORARLOS por software (EPX sobre índices) por (capa, hash). Reemplaza
    /// la lista COMPLETA de la fuente Lab (nullptr/0 = limpiar); la fuente
    /// pack (load_pack_into) vive aparte y se une. Canal propio y no
    /// ElementEffect: el tool MCP `element_effect` reemplaza la lista entera de
    /// efectos y pisaría la política. Un elemento reclamado por HD no se
    /// mejora (SceneElement.fx_enhance = 0: el asset ganó); el gate maestro
    /// hd_on lo apaga en el A/B «Original» () y en el export sin HD.
    using EnhancedElement = ayther::EnhancedElement;
    void set_enhanced_elements(const EnhancedElement* els, uint32_t n);


    /// Máscara de capas visibles del VDP (bits A/B/Window/Sprites de
    /// AYTHER_LAYER_*). El renderer del fork la lee por línea: 0xFF = todo
    /// visible. Permite aislar capas en el viewport (autoría en Editar). No-op
    /// con un core stock (sin el id de escritura 0x102).
    void set_layer_mask(uint8_t mask);

    /// Atenuar las capas NO-sprite al 25% en el frame visible (id 0x108 del fork).
    /// Con `on`, `produce_frame` emite los píxeles que no son de sprite al 25%
    /// (preponderancia visual de los sprites — viewport de Animación). Independiente
    /// de la máscara de capas (usar 0xFF para que los fondos se rendericen y luego
    /// se atenúen). No-op con un core stock. Produce-only (la re-sim bare corre sin dim).
    void set_layer_dim(bool on);


    /// Slots SAT a ocultar (bitmask de hasta 128 bits = 16 bytes; bit i = slot i).
    /// Aplicado SÓLO al frame visible (la re-sim bare corre con sprites completos
    /// → sin divergencia). Para ocultar sprites individuales por hash en Editar.
    /// `n` se acota a 16. No-op con core stock (sin el id 0x103).
    void set_sprite_suppress(const uint8_t* bits, size_t n);

    /// Ocultado de sprites POR HASH, COMPUESTO (Posar): el frame visible A se
    /// produce COMPLETO (occs/listas estables, sin efectos del presupuesto de
    /// línea del VDP), un 2º render B suprime los slots de esos hashes (mapeados
    /// de las occs de A, mismo frame — sin lag), y la base publicada toma B SÓLO
    /// dentro del rect de cada occ oculta: lo de abajo queda visible exactamente
    /// bajo el sprite oculto y nada más cambia. El "fantasma" translúcido lo
    /// dibuja el frontend encima (capa Vulkan). Transitorio — no se serializa.
    /// nullptr/0 = limpiar.
    void set_sprite_hidden(const uint64_t* hashes, uint32_t n);

    /// Celdas de tile a ocultar (máscara de 512 bytes = 64x64 celdas de 8px,
    /// stride 64 cols; bit `ty*64+tx`). El frontend mapea el hash de tile oculto
    /// → celdas del frame visible (`fv.tile_occs`); la celda se pinta con el
    /// backdrop del VDP (revela el fondo). Aplicado SÓLO al frame visible
    /// (produce-only) → re-sim bare intacta. `n` se acota a 512. No-op con core
    /// stock (sin el id 0x104).
    void set_tile_suppress(const uint8_t* bits, size_t n);

    /// Tiles de PLANO a ocultar, por hash (los de `fv.plane_tile_occs`). A
    /// diferencia de `set_tile_suppress` (por celda de pantalla), esto oculta el
    /// GRÁFICO del tile dondequiera que aparezca en su plano, sin depender del
    /// scroll: el core (id 0x105) saltea esas celdas en `render_bg_m5/_vs` y revela
    /// el plano de atrás. La sesión mapea hash → (plano, patrón, paleta) con las
    /// occurrences ya vistas y arma la máscara aplicada SÓLO al frame visible
    /// (produce-only). Fase 2b del panel Capas. No-op con core stock (sin 0x105).
    void set_plane_tile_hidden(const uint64_t* hashes, size_t n);

    /// Decodifica un patrón de tile del VDP (8×8, 4bpp) a BGRA (out = 8*8*4 = 256
    /// bytes, stride 8 px) aplicando paleta CRAM + flips. Para la vista previa de
    /// un tile de plano en el panel Capas. No-op (deja `out` intacto) si el core
    /// no expone VRAM/CRAM. `pattern` 0..2047, `pal` 0..3.
    void decode_plane_tile(uint16_t pattern, uint8_t pal, bool hflip, bool vflip,
                           uint8_t* out_bgra) const;

    /// Variante RGBA para el EXPORT de fondos: igual que decode_plane_tile pero
    /// en orden RGBA y con el color 0 TRANSPARENTE (alpha 0) — la semántica del
    /// VDP (el índice 0 deja ver el plano de atrás / backdrop), que el PNG por
    /// capa necesita para que el plano A no tape al B al re-importar.
    void decode_plane_tile_rgba(uint16_t pattern, uint8_t pal, bool hflip, bool vflip,
                                uint8_t* out_rgba) const;

    /// Vista 68k de la Work RAM. En hosts little-endian GPX guarda el array
    /// word-swapped (READ_BYTE usa addr^1) — verificado empíricamente contra
    /// Sonic 2 ($FFFE24/25 = seg/frames del timer; spike maper_probe). Estos
    /// accessors devuelven los bytes COMO LOS VE EL 68k, para que las
    /// direcciones del Lab coincidan con las documentadas (RetroAchievements,
    /// Data Crystal). u16/u32 en big-endian del bus. Fuera de rango → 0.
    uint8_t  ram_u8 (uint32_t off) const noexcept;
    uint16_t ram_u16(uint32_t off) const noexcept;
    uint32_t ram_u32(uint32_t off) const noexcept;

    /// Poke (Maper M5): escribe `len` bytes en la vista 68k a partir de
    /// `off` y marca la sesión SUCIA — una escritura fuera del input stream
    /// rompe el determinismo del replay, así que REC queda bloqueado hasta
    /// volver a un estado limpio (unserialize de un marcador o reset, que
    /// limpian el flag). false: sin RAM o fuera de rango.
    bool poke(uint32_t off, const uint8_t* data, size_t len);
    bool dirty() const noexcept;          ///< navegada por poke
    void clear_dirty() noexcept;

    // -- RAM del Z80 () ---------------------------------------------------
    //
    // Los 8 KB que el 68k ve en 0xA00000-0xA01FFF. Varios juegos dejan ahí el
    // id del tema a tocar, y por eso una herramienta que quiera disparar un
    // sonido por id necesita poder mirarla — cuando la casilla no está en work
    // RAM, no había dónde buscar.
    //
    // Vacío con un core stock: la región es del fork (ABI 1.9), y decir «no
    // hay» es distinto de decir «está en cero».
    const uint8_t* z80_ram() const noexcept;
    size_t         z80_ram_size() const noexcept;
    /// Escribe en la RAM del Z80. `false` = sin región o fuera de rango.
    ///
    /// ES UNA CARRERA con el Z80 corriendo, y por eso ensucia la sesión igual
    /// que `poke`: lo que se escribe puede durar un frame. Para dejar un id de
    /// sonido donde el Z80 lo lee alcanza; para cualquier otra cosa, no.
    bool z80_poke(uint32_t off, const uint8_t* data, size_t len);

    // -- Cheats del JUGADOR ( EM-7.3) -------------------------------------
    //
    // No confundir con el poke del Maper, que es de autoría: el modder sabe qué
    // dirección toca y por qué; el jugador tiene una cadena de nueve letras que
    // copió de algún lado.
    //
    // SE REAPLICAN POR FRAME, y eso es lo que los hace funcionar: el juego
    // reescribe esas direcciones todo el tiempo. Escribir una vez sirve para lo
    // que el juego no vuelve a tocar; para lo demás hay que insistir.
    //
    // La sesión queda SUCIA, como con cualquier poke: una escritura fuera del
    // input stream rompe el determinismo del replay. Jugar con cheats y grabar
    // una toma para autorar son cosas que no se mezclan, y el flag lo dice.

    /// Agrega un cheat ya decodificado (`ayther_core::cheat_code`). Se aplica
    /// desde el próximo frame.
    void add_cheat(uint32_t address, uint16_t value);
    /// Los saca todos. Lo que ya escribieron NO se deshace: el juego sigue con
    /// las vidas que le pusieron, que es lo que el jugador espera.
    void clear_cheats() noexcept;
    uint32_t cheat_count() const noexcept;

    // -- Scripting -------------------------------------------------------------
    Result<void> load_script(const std::string& lua_source, const char* chunk_name);

    // -- Live authoring (Lab) --------------------------------------------------
    // Assign an HD asset to a sprite hash. The mapping persists across frames
    // (re-applied after Lua overrides) so the artist sees it live in the
    // viewport — provided the asset is resolvable by the renderer (i.e. present
    // in the loaded pack). Loose on-disk files are recorded for the eventual
    // pack build but do not render until packed.
    /// `ref_rgb` (, opcional): referencia cromática E1 capturada al asignar
    /// (promedio RGB de la línea CRAM del sprite, 3 bytes) — el tinte por canal
    /// sigue los cambios de COLOR de la paleta. null = sin ref → peak-hold gris.
    void assign_sprite(uint64_t hash, const std::string& asset_path,
                       const uint8_t* ref_rgb = nullptr);
    void unassign_sprite(uint64_t hash);
    void clear_assignments();   ///< clears sprite + tile + audio + plane + Mode 3 kind assignments
    // Sustitución de pose TRANSITORIA (modelo de autoría: todo elemento es una
    // Pose). Cada entrada: hashes de los miembros + offsets relativos (rel_x/rel_y,
    // paralelos; vacíos = matching legacy por set) + asset (HD autorado o snapshot
    // del original) + flag hd. Con rel, la pose sólo matchea con los miembros en
    // sus offsets EXACTOS (bbox 1:1, una sub por instancia — sin estirar). Las
    // regiones de poses con hd=true además limpian los sprites sueltos adentro.
    // NO se serializa a ningún .toml. Reemplaza el conjunto completo; vacío = off.
    /// Candidato de asset por variante ( paso 2): la config (paleta/flip,
    /// -1 = cualquiera) para la que se autoró `asset`. El motor elige el más
    /// próximo a la variante observada del ancla. : `slots`/`sig` =
    /// identidad por CONTENIDO de paleta (bitmask de slots marcados + firma
    /// xxh3 del contenido estable); sig 0 = sin firma.
    struct PoseVariant {
        int8_t palette = -1, hflip = -1, vflip = -1;
        uint16_t slots = 0;
        uint64_t sig   = 0;
        std::string asset;
    };

    struct PosePreview {
        std::vector<uint64_t> hashes;
        std::vector<int16_t>  rel_x, rel_y;   ///< offsets por miembro ("" = legacy)
        /// Tamaño en PX de cada miembro (paralelo a hashes; vacío = desconocido):
        /// la tolerancia off-screen de un miembro AUSENTE necesita SUS dims reales
        /// — sin ellas el motor las aproxima con las del primer miembro visible y
        /// el match cae cuando una cabeza 8×8 sale por un borde.
        std::vector<int16_t>  dim_w, dim_h;
        /// : flips SAT observados por miembro al capturar (bit0 = hflip ·
        /// bit1 = vflip, paralelo a hashes; vacío = pose legacy). El resolver
        /// desempata instancias PARCIALES geométricamente ambiguas por
        /// agreement (occ.flip == member_flip ^ arreglo).
        std::vector<uint8_t>  mem_flips;
        uint16_t              bbox_w = 0, bbox_h = 0;  ///< tamaño de captura px (guard anti-gigante del legacy)
        std::string           asset;
        /// Vestuario: máscara de tinte del asset BASE ("" = sin máscara). El
        /// motor la adjunta al sub sólo cuando el asset elegido ES `asset`
        /// (un candidato de variante es recolor autorado y no la lleva).
        std::string           mask;
        bool                  hd = false;     ///< true = HD autorado; false = snapshot
        /// Cara en que está dibujado `asset` respecto de la capturada (flip de
        /// presentación de Posar): el motor la XORea al espejo detectado.
        bool                  flip_h = false, flip_v = false;
        // Paso 2 (): candidatos por variante. Vacío = un solo `asset`. Con
        // candidatos, el motor elige el más próximo a la variante observada.
        std::vector<PoseVariant> candidates;
        /// Referencia autorada del tinte E1: promedio RGB (0-255 por canal) de
        /// la línea CRAM de la pose al capturarla — "cómo se ve normal". El
        /// motor tinta el HD por canal live/ref (sigue fades Y flashes de
        /// color). {0,0,0} = sin referencia → peak-hold escalar (E1 clásico).
        uint8_t               ref_rgb[3] = { 0, 0, 0 };
        /// : referencia E1 POR LÍNEA de paleta (0-3) para poses de paletas
        /// MIXTAS — el motor emite un quad por grupo de línea y cada uno tinta
        /// contra la ref de SU línea. Línea {0,0,0} = sin ref (el grupo del
        /// ancla cae a `ref_rgb`; los demás, al peak-hold de su línea).
        uint8_t               ref_line[4][3] = {};
    };
    void set_pose_preview(const std::vector<PosePreview>& poses);

    /// In-betweens (§6.1/6.2) EN VIVO: una transición por entrada — `from`
    /// vacío = comodín «desde cualquier pose». Los strings de asset deben
    /// coincidir con los del canal de poses (rutas del proyecto). Aplicar solo
    /// AL CAMBIAR el pool (el TweenPlayer conserva los overrides entre frames,
    /// a diferencia del pose-set que se re-inyecta por frame).
    struct TweenPreview {
        std::string from;                 ///< asset origen ("" = comodín)
        std::string target;               ///< asset destino
        std::vector<std::string> frames;  ///< dibujos ordenados
        uint32_t    ticks = 3;            ///< frames de juego por dibujo
    };
    void set_tween_preview(const std::vector<TweenPreview>& tweens);
    // Look up the current assignment for a hash ("" if none).
    const char* assignment_for(uint64_t hash) const noexcept;
    // Enumerate all (hash, asset_path) assignments, sorted by hash (for the
    // Deliver workspace to serialise into a pack). R8.
    std::vector<std::pair<uint64_t, std::string>> assignments() const;
    /// Igual que assignments() pero con la ref cromática E1 por entrada ()
    /// — para persistir sprites.toml y hornear los [[sub]] con `ref`.
    struct SpriteAssignment { uint64_t hash; std::string asset; uint8_t ref_rgb[3]; };
    std::vector<SpriteAssignment> sprite_assignments() const;

    // Tile + audio HD assignments — same model as sprites (persist across frames,
    // applied after Lua overrides). Enumerated by the Deliver workspace to bake
    // tile_substitutions.toml / audio_substitutions.toml into a pack.
    void assign_tile(uint64_t hash, const std::string& asset_path);
    void unassign_tile(uint64_t hash);
    const char* tile_assignment_for(uint64_t hash) const noexcept;
    std::vector<std::pair<uint64_t, std::string>> tile_assignments() const;

    void assign_audio(uint64_t hash, const std::string& asset_path);
    void unassign_audio(uint64_t hash);
    const char* audio_assignment_for(uint64_t hash) const noexcept;
    std::vector<std::pair<uint64_t, std::string>> audio_assignments() const;

    // Plane-tile HD assignments (Fase 2c) — mismo modelo que tiles, pero la
    // identidad es el hash de contenido del tile de plano (plano+patrón+paleta).
    // El reemplazo se resuelve scroll-aware en produce_frame (overlay por posición).
    // El Deliver hornea `plane_tile_substitutions.toml`.
    void assign_plane(uint64_t hash, const std::string& asset_path);
    void unassign_plane(uint64_t hash);
    const char* plane_assignment_for(uint64_t hash) const noexcept;
    std::vector<std::pair<uint64_t, std::string>> plane_assignments() const;

    // -- Plane SETS (Pintar Fase C): sustitución HD por ELEMENTO multi-tile ----
    // Un set = conjunto de tiles de plano con offsets relativos en CELDAS
    // (el catálogo de Elementos de Pintar). El matcher corre en el scan de
    // planos: por cada aparición del ancla (member[0]) verifica el resto en
    // sus offsets; si TODOS están → UN overlay del asset estirado al bbox
    // (lane lo/hi según la prioridad VDP del ancla) y los tiles miembros se
    // suprimen por identidad (misma latencia de 1 frame que el canal 0x105).
    struct PlaneSetMember { uint64_t hash; int16_t cx, cy; };
    /// `ref_rgb` (3 bytes, opcional): referencia del tinte E1 — promedio RGB
    /// 0-255 de la línea CRAM del elemento «como se ve normal» (capturado al
    /// crearlo, mismo contrato que el `ref` de las poses). Con referencia, el
    /// quad del set se tinta live/ref por canal y sigue los fundidos de
    /// paleta; nullptr/{0,0,0} = sin tinte (comportamiento previo).
    void define_plane_set(uint64_t id, uint8_t plane, uint16_t w_cells,
                          uint16_t h_cells, const PlaneSetMember* members,
                          uint32_t member_count, const std::string& asset_path,
                          const uint8_t* ref_rgb = nullptr);
    void undefine_plane_set(uint64_t id);
    void clear_plane_sets();

    // -- ANIMACIÓN (): una SECUENCIA de plane sets con reloj propio -------
    //
    // QUÉ APORTA SOBRE LOS PLANE SETS. Un set ya sustituye por hash: gráfico A
    // → asset A. Eso alcanza cuando el juego ya anima y cada fase tiene su
    // propio hash. La Animación es para lo que ESO no puede: que el HD tenga
    // MÁS fases que el original (el juego alterna A·B y el artista quiere un
    // ciclo de seis dibujos), o que corra a otra cadencia. Por eso es un
    // REPRODUCTOR CON RELOJ PROPIO y no un seguidor del contenido — un seguidor
    // no podría mostrar un paso que en pantalla no está.
    //
    // CÓMO SE MONTA. No duplica el matcher: cuando CUALQUIER paso de la
    // secuencia matchea, esa posición queda tomada por la Animación y el asset
    // que se dibuja ahí es el del paso VIGENTE POR RELOJ, no el del que
    // matcheó. El encuadre (w/h del quad) sigue siendo el del set que matcheó
    // — los pasos de un titileo comparten huella; si no la comparten, el arte
    // de cada paso tiene que estar compuesto sobre esa misma huella.
    struct PlaneSequenceStep {
        uint64_t    set_id   = 0;    ///< Objeto (plane set) al que referencia
        const char* asset    = nullptr;  ///< HD de ESTE paso (nullptr = el del set)
        uint16_t    duration = 0;    ///< frames de juego que se sostiene (0 = default)
    };
    /// Declara una Animación. Menos de dos pasos se rechaza: un paso es el set
    /// solo, sin nada que ciclar.
    void define_plane_sequence(uint64_t id, const PlaneSequenceStep* steps,
                               uint32_t step_count);
    void undefine_plane_sequence(uint64_t id);
    void clear_plane_sequences();

    /// Modo HD del frontend (default ON). Gatea el matcher de sets: como
    /// suprime los tiles originales, con el HD apagado dejaría agujeros. El
    /// flag se lee POR PRODUCE, así que también vale para el re-render `bare`
    /// del compose y para export_frame. El Lab y el runtime lo escriben desde
    /// su propio toggle Original↔HD.
    void set_hd_enabled(bool on) noexcept;
    bool hd_enabled() const noexcept;

    // -- Routing original/HD POR SUBSISTEMA () ----------------------------
    //
    // `hd_enabled` es la llave de luz de toda la casa; esto son las llaves de
    // cada habitación. Es el punto único donde se decide qué sustitución se
    // aplica, y existe para lo que  (comparación original/AYTHER) y 
    // (perfiles) necesitan: apagar los sprites HD dejando la música, o al revés,
    // sin reiniciar nada.
    //
    // Se lee POR PRODUCE, igual que `hd_enabled`: el cambio se ve en el frame
    // siguiente y la vuelta al original es inmediata — no hay estado que
    // reconstruir, porque el original nunca se fue (los originales siguen
    // dibujándose; lo que se apaga es el reemplazo).
    //
    // Default: TODOS encendidos. Una sesión sin frontend que los maneje se
    // comporta como siempre.
    void set_subsystem_enabled(Subsystem s, bool on) noexcept;
    bool subsystem_enabled(Subsystem s) const noexcept;

    /// El estado completo como máscara (bit i = `Subsystem(i)` encendido), para
    /// serializarlo como configuración de sesión y reponerlo tal cual.
    uint32_t subsystems_enabled_mask() const noexcept;
    void     set_subsystems_enabled_mask(uint32_t mask) noexcept;

    /// ¿El pack cargado trae este subsistema? Ver `SubsystemAvailability`: el
    /// tercer estado (Unknown) es el que evita afirmar «no lo trae» sobre un
    /// pack que simplemente no lo declara.
    SubsystemAvailability subsystem_availability(Subsystem s) const noexcept;

    // -- Degradación segura ante errores de pack () ----------------------
    //
    // El fallback de  ya evita que un asset roto corte la sesión: se oye el
    // original y listo. Lo que falta acá es la ESCALADA — un pack con muchos
    // assets rotos reintenta cada uno, cada frame, y paga la resolución
    // completa por algo que ya se sabe que no va a andar.
    //
    // Se cuentan ASSETS DISTINTOS, no ocurrencias: un archivo roto que suena
    // mil veces es un problema; doce archivos distintos es un pack mal armado.

    /// Subsistemas que el MOTOR apagó solo por fallos repetidos (bit i =
    /// `Subsystem(i)`). Aparte de `subsystems_enabled_mask` a propósito: hay
    /// que poder distinguir «el usuario lo apagó» de «se apagó por fallos»,
    /// porque sólo del segundo hay algo que contarle al usuario.
    uint32_t auto_disabled_subsystems() const noexcept;

    /// Lo que hay que decirle al usuario, ya redactado. "" = nada que decir.
    ///
    /// Lo arma el Engine y no el frontend porque el que sabe QUÉ pasó es el
    /// que lo contó; el frontend decide DÓNDE y CUÁNDO mostrarlo. Nombra el
    /// pack: con los assets por hash (), sin eso no hay forma de volver al
    /// proyecto que lo horneó.
    std::string degradation_message() const;

    /// Volver a intentar: reactiva lo que se apagó solo y olvida los fallos.
    /// Lo pide el usuario (el panel de ) — el motor no reintenta por su
    /// cuenta, porque volver a probar lo mismo que ya falló doce veces es
    /// exactamente lo que la escalada existe para no hacer.
    void clear_auto_disabled() noexcept;

    // -- Perfiles de remasterización () ----------------------------------
    //
    // Un perfil es un PRESET CON NOMBRE de lo que  y  ya dejan togglear:
    // qué subsistemas se sustituyen y qué buses suenan. No multiplica el
    // material — filtra el que el pack ya trae. Sin eso, un pack con cuatro
    // perfiles pesaría cuatro veces, que es la «complejidad combinatoria de
    // assets por perfil» que la issue anota como riesgo.
    //
    // La lista viene del pack y siempre trae «original» primero.

    /// Cuántos perfiles ofrece el pack cargado. 0 sin pack.
    uint32_t profile_count() const noexcept;
    /// Id / nombre del perfil `i`. Cadena vacía fuera de rango.
    std::string profile_id(uint32_t i) const;
    std::string profile_name(uint32_t i) const;

    /// Aplica el perfil `id`: setea la máscara de subsistemas y los mutes de
    /// bus de una sola vez. `false` = ese perfil no existe en este pack, y ahí
    /// NO se toca nada — aplicar «lo más parecido» dejaría al usuario viendo
    /// algo que no pidió sin que nada lo diga.
    bool set_profile(const std::string& id);

    /// El perfil ACTIVO, o "" si el estado actual no es ninguno.
    ///
    /// Vacío es un resultado legítimo y es la mitad del contrato: apenas
    /// alguien toca un toggle suelto (/), el estado deja de ser el que
    /// el perfil describe. Seguir diciendo «enhanced» mentiría sobre lo que se
    /// está viendo — el perfil «custom» del alcance de la issue no se declara,
    /// se ALCANZA, y esto es cómo se detecta.
    std::string active_profile() const;

    /// El perfil que el pack aplica al cargarse sin pedir otro. Lo llama
    /// `set_pack` solo; está expuesto para que un frontend pueda ofrecer
    /// «volver al predeterminado».
    bool apply_default_profile();

    // -- Validación de packs () -------------------------------------------
    //
    // ¿Este pack puede correr con ESTA sesión? Se contesta ANTES de cargarlo y
    // sin abrirlo, así que un pack de otro juego o de un formato más nuevo no
    // puede tirar nada: no se llega a abrir.
    //
    // Devuelve una lista y no un sí/no porque son dos cosas distintas: una
    // incompatibilidad crítica (otro juego, un Engine que no existe todavía) y
    // una degradación opcional (un subsistema que este build no conoce). Con un
    // booleano, o se rechaza lo segundo o se acepta lo primero.
    struct PackFinding {
        bool        error = false;   ///< false = advertencia
        std::string code;            ///< estable, para decidir sin leer el texto
        std::string message;         ///< para el usuario
    };
    /// Contexto tomado de la sesión (ROM cargada, plataforma, core) más lo que
    /// el caller quiera precisar. Lo que no se sabe se reporta como no
    /// verificado en vez de darse por bueno.
    std::vector<PackFinding> validate_pack(const std::string& pack_path) const;

    // -- Análisis de nivel de un asset de audio () ------------------------
    //
    // Lo que un autor necesita ANTES de publicar: si el asset clipea, si se va a
    // perder debajo del juego, y cuánto habría que corregirlo. Se mide sobre el
    // PCM que va a la mezcla (no sobre los bytes del contenedor: un OGG de
    // 22 kHz mono suena distinto de lo que dice su cabecera) y NUNCA se toca el
    // archivo — en AYTHER toda corrección es ganancia de reproducción.
    //
    // Cacheado por ruta y invalidado por el hot-reload, así que el panel lo
    // puede pedir por frame.
    using AssetLevel = AudioAssetLevel;   // audio_asset_level.h
    const AssetLevel& audio_asset_level(const std::string& abs_path) const;

    /// : la ENVOLVENTE del asset — `bins` pares (min, max) intercalados en
    /// -1..1, para dibujar la forma de onda. Vacío = no se pudo decodificar.
    ///
    /// Min y max y no un valor absoluto: dibujada sólo con el máximo, una forma
    /// de onda no muestra la asimetría, y ahí es donde se ve el offset de DC y
    /// el recorte de un solo lado.
    const std::vector<float>& audio_asset_waveform(const std::string& abs_path,
                                                   uint32_t bins) const;

    // -- Buses lógicos de audio () ----------------------------------------
    //
    // Volumen y silencio POR CATEGORÍA. El bus de un sonido lo trae el «Tipo»
    // de su Secuencia; una asignación por firma suelta cae en **Efectos**.
    //
    // La diferencia con el routing de , que es fácil de confundir:
    //
    //   · apagar el SUBSISTEMA Música  = «no sustituyas la música» → suena la
    //     música ORIGINAL del juego;
    //   · silenciar el BUS Música      = «no quiero música» → no suena ni el HD
    //     ni el original.
    //
    // Son intenciones distintas y por eso son controles distintos.
    //
    // El volumen escala el HD. Bajar a medias el ORIGINAL exige una fuente con
    // ganancia en el router de voces (hoy sólo sabe copiar o callar), así que
    // por ahora el original responde al MUTE del bus y no al volumen — está
    // dicho acá para que nadie lo descubra escuchando.
    void  set_bus_volume(AudioBus bus, float gain) noexcept;
    float bus_volume(AudioBus bus) const noexcept;
    void  set_bus_muted(AudioBus bus, bool muted) noexcept;
    bool  bus_muted(AudioBus bus) const noexcept;

    // -- CUADRO (CU001): pantalla estática completa → un asset HD -------------
    // Una celda del Cuadro es su posición ABSOLUTA en la grilla de pantalla
    // (columna/fila de 8 px, como `screen_plane_sig`) más el hash del tile: un
    // Cuadro no scrollea, así que la posición ES parte de la identidad.
    struct ScreenCell { uint64_t hash; uint8_t plane; uint8_t col, row; };

    /// Declara un Cuadro. El reconocimiento es por COBERTURA, no por igualdad:
    ///   `min_match` — fracción MÍNIMA de las celdas declaradas que tiene que
    ///                 estar presente en su posición exacta (0.92 razonable).
    ///                 Sin tolerancia, una sola celda animada —una llama que
    ///                 parpadea, un «PRESS START» que titila— tiraría el Cuadro
    ///                 entero.
    ///   `max_extra` — celdas del frame (dentro de la máscara) que NO están en
    ///                 el Cuadro, como fracción de las declaradas. Es el guarda
    ///                 imprescindible: sin él, una pantalla que es SUPERCONJUNTO
    ///                 (un menú dibujado encima del título) matchearía el título
    ///                 con cobertura perfecta.
    /// El asset se dibuja a PANTALLA COMPLETA en su propia lane, debajo de los
    /// overlays de plano — así una Utilería o un glifo autorado sobre esa misma
    /// pantalla se sigue viendo encima. Como es opaco y cubre todo, no hace
    /// falta suprimir nada.
    void define_screen(uint64_t id, uint8_t plane_mask,
                       const ScreenCell* cells, uint32_t cell_count,
                       float min_match, float max_extra,
                       const std::string& asset_path);
    void undefine_screen(uint64_t id);
    void clear_screens();

    // -- CINEMÁTICA (CU004): secuencia ordenada de Cuadros ----------------------
    // Lo que agrega sobre Cuadros sueltos NO es el dibujo —eso ya lo hace el
    // Cuadro— sino el ORDEN: desambigua dos pantallas idénticas que aparecen en
    // cinemáticas distintas, y da la semántica de cancelación del spec (si el
    // jugador aprieta Start y el juego salta a un menú, la secuencia se corta y
    // se re-evalúa la pantalla nueva, sin esperar nada).
    //
    // Se puede ENTRAR en cualquier paso, no sólo el primero: la posición sale
    // del CONTENIDO de la pantalla, así que un scrub al medio de la cinemática
    // cae donde corresponde en vez de tener que reproducir desde el principio.
    //
    // `gap_frames` es la tolerancia de frames sin Cuadro confirmado antes de
    // cancelar. NO puede ser 0: `screen_match_id` cae a 0 durante un frame en
    // TODA transición limpia de Cuadro (la histéresis exige dos frames para
    // confirmar el siguiente), y varios más si hay un fundido de por medio.
    /// Un paso: el Cuadro que se espera, y opcionalmente el asset que lo
    /// reemplaza cuando la secuencia va por acá (vacío = usa el del Cuadro).
    /// `video_offset` (): en qué frame del clip arranca ESTE paso, cuando el
    /// asset es un `.ivf`. Es lo que hace que un video cubra varios pasos y que
    /// entrar por el medio de la secuencia caiga en el plano correcto — la
    /// posición sale del CONTENIDO (qué paso matcheó) más este offset, no de un
    /// contador. Ignorado si el asset no es video.
    struct KinematicStep {
        uint64_t    screen_id;
        const char* asset;
        uint32_t    video_offset = 0;
    };
    /// Ajustes del MEDIO de la Cinemática — lo que no es la secuencia de pasos.
    /// Van juntos en un struct y no como parámetros sueltos porque el medio va
    /// a seguir creciendo (volumen, ducking…) y cada campo nuevo sería otra
    /// firma rota en los tres llamadores.
    struct KinematicMedia {
        /// El video se REPITE si es más corto que el tramo, en vez de sostener
        /// su último frame. Es una decisión autoral, no un default: un clip de
        /// fondo (lluvia, fuego) quiere ciclar y una escena narrada no —
        /// rebobinarla a mitad de camino sería un defecto. Por eso el motor
        /// sostiene salvo que se pida lo contrario.
        bool        loop  = false;
        /// Pista de AUDIO del video, como asset aparte (el IVF es sólo video).
        /// nullptr/"" = la Cinemática suena con el audio del juego.
        const char* audio = nullptr;
        /// Volumen de la pista de la Cinemática (1 = original).
        float       gain = 1.0f;
        /// Volumen de la BANDA SONORA del juego MIENTRAS corre la Cinemática
        /// (1 = intacta, 0 = muda). Es un ducking con lifetime propio: se
        /// aplica al entrar y se devuelve a 1 al salir. Sin esto, una escena
        /// narrada sonaba encima de la música del juego.
        float       game_gain = 1.0f;
    };
    void define_kinematic(uint64_t id, const KinematicStep* steps, uint32_t step_count,
                          uint32_t gap_frames = 12,
                          const KinematicMedia* media = nullptr);
    void undefine_kinematic(uint64_t id);
    void clear_kinematics();

    // -- PANORÁMICA (CU003): la tira del nivel de una capa → textura HD ------
    // Una celda de la tira es su posición en espacio de NIVEL (celdas de 8 px,
    // el mismo espacio que reconstruye el stitcher) más el hash del tile.
    struct PanoramaCell { uint64_t hash; int32_t lx, ly; };

    /// Declara una Panorámica: la tira reconstruida de UNA capa.
    ///
    /// El ANCLAJE es por CONTENIDO, no por la cámara acumulada
    /// (`plane_cam_*`): esa es *relativa* — se re-ancla en 0 ante cualquier
    /// seek/scrub, y en el Lab el artista scrubbea todo el tiempo, así que la
    /// panorámica quedaría en el offset equivocado hasta reproducir hacia
    /// adelante. En cambio, cada celda visible cuyo hash está en la tira dice
    /// dónde está la cámara (`cam_px = lx*8 - screen_x`); la moda de esos votos
    /// la fija exacta, y eso vale igual tras un seek, una carga de savestate o
    /// un corte de escena.
    ///
    /// Sólo votan los hashes RAROS de la tira (≤ `kPanoramaRare` apariciones):
    /// misma idea que el ancla menos frecuente del matcher de sets — el tile de
    /// cielo que aparece 500 veces no aporta información y sí costo. La rareza
    /// se calcula acá, al declarar, no por frame.
    void define_panorama(uint64_t id, uint8_t plane,
                         int32_t origin_x, int32_t origin_y,
                         uint16_t w_cells, uint16_t h_cells,
                         const PanoramaCell* cells, uint32_t cell_count,
                         const std::string& asset_path);
    void undefine_panorama(uint64_t id);
    void clear_panoramas();
    /// Apariciones máximas en la tira para que un hash sirva de ancla.
    static constexpr uint32_t kPanoramaRare = 8;
    /// Fracción MÍNIMA de los votantes que tiene que respaldar al ganador para
    /// declarar la cámara anclada. Un hash raro puede repetirse en otro tramo
    /// del nivel, así que una mayoría flaca es un anclaje dudoso: sin este
    /// piso se publicaba como válido igual y el renderer dibujaba la tira en el
    /// lugar equivocado. Por debajo, `panorama_valid` queda en false — «no sé
    /// dónde estoy» es una respuesta útil; una posición inventada, no.
    static constexpr uint32_t kPanoramaMinVotePct = 50;

    // -- Animaciones C-S2 (Componentes): playback HD en fase ---------------------
    // Una "Acción" (clip = anim_group_id) se dibuja en HD sincronizada al juego:
    // por la pose que el juego muestra este frame, el frame HD del sheet en el
    // bbox del metasprite. Nivel 0 (Pop) o Nivel 1 (tween geométrico del
    // transform entre keyframes — glide en vez de pop, sin arte extra).
    /// Define (o reemplaza) la animación HD de un clip: sheet + mapa pose→frame
    /// + nivel de tween. El tab Animaciones (C-S3) la autora; Deliver hornea
    /// `animations.toml`. Resultados por frame en FrameView.anim_frames.
    void   define_animation(uint64_t clip_id, const std::string& sheet_asset,
                            const HdPose* poses, uint32_t pose_count, int tween_level);
    void   undefine_animation(uint64_t clip_id);
    void   clear_animations();
    size_t animation_count() const noexcept;
    /// Definiciones vigentes (autoradas + cargadas del pack), ordenadas por
    /// clip_id — el Deliver las hornea en `animations.toml` (C-S4).
    std::vector<AnimationDef> animation_definitions() const;

    // -- Audios C-A2 (Componentes): sustitución HD por EVENTO --------------------
    // Un sonido con ataque y cola (jingle/voz/música) abarca muchos batches de
    // hash cambiante → se sustituye como EVENTO entero: rango-mute del emulador
    // en [start,end] + asset HD alineado al start_frame. Los frames de los
    // eventos son frames DE LA TOMA → aplican durante su replay; en vivo no
    // matchean (no-op). Límite C-A1: un sonido DISTINTO solapado en la mezcla
    // no se separa (hashes de batch opacos).
    /// Corre el detector de eventos sobre el historial de audio de la toma
    /// (.arp v7, CSR por frame) y resuelve las ventanas de sustitución con las
    /// asignaciones vigentes. Llamar al cargar/cambiar la toma. Devuelve la
    /// cantidad de eventos detectados (también los deja en audio_events()).
    size_t resolve_audio_events(const AytherRecording& rec);
    /// Eventos detectados por el último resolve_audio_events (para que el Lab
    /// liste/asigne). Borrados al re-resolver.
    const AytherAudioEvent* audio_events(size_t* count) const noexcept;
    /// Asigna un asset HD (WAV/OGG/FLAC del pack) a una FIRMA de evento; ""
    /// desasigna. Persiste entre tomas; re-resuelve las ventanas al instante.
    void assign_audio_event(uint64_t signature, const std::string& asset, bool looping);
    std::vector<AudioEventAssignment> audio_event_assignments() const;
    /// Ventanas resueltas (evento asignado → rango + asset), para la UI del
    /// timeline. Válidas hasta el próximo resolve/assign.
    const AytherAudioEventSub* audio_event_subs(size_t* count) const noexcept;

    // -- Fondos (Componentes): stitcher de planos + export por capa -------------
    // Mientras la captura está ON, produce_frame acumula las celdas visibles de
    // los planos A/B en ESPACIO DE NIVEL (BackgroundStitcher + unwrap del scroll
    // del VDP — el camino validado por tools/background_spike). La captura espera
    // frames SECUENCIALES (pasada lineal de una toma o juego en vivo): un salto
    // de scrub rompe el unwrap. Encender de nuevo arranca una captura fresca.
    void   bg_capture(bool on);
    bool   bg_capturing() const noexcept;
    /// Celdas de nivel acumuladas del plano (0=A · 1=B). 0 sin captura.
    size_t bg_cell_count(uint8_t plane) const noexcept;
    /// Exporta cada plano capturado con celdas a un PNG por capa
    /// (`bg_<A|B|W>_t<ox>x<oy>_<hash>.png`, índice de re-import en el nombre)
    /// en `out_dir`. Devuelve los paths escritos ({} si no hay nada capturado).
    Result<std::vector<std::string>> export_backgrounds(const std::string& out_dir);
    /// Exporta SOLO el plano `plane` (0=A · 1=B · 2=W) del stitcher a `path`
    /// (archivo PNG exacto, el caller elige nombre y carpeta). Error si no hay
    /// captura activa, el plano no acumuló celdas o falla la escritura — a
    /// diferencia de export_backgrounds, acá el fallo NO es silencioso.
    Result<void> export_background_plane(uint8_t plane, const std::string& path);
    /// Bounds del stitcher para `plane` en CELDAS de nivel: out = {min_x, min_y,
    /// max_x, max_y}. false sin captura o plano vacío. : la definición de
    /// una Panorámica DEBE usar estos bounds (los del PNG exportado) — los
    /// extents de bg_cells() pueden diferir en una fila/columna (celdas de
    /// borde parciales) y ese desfase estira y corre el asset al dibujar.
    bool bg_bounds(uint8_t plane, int32_t out[4]) const;
    /// Celdas acumuladas del plano como CELDAS DE PANORÁMICA: (hash, lx, ly),
    /// listas para `define_panorama`. Distinto de lo que exporta el stitcher a
    /// PNG: eso son códigos de nametable (para re-dibujar la tira), esto es la
    /// identidad por CONTENIDO con la que la Panorámica se reconoce en runtime.
    /// Orden estable (por fila, luego columna) para que el TOML horneado sea
    /// reproducible entre barridos.
    std::vector<PanoramaCell> bg_cells(uint8_t plane) const;

    // -- Modo 3 (RAM anchoring) -------------------------------------------------
    // Sustitución de metasprite POR INSTANCIA: el perfil TOML declara dónde viven
    // las entidades en la work RAM (game_profile.rs); cada frame produce_frame lee
    // sus world_pos, las proyecta con la cámara del VDP (camino validado por
    // tools/mode3_spike) y asigna los sprites SAT a cada instancia. Resultados en
    // FrameView.entity_subs / entity_instances.
    /// Carga (o reemplaza) el perfil de anclas; "" lo descarga (Modo 3 no-op).
    Result<void> load_game_profile(const std::string& toml_path);
    bool         has_game_profile() const noexcept;
    /// Asset HD para TODA instancia del kind `kind_name` del perfil (cada una en
    /// su propio bbox). Persiste entre frames; "" desasigna ese kind.
    void         assign_kind(const std::string& kind_name, const std::string& asset_path);
    // -- Animation clips (C-S1) ------------------------------------------------
    // Looping cycles the sprite hasher detected from the SAT slot histories,
    // consolidated into an ordered pose sequence + per-frame duration (the §4
    // "phase"). The Animación workspace reads these as a starting point for
    // authoring. Accumulated state (not per-frame); empty until warm-up.
    struct AnimClipFrame { uint64_t pose; uint16_t duration; };
    struct AnimationClip {
        uint64_t                  id;       // = anim_group_id (stable, matches occs)
        bool                      looping;
        std::vector<AnimClipFrame> frames;  // ordered (canonical: starts at min pose)
    };
    size_t                     animation_clip_count() const noexcept;
    std::vector<AnimationClip> animation_clips() const;
    /// Reset the animation detector (clear history/groups/clips). Call before a clip
    /// generation run so the result reflects only the recording scanned (C-S5).
    void                       reset_animation_detection() noexcept;

    // -- : ACETATOS que trae el pack ---------------------------------------
    // Las capas Custom con contenido propio que el artista compuso en el
    // workspace Componer. El pack las lleva desde  (antes morían en el Lab:
    // se persistían en el proyecto y el bake no las escribía).
    //
    // La sesión las LEE y las OFRECE; no arma el stack. El stack de capas es
    // estado del FRONTEND —el Lab lo posee, el renderer lo recibe por
    // parámetro— así que decidir cuándo y con qué orden construirlo es del que
    // dibuja: Aether Play arma el suyo al abrir el pack, el Lab ya tiene el del
    // proyecto. Poner el stack acá adentro sería moverle esa decisión al motor.
    //
    // `content.asset` es el NOMBRE DE ENTRADA del pack (id de contenido), no una
    // ruta del disco — se resuelve contra el pack como cualquier otro asset.
    // Vacío si el pack no trae acetatos.toml (packs previos a ).
    struct PackOverlay {
        std::string         name;
        bool                visible = true;
        AytherLayerContent  content;
    };
    const std::vector<PackOverlay>& pack_overlays() const noexcept;

    // -- E-2 (): suscripciones de la ABI AYTHER v1 -------------------------
    // Diagnóstico del contrato con el core: qué se pidió, qué quedó ACTIVO y
    // qué soporta el binario. Con un core sin ABI los tres son 0 — que es la
    // forma de distinguir «no hay ABI» de «hay ABI y no activó nada», dos
    // situaciones que desde afuera se ven igual (nada se observa) y se
    // arreglan distinto.
    //
    // `active` sólo tiene sentido DESPUÉS del primer frame: las suscripciones
    // entran en el frame boundary del core.
    void ayther_subscriptions(uint32_t* requested, uint32_t* active,
                              uint32_t* supported) const noexcept;
    /// True si el core cargado negoció la ABI v1 (gate de los caminos nuevos).
    bool has_ayther_abi() const noexcept;

    /// La ABI negociada (`AYTHER_ABI_VERSION_MAJOR/MINOR`; 0 sin ABI) y el
    /// `build_id` que declara el core ("" sin ABI). Es lo que un frontend
    /// muestra para saber CONTRA QUÉ está grabando: una toma replicada con otro
    /// core puede divergir, y sin esto no hay forma de decir cuál era.
    uint32_t    ayther_abi_version() const noexcept;
    const char* ayther_build_id()   const noexcept;

    /// `SYSTEM` (ABI 1.5, guía 1.9 §5.1): lo que el core dice del contenido —
    /// modo del VDP (4/5, 0 mientras no eligió), h40, interlace, S/H, región y
    /// el viewport del frame emitido con su offset (Game Gear: 160×144 en
    /// (48,24)). Se refresca por frame; `ok` = el core lo da.
    ///
    /// ABI 1.10: `viewport_w/h`, `interlace` y `h40` describen el frame EMITIDO
    /// (h40 ya no puede contradecir al viewport); `vdp_mode` y `shadow_highlight`
    /// salen de los registros. `geometry_pending` = los registros ya cambiaron
    /// la geometría y el frame que viene la aplica: quien quiera la geometría
    /// «definitiva» espera a que se apague; quien dibuja el frame recibido usa
    /// el viewport tal cual. Con un core < 1.10 es siempre false.
    struct SystemInfo {
        bool     ok = false;
        uint8_t  system_hw = 0, region_pal = 0, vdp_mode = 0, interlace = 0;
        uint8_t  h40 = 0, shadow_highlight = 0;
        uint16_t lines_per_frame = 0;
        uint16_t viewport_x = 0, viewport_y = 0, viewport_w = 0, viewport_h = 0;
        bool     geometry_pending = false;
    };
    SystemInfo system_info() const noexcept;

    // -- Read-only introspection (identity / pacing for the frontend) ----------
    const char* game_id()    const noexcept;   ///< pack-reported game id ("" if no pack)
    double      timing_fps() const noexcept;   ///< core timing fps (drives frontend pacing)
    ///  (telemetria de audio): frames de flush con backlog < 1/4 del target
    /// (starvation acumulada) y el ratio DRC vigente. Para diagnosticar la
    /// degradacion de ENTREGA con HD activo (tool MCP audio_health del Lab).
    uint64_t    audio_starved_frames() const noexcept;
    float       audio_drc_ratio() const noexcept;
    float       audio_backlog_avg() const noexcept;   ///< frames (EMA)

    /// Por qué NO sonó. `audio_starved_*` mide la ENTREGA —si el PCM llega
    /// tarde— pero no dice nada cuando el PCM ni siquiera se manda, que es el
    /// caso de «no se escucha nada». Hay cuatro caminos al silencio y desde
    /// afuera se ven idénticos: la salida está inaudible (produce interno, o
    /// replay en pausa), el motor está en warm/bake, el audio está deshabilitado
    /// en la config, o simplemente está mudo. Contarlos por separado convierte
    /// el síntoma en una respuesta.
    /// `flushed` = frames en que el PCM SÍ salió al device.
    void audio_gate_counts(uint64_t* flushed, uint64_t* inaudible,
                           uint64_t* quiet, uint64_t* disabled) const noexcept;
    ///  (telemetría de la pausa): cuadros estéreo DESCARTADOS por los
    /// cortes de transporte (staging + backlog del emulador + synth) y cuántos
    /// cortes descartaron algo. Verifica que pausar corta de verdad — un corte
    /// con 0 frames descartados y sonido audible es un stream fuera del corte.
    void audio_pause_stats(uint64_t* cut_frames,
                           uint64_t* cuts) const noexcept;
    /// El flag de salida audible vigente (lo escribe el frontend por frame).
    bool audio_audible() const noexcept;

    // -- Borrowed motor resources the frontend reads (valid while owned) -------
    // The active HD pack: the frontend reads asset bytes to upload HD textures.
    // Non-owning — invalidated by set_pack()/reload_pack(). Null if no pack.
    AyArchive*     pack()          const noexcept;
    // Emulator work RAM (read-only) for inspection overlays (e.g. Sonic XY).
    const uint8_t* work_ram()      const noexcept;
    size_t         work_ram_size() const noexcept;
    // Export the tile hasher's catalog to a TOML file (authoring convenience).
    void           dump_tile_catalog(const char* path) const;

private:
    AytherSession();                 ///< use create()
    const FrameView& produce_frame();///< run+build a frame (shared by step/rewind)
    void           replay_capture_key(uint32_t key);  ///< stash a replay keyframe (R7d)
    /// R7e: mejor arranque para un seek a `target` (el mayor de: estado inicial,
    /// keyframe runtime, keyframe horneado del .arp). Descomprime el horneado a
    /// kf_scratch si gana; deja `state` apuntando al estado crudo. Devuelve el frame.
    uint32_t       replay_start(const AytherRecording& rec, uint32_t target,
                                const std::vector<uint8_t>*& state);
    /// Sólo el frame de arranque (sin descomprimir) — para replay_seek_cost.
    uint32_t       replay_start_frame(const AytherRecording& rec, uint32_t target) const;
    /// Captura la MEZCLA de una ventana corta a partir del frame `f` (re-sim desde
    /// el mejor keyframe) y la reproduce one-shot SIN mover el playhead. Helper
    /// común de preview_audio (ubica `f` por hash grabado) y preview_audio_at (usa
    /// el playhead directo). Devuelve los cuadros estéreo capturados.
    size_t         capture_audio_window(const AytherRecording& rec, uint32_t f);
    /// Núcleo de captura: re-sim `win` cuadros desde `f` acumulando la MEZCLA en
    /// cap_pcm, restaurando el playhead. Reproduce one-shot si `play`. Compartido
    /// por capture_audio_window (preview) y export_audio_event_wav (handoff).
    /// `mute_mask` (opcional): canales silenciados DURANTE la ventana capturada
    /// (bits 0-5 FM · 6-9 PSG) — capture_channel_pcm aísla un canal con esto.
    /// `max_samples` (opcional): tope del buffer capturado (0 = default ~10 s;
    /// capture_channel_pcm lo sube para cubrir la toma completa).
    /// `member_sigs` (opcional): aislamiento DINÁMICO por evento — cada frame de
    /// la ventana deja sonar solo los canales con un evento de esas firmas
    /// activo (requiere analyze_audio_events; sin análisis cae a `mute_mask`).
    /// `dynamic_mute`: aplica POR FRAME la máscara dinámica del playback
    /// (dynamic_audio_mute_at — subs por evento/Secuencia + instrumento +
    /// ocurrencia + manual) — el «original muteado» del mixdown del export MP4.
    size_t         capture_pcm_span(const AytherRecording& rec, uint32_t f,
                                    uint32_t win, bool play, uint32_t mute_mask = 0,
                                    size_t max_samples = 0,
                                    const std::vector<uint64_t>* member_sigs = nullptr,
                                    bool dynamic_mute = false);
    struct Impl;                     ///< pimpl: owns the unique_handle<>s + runner + audio
    std::unique_ptr<Impl> impl_;
};

}  // namespace ayther

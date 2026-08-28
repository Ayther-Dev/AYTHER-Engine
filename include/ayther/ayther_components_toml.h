#pragma once
// ---------------------------------------------------------------------------
// ayther_components_toml.h — round-trip TOML de la capa de Componentes:
// `animations.toml` (C-S4) y `audio_events.toml` (C-A4).
//
// El horneo (bake_*) lo llama el Deliver del Lab al construir el pack; el
// parseo (parse_*) lo llama AytherSession al cargar un pack (load_pack_into),
// re-poblando el AnimationPlayer / el mirror de asignaciones por evento. Ambos
// lados viven en el ENGINE (funciones libres, sin UI ni Vulkan) para que el
// round-trip sea testeable headless con strings.
//
// Formatos:
//   animations.toml (engine-owned):
//     [[animation]]
//     clip  = "0x<16hex>"          # handle de autoría (clip id)
//     sheet = "sheets/run.png"
//     tween = 1                     # 0 Pop · 1 tween geométrico
//     [[animation.pose]]
//     pose   = "0x<16hex>"          # hash de la pose (identidad estable)
//     src    = [x, y, w, h]         # sub-rect del sheet (px)
//     anchor = [x, y, w, h]         # keyframe dst (Nivel 1)
//     ticks  = 6
//
//   audio_events.toml — MISMO esquema que parsea el core Rust
//   (AudioSubstitutor::parse_events_toml, cargado por load_from_pack):
//     [[event]]
//     signature = "0x<16hex>"
//     asset     = "audio/music/zone1.ogg"
//     loop      = true              # opcional (default false)
//
//   plane_sets.toml — Utilería (CU002) y Glifos (CU005): sustitución HD por
//   ELEMENTO multi-tile de plano. Hasta acá el catálogo de Pintar sólo existía
//   en la sesión de autoría (inyectado por API), así que el `.ay` entregado NO
//   reproducía ninguna sustitución multi-tile; este archivo cierra ese hueco.
//     [[font]]
//     id = "0x<16hex>" · name = "HUD" · cell_w = 1 · cell_h = 2
//     [[set]]
//     id      = "0x<16hex>"        # pintar_element_id (determinista por captura)
//     name    = "Cofre"            # informativo (overlay/debug)
//     type    = "utileria"         # utileria | glifo
//     plane   = 0                   # 0=A · 1=B · 2=Window
//     w_cells = 3 · h_cells = 2     # bbox
//     asset   = "cofre.png"         # basename (el bake lo rutea al tier)
//     tiles   = "0x<hash>:cx,cy|…"  # miembros con offset RELATIVO en celdas
//     font    = "0x<16hex>" · ch = "A"    # sólo type="glifo"
//
//   Los FLIPS observados al capturar NO se hornean a propósito: el hash de
//   tile de plano es flip-invariante y el matcher no los exige (una utilería
//   espejada matchea igual). Viven sólo en pintar_elements.toml, que los usa
//   para el export fiel del PNG base.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ayther_animation.h"      // AnimationDef / AnimationPlayer
#include "ayther_audio_events.h"   // AudioEventAssignment / AudioEventSubstitution

namespace ayther {

// -- Utilería / Glifos: sets de plano en el pack (CU002 · CU005) --------------

/// Miembro de un set: hash del tile de plano + offset RELATIVO en CELDAS al
/// top-left del conjunto. Espeja `AytherSession::PlaneSetMember` (sin flips: el
/// hash es flip-invariante y el matcher no los exige).
struct PackPlaneSetMember {
    uint64_t hash = 0;
    int16_t  cx = 0, cy = 0;
};

/// Un elemento multi-tile del catálogo de Pintar, tal como viaja en el pack.
struct PackPlaneSet {
    uint64_t    id      = 0;      ///< pintar_element_id (determinista por captura)
    std::string name;             ///< informativo (overlay / debug)
    std::string type;             ///< "utileria" | "glifo"
    uint8_t     plane   = 0;      ///< 0=A · 1=B · 2=Window
    uint16_t    w_cells = 0, h_cells = 0;
    std::string asset;            ///< basename dentro del pack
    std::vector<PackPlaneSetMember> members;
    uint64_t    font_id = 0;      ///< sólo glifo
    std::string ch;               ///< sólo glifo: code point UTF-8
    /// : offset de RE-ANCLAJE del HUD, en píxeles. Cuando la pantalla se
    /// ensancha (16:9) el juego sigue dibujando su interfaz en coordenadas de
    /// la pantalla original, así que un marcador pegado al borde izquierdo
    /// queda flotando hacia el centro. El autor mira, ve qué Objeto quedó
    /// fuera del área segura, y carga acá los valores que lo devuelven adentro.
    /// (0,0) = no se toca, que es el caso de todo Objeto que ya cae bien.
    int16_t     off_x = 0, off_y = 0;
    /// Referencia del tinte E1 (promedio RGB 0-255 de la línea CRAM del
    /// elemento al capturarlo, mismo dialecto `ref = "r,g,b"` que las poses).
    /// {0,0,0} = sin tinte → se omite al hornear (byte-idéntico al previo).
    uint8_t     ref_rgb[3] = { 0, 0, 0 };
};

/// Fuente del juego: agrupador de glifos, sin bitmap propio.
struct PackPlaneFont {
    uint64_t    id = 0;
    std::string name;
    uint16_t    cell_w = 1, cell_h = 1;
};

/// Tope de miembros por set. Era 64 con la teoría de que «cientos de celdas =
/// un Cuadro» — el isologotipo del título de Golden Axe la refutó (): se
/// TRASLADA (sube 1 px/frame), así que el Cuadro no lo puede matchear y tiene
/// que ser un Objeto de 277 celdas... que el bake salteaba EN SILENCIO. 512 le
/// da aire; el costo del matcher (O(miembros × apariciones), todos presentes)
/// sigue acotado porque las apariciones ancladas de un set grande son pocas.
/// Lo que ya no es silencioso: superarlo es un hallazgo del lint ().
inline constexpr size_t kMaxPlaneSetMembers = 512;

// -- Horneo (Deliver → pack) --------------------------------------------------
std::string bake_animations_toml(const std::vector<AnimationDef>& defs);
std::string bake_audio_events_toml(const std::vector<AudioEventAssignment>& assigns);
/// Sets con más de `kMaxPlaneSetMembers` miembros, sin miembros o sin asset se
/// OMITEN (el caller reporta). Las fuentes sin glifos se emiten igual: son
/// baratas y sirven al charmap.
std::string bake_plane_sets_toml(const std::vector<PackPlaneSet>& sets,
                                 const std::vector<PackPlaneFont>& fonts);

// -- Parseo (pack → sesión) ---------------------------------------------------
/// Define en `into` cada animación del TOML (define() reemplaza por clip_id).
/// Devuelve la cantidad de animaciones cargadas.
size_t parse_animations_toml(const std::string& text, AnimationPlayer& into);
/// Asigna en `into` cada evento del TOML (mirror de autoría del engine; el
/// core Rust parsea el mismo archivo para su catálogo). Devuelve la cantidad.
size_t parse_audio_events_toml(const std::string& text, AudioEventSubstitution& into);

/// : firmas MIEMBRO por Secuencia de audio_events.toml — `members` de cada
/// [[event]] (firma disparadora → lista de firmas). El runtime las usa para el
/// mute SELECTIVO dentro de la ventana (solo los eventos activos de esas
/// firmas, no el canal completo). Packs viejos sin `members` → mapa vacío y el
/// runtime cae al range-mute de `channels`.
std::unordered_map<uint64_t, std::vector<uint64_t>>
parse_audio_event_members(const std::string& text);
/// : lista de firmas `field` por [[event]] (`members`, `head`). La
/// CABEZA = las firmas que arrancan junto al disparador: el runtime ancla
/// la Secuencia por mayoría de la cabeza aunque el disparador sea una
/// variante (ver audio_seq_anchor.h).
std::unordered_map<uint64_t, std::vector<uint64_t>>
parse_audio_event_sig_list(const std::string& text, const char* field);

/// : `tail_frames` por [[event]] de audio_events.toml — cuántos frames
/// puede seguir sonando el HD DESPUÉS de end_frame (0 = corta exacto en el
/// límite). AUSENTE = ilimitado (packs legacy: el non-loop drenaba entero y
/// cambiarles el sonido al migrar sería una regresión audible) — por eso el
/// mapa solo trae las firmas que declaran el campo, y el writer de Entregar
/// lo escribe SIEMPRE para que el contrato quede explícito al re-hornear.
/// Mismo transporte que `members`: el parser Rust del catálogo lo ignora.
std::unordered_map<uint64_t, uint32_t>
parse_audio_event_tails(const std::string& text);

/// : ganancia por firma. Ausente = 1.0 (neutro), que es lo que el mixer
/// aplicaba fijo antes de que el dato viajara.
std::unordered_map<uint64_t, float>
parse_audio_event_gains(const std::string& text);

/// : region de loop por firma, en CUADROS del asset. Ausente = el asset
/// entero. Un rango invalido (fin <= inicio) se descarta al parsear.
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>>
parse_audio_event_loops(const std::string& text);

/// : `fade_frames` por [[event]] — cuántos frames dura el DESVANECIMIENTO
/// después de end_frame (ausente / 0 = sin fade: manda la política de ).
/// Es ALTERNATIVA a `tail_frames`, no acumulable: con fade, el corte no
/// interrumpe la rampa. Mismo transporte que `tail`: sólo entran al mapa las
/// firmas que lo declaran, y el parser Rust del catálogo lo ignora.
std::unordered_map<uint64_t, uint32_t>
parse_audio_event_fades(const std::string& text);

/// Un CUADRO tal como viaja en el pack: la pantalla entera de las capas que lo
/// componen. La celda lleva su posición ABSOLUTA en la grilla de pantalla (un
/// Cuadro no scrollea) y el plano del que vino.
struct PackScreenCell { uint64_t hash; uint8_t plane, col, row; };
struct PackScreen {
    uint64_t    id = 0;
    std::string name;
    uint8_t     plane_mask = 0x07;
    float       min_match = 0.92f, max_extra = 0.08f;
    std::string asset;
    std::vector<PackScreenCell> cells;
};

/// `screens.toml` — un [[screen]] por Cuadro. Las celdas van en un pipe-list
/// `hash:plano,col,fila`, mismo dialecto que el resto del pack.
std::string bake_screens_toml(const std::vector<PackScreen>& screens);
size_t parse_screens_toml(const std::string& text, std::vector<PackScreen>& out);

/// -- PANORÁMICA (CU003) ------------------------------------------------------
/// La tira del nivel de una capa. Tiene archivo propio y no entra ni en
/// `plane_sets.toml` ni en `screens.toml`, por TAMAÑO: una tira de un nivel tipo
/// Sonic son ~36 000 celdas (~1 MB), entre 10 y 40 veces todo el resto del
/// catálogo junto. Meterla en el catálogo de elementos haría que renombrar un
/// glifo reescribiera un megabyte y que abrir el proyecto lo reparseara aunque
/// el artista vaya a otro workspace.
///
/// `lx`/`ly` son la posición en ESPACIO DE NIVEL y van con SIGNO: el origen de
/// la tira es el mínimo observado, así que una celda vista antes del origen da
/// negativo. El `col`/`row` sin signo de PackScreenCell no sirve acá — truncaría
/// la tira entera.
struct PackPanoramaCell { uint64_t hash; int32_t lx, ly; };
struct PackPanorama {
    uint64_t    id = 0;
    std::string name;
    uint8_t     plane = 0;
    int32_t     origin_x = 0, origin_y = 0;
    uint16_t    w_cells = 0, h_cells = 0;
    std::string asset;
    std::vector<PackPanoramaCell> cells;
};

/// `panoramas.toml` — un [[panorama]] por tira. Las celdas van en un ARRAY con
/// una entrada POR FILA (`ly: lx:hash|lx:hash|…`) y no en un pipe-list único: un
/// millón de caracteres en una sola línea rompe los diffs de git, que es el
/// criterio declarado de los escritores a mano de este proyecto. El `lx`
/// explícito por celda deja que la fila tenga huecos.
std::string bake_panoramas_toml(const std::vector<PackPanorama>& pans);
size_t parse_panoramas_toml(const std::string& text, std::vector<PackPanorama>& out);

/// -- CINEMÁTICA (CU004) ------------------------------------------------------
/// Una secuencia ORDENADA de Cuadros. A diferencia de la Panorámica, es una
/// lista corta de ids: cabe holgada en una línea y no necesita dialecto propio.
/// El asset por paso es opcional — vacío = se usa el del Cuadro, que es el caso
/// normal (lo que la Cinemática aporta es el ORDEN, no otro dibujo).
struct PackKinematicStep {
    uint64_t    screen_id = 0;
    std::string asset;
    /// Frame del clip en que arranca este paso, si `asset` es un `.ivf` ().
    uint32_t    video_offset = 0;
};
struct PackKinematic {
    uint64_t    id = 0;
    std::string name;
    uint32_t    gap_frames = 12;   ///< frames tolerados sin Cuadro confirmado
    /// El video CICLA si es más corto que el tramo (en vez de sostener su
    /// último frame). Decisión autoral: un fondo quiere ciclar, una escena
    /// narrada no.
    bool        loop = false;
    /// Pista de AUDIO del video — asset aparte, porque el IVF es sólo video.
    /// Vacío = la Cinemática suena con el audio del juego.
    std::string audio;
    float       gain = 1.0f;        ///< volumen de la pista de la Cinemática
    float       game_gain = 1.0f;   ///< volumen de la banda sonora del juego
                                    ///< MIENTRAS corre (ducking, 1 = intacta)
    std::vector<PackKinematicStep> steps;
};

/// `kinematics.toml` — un [[kinematic]] por secuencia. Los pasos van en un
/// pipe-list `0xid` o `0xid:asset`, mismo dialecto compacto del resto del pack.
std::string bake_kinematics_toml(const std::vector<PackKinematic>& kins);
size_t parse_kinematics_toml(const std::string& text, std::vector<PackKinematic>& out);

// ---------------------------------------------------------------------------
// instruments.toml (/) — re-sintesis POR TIMBRE: que voz del chip se
// reemplaza por que preset de que SoundFont. Es un eje COMPLEMENTARIO a la
// Secuencia: el juego sigue tocando —su tempo, sus cortes— y solo cambia el
// TIMBRE, asi que no puede desincronizar.
//
// Mismo esquema que hornea el Lab y que lee el core Rust
// (core/src/instrument_map.rs). Vive aca para que el pack tenga UN lector y
// para que sea testeable sin sesion.
// ---------------------------------------------------------------------------
struct PackInstrument {
    uint64_t    patch = 0;       ///< timbre del juego (id de instrumento)
    std::string soundfont;       ///< basename; el pack lo trae recortado
    uint16_t    bank = 0, preset = 0;
    int8_t      transpose = 0;
    float       gain = 1.0f;
};

/// Parsea `instruments.toml`. Un timbre SIN soundfont se descarta: no se puede
/// re-sintetizar y dejarlo poblaria el catalogo con una entrada que apunta a
/// la nada. Devuelve cuantos quedaron.
size_t parse_instruments_toml(const std::string& text, std::vector<PackInstrument>& out);

/// ANIMACIÓN (): una secuencia de plane sets (Objetos) con reloj propio.
/// Va en `plane_sequences.toml` y no en `animations.toml`, que ya es de las
/// ACCIONES (poses HD en fase): son dos cosas distintas y compartir archivo
/// las mezclaría. El nombre lo ata a `plane_sets.toml`, que es sobre lo que
/// cicla.
struct PackPlaneSeqStep {
    uint64_t    set_id   = 0;
    std::string asset;         ///< "" = el asset del propio set
    uint16_t    duration = 0;  ///< frames de juego (0 = default del motor)
};
struct PackPlaneSequence {
    uint64_t    id = 0;
    std::string name;
    std::vector<PackPlaneSeqStep> steps;
};
std::string bake_plane_sequences_toml(const std::vector<PackPlaneSequence>& seqs);
size_t parse_plane_sequences_toml(const std::string& text,
                                  std::vector<PackPlaneSequence>& out);

/// Paso vigente a `t` frames del ancla, donde `t` YA viene reducido al ciclo
/// (el llamador hace el módulo contra la suma de duraciones). `duration == 0`
/// cae a `default_dur`. Devuelve `n-1` si `t` se pasa — no puede pasar con el
/// módulo hecho, pero un paso vigente es mejor que ninguno.
///
/// Es una función LIBRE y no un método del runtime para poder probar la
/// cadencia sin emulador: el reloj de la Animación vive dentro de produce_frame
/// y montar un caso real pide core + ROM + un frame que matchee.
uint32_t plane_sequence_step_at(const uint16_t* durations, uint32_t n,
                                uint64_t t, uint16_t default_dur);
/// Suma de duraciones (largo del ciclo en frames de juego). 0 si `n == 0`.
uint32_t plane_sequence_total(const uint16_t* durations, uint32_t n,
                              uint16_t default_dur);

/// Lee `plane_sets.toml`. Devuelve la cantidad de sets válidos (con miembros y
/// asset); los inválidos se descartan en silencio, igual que el resto de los
/// parsers del pack. Las fuentes salen por `out_fonts` (pueden venir vacías).
size_t parse_plane_sets_toml(const std::string& text,
                             std::vector<PackPlaneSet>& out_sets,
                             std::vector<PackPlaneFont>& out_fonts);

/// -- elements.toml () ----------------------------------------------------
///
/// Las Identidades autorables de Pintar en UN documento. Antes eran cinco
/// archivos, y lo que los separaba no era la Identidad sino el MECANISMO del
/// motor que las sirve: Cuadro, Panorámica, Cinemática, Animación, Utilería,
/// Carácter y UI son todas la misma familia para el artista. Acá el mecanismo
/// es el nombre del array y no un archivo.
///
/// La forma de cada entrada NO cambia: el documento es la concatenación de lo
/// que antes salía por separado. Por eso `parse_elements_toml` puede reusar los
/// mismos decodificadores, y un pack viejo se sigue leyendo con los cinco
/// `parse_*_toml` de arriba, que quedan vigentes para el LEGACY y para los
/// archivos del proyecto.
///  (runtime_enhancement): una Identidad marcada «Mejorar por software»,
/// ya EXPANDIDA a su identidad de motor (capa, hashes) — el runtime no vuelve
/// a resolver nada: el compose indexado mejora por (capa, hash) lo que no
/// reclamó un HD. `layer` es la capa de SceneElement: 0=B · 1=A · 2=W ·
/// 3=Sprite (¡no el plano de Pintar, que es 0=A · 1=B · 2=Window!).
/// Bloques `[[enhance]]` dentro de elements.toml; un player viejo ignora el
/// array desconocido y degrada a original — [[set]]/[[screen]] no cambian.
struct PackEnhance {
    uint64_t    id = 0;            ///< id de la Identidad de origen (informativo)
    std::string name;
    uint8_t     layer = 3;         ///< capa de SceneElement
    std::vector<uint64_t> hashes;  ///< pipe-list "0x…|0x…" en el TOML
    uint8_t     k = 255;           ///< : intensidad 0..255; ausente = 255 (se omite en el default)
};
std::string bake_enhance_toml(const std::vector<PackEnhance>& enh);
size_t parse_enhance_toml(const std::string& text, std::vector<PackEnhance>& out);

std::string bake_elements_toml(const std::vector<PackScreen>& screens,
                               const std::vector<PackPanorama>& pans,
                               const std::vector<PackKinematic>& kins,
                               const std::vector<PackPlaneSequence>& seqs,
                               const std::vector<PackPlaneSet>& sets,
                               const std::vector<PackPlaneFont>& fonts,
                               const std::vector<PackEnhance>& enh = {});

/// Lee el documento único en una sola pasada de parse. Devuelve el total de
/// entradas válidas de todas las familias. `enh` () puede ser nullptr:
/// un consumidor que no mejora nada no necesita la lista.
size_t parse_elements_toml(const std::string& text,
                           std::vector<PackScreen>& screens,
                           std::vector<PackPanorama>& pans,
                           std::vector<PackKinematic>& kins,
                           std::vector<PackPlaneSequence>& seqs,
                           std::vector<PackPlaneSet>& sets,
                           std::vector<PackPlaneFont>& fonts,
                           std::vector<PackEnhance>* enh = nullptr);

}  // namespace ayther

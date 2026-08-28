#pragma once
// ---------------------------------------------------------------------------
// C declarations for symbols exported from the Rust ayther_core static lib.
// Keep this header in sync with core/src/lib.rs.
//
// Most of the type-safe surface now goes through cxx::bridge (core/src/ffi.rs,
// integrated via corrosion). The extern-C wrappers below remain ONLY for the
// zero-copy hot path (process_frame / update_ram / set_pack — raw pointers cxx
// does not bridge); keep them in sync with lib.rs by hand.
// ---------------------------------------------------------------------------
// `<stdint.h>` en vez de `<cstdint>` y `extern "C"` bajo guarda: es un paso
// hacia poder incluirlo desde C, y desde C++ no cambia nada.
//
// AVISO, para no prometer a medias: este header **todavía no es C puro**. Usa
// `bool` y nombres de struct sin `typedef`, así que un `.c` no lo compila. La
// API C del SDK es `ayther_sdk.h` —ahí está la superficie pensada para C, con
// lectura de packs incluida— y éste es un header de contrato compartido en C++.
// Lo destapó el ejemplo `pack_read`, al intentar usarlo desde C.
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

/// Returns the ayther_core library version integer (5 = v0.5.0).
uint32_t ayther_core_version();

// ---------------------------------------------------------------------------
// Sonic 2 — 68000 work-RAM reads
// ---------------------------------------------------------------------------

/// Read player X/Y position.
/// Offsets in the 64 KB RAM block: X=0xB008, Y=0xB00C (Big-Endian i16).
bool ayther_sonic_read_xy(const uint8_t* ram, size_t size,
                           int16_t* out_x, int16_t* out_y);

/// Read player X/Y velocity (signed fixed-point subpixels/frame).
/// Offsets: VX=0xB014, VY=0xB018 (Big-Endian i16).
bool ayther_sonic_read_velocity(const uint8_t* ram, size_t size,
                                 int16_t* out_vx, int16_t* out_vy);

// ---------------------------------------------------------------------------
// TileHasher — opaque sprite fingerprinting engine
//
// Ownership: caller creates with _new(), must release with _free().
// Thread safety: NOT thread-safe; drive from one emulation thread.
//
// Pixel format values (RETRO_PIXEL_FORMAT_*):
//   0 = 0RGB1555 (legacy)
//   1 = XRGB8888
//   2 = RGB565   (Genesis Plus GX default)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  — gate de condiciones de audio (el evaluador vive en el core)
// ---------------------------------------------------------------------------

struct AudioEventGate;   // opaco — no dereferenciar

/// Compila el gate desde el texto de audio_events.toml. Devuelve NULL cuando no
/// hay ninguna condición (el caso normal), y así el caller se ahorra la consulta
/// por frame sin tener que preguntar nada.
AudioEventGate* ayther_audio_gate_new(const char* text);
void            ayther_audio_gate_free(AudioEventGate* g);

/// Firmas cuyas condiciones NO se cumplen en este frame — las que tienen que
/// sonar en ORIGINAL. Escribe hasta `cap` y devuelve el total disponible.
uint32_t ayther_audio_gate_eval(const AudioEventGate* g,
                                const uint8_t* ram, size_t ram_len,
                                bool word_swapped, uint32_t frame,
                                uint64_t* out, uint32_t cap);

// ---------------------------------------------------------------------------
//  EM-8.2 — gate del ENSANCHADO (mismo camino A que el de audio)
// ---------------------------------------------------------------------------

struct WidescreenGate;   // opaco — no dereferenciar

/// Compila el gate desde el texto de widescreen.toml. Devuelve NULL cuando el
/// pack no declara `[[widescreen]]` — todos los ya horneados — y así el caller
/// se ahorra la consulta por frame Y no apaga el ensanchado manual del Lab.
WidescreenGate* ayther_widescreen_gate_new(const char* text);
void            ayther_widescreen_gate_free(WidescreenGate* g);

/// El ancho lógico de este frame. Escribe `out_width` y devuelve true SÓLO si
/// alguna regla matcheó; con false el caller conserva lo que tenía.
bool ayther_widescreen_gate_eval(const WidescreenGate* g,
                                 const uint8_t* ram, size_t ram_len,
                                 bool word_swapped, uint32_t frame,
                                 uint32_t* out_width);

struct AytherTileHasher;  // opaque — do not dereference

/// Allocate a new TileHasher.  Free with ayther_tile_hasher_free().
AytherTileHasher* ayther_tile_hasher_new();

/// Destroy a TileHasher and release its memory.
void ayther_tile_hasher_free(AytherTileHasher* h);

/// Submit one video frame.
/// Returns the number of NEW unique tiles discovered in this frame.
uint32_t ayther_tile_hasher_process_frame(AytherTileHasher* h,
                                           const uint8_t*    pixels,
                                           uint32_t          width,
                                           uint32_t          height,
                                           size_t            pitch,
                                           uint32_t          pixel_format);

/// Total unique tiles accumulated since creation.
uint32_t ayther_tile_hasher_unique_count(const AytherTileHasher* h);

/// Dump the full catalog to a TOML file.  Returns true on success.
bool ayther_tile_hasher_dump_toml(const AytherTileHasher* h,
                                   const char*             path);

// ---------------------------------------------------------------------------
// AyArchive — opaque .ay pack filesystem  (v0.4.0)
//
// Format: ZIP container + manifest.toml + optional ED25519 signature.
//
// Ownership: caller opens with _open(), must release with _close().
// Thread safety: NOT thread-safe; use from one thread at a time.
// ---------------------------------------------------------------------------

struct AyArchive;  // opaque — do not dereference

/// Open a .ay pack file.  Returns NULL on error (bad signature, missing
/// manifest, corrupt ZIP).  Unsigned packs are accepted in Debug builds only.
/// Free the handle with ayther_pack_close().
AyArchive* ayther_pack_open(const char* path);

/// Destroy an AyArchive and release its memory.
void ayther_pack_close(AyArchive* pack);

/// Set the active region for transparent asset overrides.
/// E.g. "JP" → read("graphics/title.png") tries "locales/JP/graphics/title.png" first.
void ayther_pack_set_region(AyArchive* pack, const char* region);

/// : bitmask de tiers de resolución incluidos (bit t = tier t presente;
/// 0 = pack legacy). Tiers: 0=HD 3x · 1=Full HD 4.5x · 2=2K 6x · 3=4K 9x ·
/// 4=8K 18x. El indice CRECE con la resolucion: `set_tier` barre hacia arriba
/// desde el ideal y uno fuera de orden le haria elegir de menos.
uint8_t ayther_pack_tiers(const AyArchive* pack);

/// : activa el tier para el `ideal` del display — el menor incluido >=
/// ideal, o el mayor incluido si no hay. Los lookups resuelven
/// `tiers/<activo>/<nombre>` de forma transparente. No-op en packs legacy.
void ayther_pack_set_tier(AyArchive* pack, int ideal);

/// : mapea la altura de salida (px) al tier ideal y lo activa:
/// <=720 HD · <=1080 Full HD · <=1440 2K · <=2160 4K · mas 8K.
void ayther_pack_set_tier_for_height(AyArchive* pack, int out_height_px);

/// Return the size in bytes of a logical asset, or -1 if not found.
int64_t ayther_pack_file_size(const AyArchive* pack, const char* logical_path);

/// Read a logical asset into out_buf (capacity buf_cap bytes).
/// Returns bytes written, or -1 if path not found or buffer too small.
/// Pre-allocate using ayther_pack_file_size().
int64_t ayther_pack_read(const AyArchive* pack, const char* logical_path,
                          uint8_t* out_buf, size_t buf_cap);

/// : si la entrada se puede leer POR RANGO. Preguntarlo antes es lo que
/// permite elegir estrategia: con streaming un video se reproduce sin
/// materializarse (RAM = un frame), sin streaming hay que leerlo entero.
///
/// Es true sólo para entradas `Stored` cuyo indice firmado trae hashes por
/// trozo: los packs anteriores a  y toda entrada deflateada dan false.
/// : cuántas entradas tiene el pack, y el nombre de cada una (orden
/// alfabético estable). El nombre se COPIA al buffer del llamador: un puntero
/// prestado obligaría a saber cuánto vive, y ése es el contrato que nadie lee.
/// Devuelve los bytes escritos, 0 si el índice no existe, y NEGATIVO (el largo
/// necesario) si el buffer es chico.
uint32_t ayther_pack_entry_count(const AyArchive* pack);
int32_t  ayther_pack_entry_name(const AyArchive* pack, uint32_t i,
                                char* dst, uint32_t cap);

bool ayther_pack_entry_streamable(const AyArchive* pack, const char* logical_path);

/// : leer `len` bytes de una entrada desde `offset` sin materializarla.
///
/// Devuelve los bytes escritos —puede ser MENOS que `len` cuando el rango llega
/// al final de la entrada— o -1 si la entrada no es direccionable por rango, si
/// el rango cae fuera, o si un trozo no verifica contra el indice firmado.
/// Nada sale de aca sin verificar: la unidad de verificacion es el trozo, no la
/// entrada, y eso es lo que hace barata la lectura parcial.
int64_t ayther_pack_read_range(const AyArchive* pack, const char* logical_path,
                               uint64_t offset, uint8_t* out_buf, size_t len);

/// Return the pack's game_id as a null-terminated string.
/// Valid for the lifetime of the AyArchive.  Do NOT free the pointer.
const char* ayther_pack_game_id(const AyArchive* pack);

// ---------------------------------------------------------------------------
//  — validación de compatibilidad, ANTES de abrir el pack
// ---------------------------------------------------------------------------
//
// Devuelve una LISTA de hallazgos y no un booleano, porque hay dos cosas
// distintas que un pack puede tener mal:
//
//   · incompatibilidad CRÍTICA (es de otro juego, pide un Engine que no
//     existe) → error: abrir serviría contenido equivocado;
//   · degradación OPCIONAL (trae un subsistema que este build no conoce, se
//     horneó con otro core) → advertencia: correr y avisar.
//
// Con un booleano, o se rechaza lo segundo —y un pack usable no abre— o se
// acepta lo primero, y el usuario ve el pack de otro juego sin saber por qué.
//
// No abre el pack: por eso un pack incompatible no puede tirar la sesión.

typedef struct AytherPackReport AytherPackReport;

/// Lo que la sesión tiene puesto. Punteros nulos y `has_rom = false` significan
/// «no se sabe» — y eso sale como ADVERTENCIA en el informe, para que «no se
/// comprobó» no se lea como «está bien».
typedef struct {
    uint32_t    rom_crc32;
    bool        has_rom;
    const char* platform;        ///< "megadrive" · "segacd" · NULL
    const char* core_build_id;   ///< build_id del core del fork · NULL
    const char* engine_version;  ///< NULL = la de este build
    bool        release_build;   ///< en release, un pack sin firma es ERROR
} AytherValidateCtx;

AytherPackReport* ayther_pack_validate(const char* path, const AytherValidateCtx* ctx);
uint32_t          ayther_pack_report_count(const AytherPackReport* r);
/// 0 = error · 1 = advertencia · 2 = recomendación () · -1 fuera de rango.
int32_t           ayther_pack_report_severity(const AytherPackReport* r, uint32_t i);
/// Código estable, para decidir sin parsear castellano.
const char*       ayther_pack_report_code(const AytherPackReport* r, uint32_t i);
/// Mensaje para humanos.
const char*       ayther_pack_report_message(const AytherPackReport* r, uint32_t i);
/// La única pregunta que decide si arrancar. Las advertencias se muestran igual.
bool              ayther_pack_report_has_errors(const AytherPackReport* r);
void              ayther_pack_report_free(AytherPackReport* r);

/// : el GRADO de compatibilidad, derivado del mismo informe de arriba.
///
/// Existe además de `ayther_pack_validate` porque la pregunta es otra: el
/// informe dice QUÉ pasa, el grado dice QUÉ HACER. Y porque el criterio para
/// pasar de una lista de hallazgos a un veredicto tiene que ser uno solo —
/// Play, el SDK y el Hub contestan lo mismo porque llaman acá.
typedef struct AytherCompat AytherCompat;

/// 0 exacta · 1 con advertencias · 2 experimental · 3 incompatible.
/// De mejor a peor, y el orden es contrato.


///  EM-4.1: el hash de FORMA de un tile — invariante al brillo, sensible a
/// la silueta. Agrupa las variantes que un fade POR CONTENIDO produce (el juego
/// escribe tiles con índices más oscuros), que son las que cuestan autoría: un
/// fade por PALETA ya lo agrupa solo el hash de sprite, que es ciego a paleta.
///
/// No reemplaza al hash de identidad: lo acompaña. Uno dice «qué tile es» y
/// éste, «de qué familia».
uint64_t ayther_tile_shape_hash(const uint8_t* tile, uint32_t n);
/// Nivel medio de los píxeles OPACOS (0..15). Negativo = todo transparente, que
/// NO es lo mismo que un tile negro.
float    ayther_tile_mean_level(const uint8_t* tile, uint32_t n);
/// Cuánto atenuar el asset de `referencia` para reproducir `tile`. Negativo =
/// alguno no tiene píxeles opacos.
float    ayther_tile_brightness_factor(const uint8_t* tile, const uint8_t* referencia);

///  EM-7.4: parches IPS/BPS del USUARIO, aplicados en RAM.
///
/// Una fan-translation o un romhack es un parche: describe cómo transformar una
/// ROM que el usuario ya tiene, sin llevar el juego adentro. Por eso se puede
/// distribuir donde la ROM no, y por eso esto es BYOR-safe.
///
/// El parche se aplica al BUFFER que se le pasa al core, nunca al archivo del
/// disco: la misma doctrina que el resto del proyecto, y acá además protege al
/// usuario de quedarse sin su ROM original por probar un hack.
bool    ayther_is_rom_patch(const uint8_t* data, uint32_t size);
/// Bytes escritos, o negativo: -1 args · -2 no es parche · -3 no entra
/// (el tamaño necesario queda en `out_needed`) · -4 fallo (ver el error).
int64_t ayther_apply_rom_patch(const uint8_t* rom, uint32_t rom_n,
                               const uint8_t* parche, uint32_t parche_n,
                               uint8_t* out, uint32_t out_cap,
                               uint32_t* out_needed);
/// El motivo del último fallo. Un «no se pudo parchear» sin motivo deja al
/// usuario sin saber si bajó el parche equivocado o si su ROM está dañada.
uint32_t ayther_rom_patch_error(char* buf, uint32_t cap);

AytherCompat* ayther_pack_compat(const char* path, const AytherValidateCtx* ctx);
int32_t       ayther_compat_grade(const AytherCompat* c);
/// Nunca vacío para un handle válido.
const char*   ayther_compat_reason(const AytherCompat* c);
/// Lo que NO se pudo comprobar — es lo que separa «experimental» de «exacta».
uint32_t      ayther_compat_unverified_count(const AytherCompat* c);
const char*   ayther_compat_unverified(const AytherCompat* c, uint32_t i);
/// El veredicto entero en JSON, con el informe adentro.
const char*   ayther_compat_json(const AytherCompat* c);
void          ayther_compat_free(AytherCompat* c);

// ---------------------------------------------------------------------------
//  — perfiles de remasterización
// ---------------------------------------------------------------------------
//
// Un perfil NO multiplica el material: filtra el que ya está. Declara qué
// subsistemas enciende y qué buses silencia; los assets son los mismos. Sin
// eso, un pack con cuatro perfiles pesaría cuatro veces.
//
// La lista SIEMPRE trae «original» primero (implícito, no se declara y no se
// puede sacar) y siempre tiene exactamente un default: el llamador no tiene que
// defenderse de una lista vacía ni de dos defaults.

/// Cuántos perfiles ofrece el pack. Nunca 0.
uint32_t ayther_pack_profile_count(const AyArchive* pack);

/// Campo `field` del perfil `i`: "id" · "name" · "description". La cadena vale
/// hasta la próxima llamada a esto o a `ayther_pack_meta_field` — comparten
/// buffer, porque dos con la misma regla sólo agregan una forma de equivocarse.
const char* ayther_pack_profile_field(AyArchive* pack, uint32_t i, const char* field);

/// Subsistemas que enciende (bit j = `ayther_subsystem_name(j)`). 0 en
/// «original», que es lo correcto: no enciende nada.
uint32_t ayther_pack_profile_systems(const AyArchive* pack, uint32_t i);

/// Buses que silencia (0=sin clasificar · 1=música · 2=efectos · 3=voces).
uint32_t ayther_pack_profile_muted_buses(const AyArchive* pack, uint32_t i);

/// El perfil que se aplica al cargar el pack sin pedir otro.
uint32_t ayther_pack_default_profile(const AyArchive* pack);

/// Índice del perfil `id`, o -1 si el pack no lo tiene. No devuelve 0 porque
/// «no existe» y «existe y no enciende nada» son cosas distintas.
int32_t ayther_pack_profile_index(const AyArchive* pack, const char* id);

// ---------------------------------------------------------------------------
//  — créditos y procedencia del pack, para Play y Hub
// ---------------------------------------------------------------------------

/// Handle opaco con `credits.toml` ya parseado. Se pide, se consulta y se
/// libera: el pack no paga el parseo cuando nadie los muestra.
typedef struct AytherCredits AytherCredits;

/// NULL si el pack no trae créditos o si el archivo está roto — un pack sin
/// créditos es válido y uno con el archivo ilegible tiene que seguir jugándose.
/// Lo que no puede es mostrar una atribución inventada.
AytherCredits* ayther_pack_credits(const AyArchive* pack);

/// Cuántas PERSONAS acredita el pack (no cuántos assets).
uint32_t    ayther_credits_count(const AytherCredits* c);
const char* ayther_credits_author(const AytherCredits* c, uint32_t i);
/// Rol declarado, o cadena vacía (no NULL: un rol ausente no es fuera de rango).
const char* ayther_credits_role(const AytherCredits* c, uint32_t i);
/// Las licencias que aportó, separadas por coma.
const char* ayther_credits_licenses(const AytherCredits* c, uint32_t i);
uint32_t    ayther_credits_assets(const AytherCredits* c, uint32_t i);

/// La atribución del asset `asset_id` — lo que Play muestra del asset que está
/// usando. El id es el nombre de contenido de la entrada (), sin `assets/`
/// ni el prefijo de tier: el mismo dibujo en cuatro tiers es UN asset y tiene
/// UNA procedencia. NULL si ese asset no declara nada.
const char* ayther_credits_attribution(const AytherCredits* c, const char* asset_id);

void ayther_credits_free(AytherCredits* c);

// ---------------------------------------------------------------------------
//  — metadatos del manifest, consultables SIN ejecutar el pack
// ---------------------------------------------------------------------------

/// Versión de ESQUEMA del manifest que este build escribe y entiende.
uint32_t ayther_manifest_schema_supported(void);

/// Versión del ENGINE — la misma contra la que el validador () compara el
/// `engine_min` del pack. La expone el core para que el reporte técnico ()
/// no mantenga una segunda copia del número: dos copias se separan en el primer
/// bump y el reporte pasa a mentir sobre qué Engine horneó el pack.
const char* ayther_engine_version(void);

/// Esquema DECLARADO por el pack (1 = horneado antes de que el campo
/// existiera). Un pack con un esquema MAYOR que el soportado no abre: la
/// tolerancia a campos desconocidos es forward-compat de datos, pero abrir un
/// pack que dice depender de algo que este build no sabe leer sería servirlo a
/// medias y llamarlo éxito.
uint32_t ayther_pack_schema(const AyArchive* pack);

/// Subsistemas que AYTHER sabe sustituir, en ORDEN CANÓNICO. El índice es el
/// contrato con `AytherSubsystem` del Engine, y hay un test que compara las dos
/// listas nombre por nombre.
uint32_t    ayther_subsystem_count(void);
const char* ayther_subsystem_name(uint32_t index);

/// : máscara de subsistemas que el pack DECLARA traer (bit i = el pack trae
/// `ayther_subsystem_name(i)`).
///
/// **0 es ambiguo**: hay que leerlo junto con `ayther_pack_declares_systems`.
/// Un pack legacy no declara nada, y eso no es lo mismo que declarar que no
/// trae nada — tratarlos igual haría que todo pack viejo apareciera vacío.
uint32_t ayther_pack_systems(const AyArchive* pack);
bool     ayther_pack_declares_systems(const AyArchive* pack);

/// : un campo de `[compat]` o de autoría, como cadena NUL-terminada.
/// `field`: "rom_crc32" · "platform" · "core_min" · "license" · "contributors"
/// (esta última separada por comas). NULL = no declarado, que es distinto de
/// declarado vacío.
///
/// El puntero vive hasta la próxima llamada sobre el MISMO pack (se cachea
/// adentro para no filtrar una asignación por consulta), así que hay que
/// copiarlo antes de volver a preguntar.
const char* ayther_pack_meta_field(AyArchive* pack, const char* field);

/// BUILD ID del pack (/) — identifica UN horneado concreto.
///
/// NO está declarado en el pack: se DERIVA de los bytes de `integrity.toml`,
/// que es el conjunto de hashes de todo lo que hay adentro. Por eso no puede
/// mentir (un campo del manifest se edita; esto se recalcula), dos horneados
/// idénticos dan el mismo id, y no hay circularidad — declararlo en el manifest
/// era imposible, porque la integridad cubre el manifest.
///
/// Es lo que hace diagnosticable un pack cuyos assets se nombran por hash: el
/// mensaje de error lleva `hash - juego vN build XXXX` y el buscador del Lab
/// resuelve ese par contra el log del horneado.
///
/// VACÍO en packs legacy (sin integrity.toml). Tratarlo como «desconocido», no
/// como un id.
const char* ayther_pack_build_id(const AyArchive* pack);

// ---------------------------------------------------------------------------
// AytherPackWatcher — cross-platform pack hotreload watcher  (v0.9.5)
//
// Wraps a background OS file-change notifier:
//   Windows — ReadDirectoryChangesW
//   Linux   — inotify
//   macOS   — FSEvents
//
// poll() is non-blocking: drains a background-thread mpsc channel.
// Intended to be called once per frame (~16 ms period).
//
// Ownership: caller creates with _new(), must release with _free().
// ---------------------------------------------------------------------------

struct AytherPackWatcher;  // opaque — do not dereference

/// Start watching `path` for modifications/creations.
/// Returns NULL if the path's parent directory cannot be watched
/// (unsupported platform, bad path, permission error, etc.).
AytherPackWatcher* ayther_pack_watcher_new(const char* path);

/// Non-blocking poll.
/// Returns true if the watched file was created or modified since the
/// last call.  Drains all pending OS events so subsequent calls within
/// the same frame do not re-trigger.
/// Returns false if `w` is NULL.
bool ayther_pack_watcher_poll(AytherPackWatcher* w);

/// Destroy a watcher and release its memory.
void ayther_pack_watcher_free(AytherPackWatcher* w);

// ---------------------------------------------------------------------------
// ScriptEnv — sandboxed Lua 5.4 runtime  (v0.5.0)
//
// API available to pack scripts:
//   ayther.version()           → string
//   ayther.log(msg)            → void
//   ayther.ram.read_u8(n)      → u8
//   ayther.ram.read_u16_be(n)  → u16   big-endian (68000 native)
//   ayther.ram.read_i16_be(n)  → i16   (player positions, velocity)
//   ayther.ram.read_u32_be(n)  → u32
//   ayther.pack.read(path)     → string|nil
//   ayther.pack.exists(path)   → bool
//   ayther.on_frame(fn)        → register a per-frame callback
//
// Ownership: caller creates with _new(), must release with _free().
// Thread safety: NOT thread-safe; drive from the emulation thread only.
// ---------------------------------------------------------------------------

struct AytherScriptEnv;  // opaque — do not dereference

/// Create a sandboxed Lua 5.4 ScriptEnv.  Returns NULL on failure.
AytherScriptEnv* ayther_script_new();

/// Destroy a ScriptEnv.
void ayther_script_free(AytherScriptEnv* env);

/// Link the loaded .ay pack so `ayther.pack.*` works.  Pass NULL to detach.
void ayther_script_set_pack(AytherScriptEnv* env, const AyArchive* pack);

/// Load and execute a Lua source string.  Returns true on success.
bool ayther_script_load_string(AytherScriptEnv* env,
                                const char*      source,
                                const char*      chunk_name);

/// Snapshot RAM and fire all ayther.on_frame callbacks.
/// Returns the count of callbacks that completed without error.
uint32_t ayther_script_on_frame(AytherScriptEnv* env,
                                 const uint8_t*   ram,
                                 size_t           ram_size);

// ---------------------------------------------------------------------------
// Tile substitution — POD structs  (v0.6.0)
// (Defined before the functions that reference them.)
// ---------------------------------------------------------------------------

/// xxHash3-64 tile occurrence in a single frame.
struct AytherTileOccurrence {
    uint64_t hash;
    uint32_t tile_x;  ///< tile-grid column (pixel_x = tile_x × 8)
    uint32_t tile_y;  ///< tile-grid row    (pixel_y = tile_y × 8)
};

/// Lua-registered tile override: replace hash with the given pack asset.
struct AytherTileOverride {
    uint64_t hash;
    char     asset_path[256];  ///< null-terminated logical path in the .ay pack
};

/// Resolved substitution instruction: blit asset at (tile_x, tile_y).
struct AytherTileSub {
    char     asset_path[256];
    uint32_t tile_x;
    uint32_t tile_y;
};

/// Push tile occurrences for the current frame so `ayther.tiles.list()`
/// returns correct data inside on_frame callbacks.
/// Call this BEFORE ayther_script_on_frame().
/// Also call ayther_script_update_audio() before on_frame for audio list.
void ayther_script_update_tiles(AytherScriptEnv*            env,
                                 const AytherTileOccurrence* occs,
                                 uint32_t                    count);

/// Read tile overrides registered by Lua `ayther.tiles.replace()`.
/// Fill out_buf with up to buf_cap entries.  Returns entries written.
uint32_t ayther_script_get_tile_overrides(const AytherScriptEnv* env,
                                           AytherTileOverride*    out_buf,
                                           uint32_t               buf_cap);

// Forward declarations required by the script API below (full definitions
// appear later in this header in the sprite/audio hasher sections).
struct AytherSpriteOccurrence;
struct AytherAudioOccurrence;

// ---------------------------------------------------------------------------
// Lua sprite API  (v0.9.3)
// ---------------------------------------------------------------------------

/// Push the current frame's sprite occurrences so `ayther.sprites.list()` returns
/// correct data inside on_frame callbacks.
/// Call this BEFORE ayther_script_on_frame() each frame.
void ayther_script_update_sprites(AytherScriptEnv*              env,
                                   const AytherSpriteOccurrence* occs,
                                   uint32_t                      occ_count);

/// Lua-registered sprite override: replace hash with the given pack asset.
struct AytherSpriteOverride {
    uint64_t hash;
    char     asset_path[256];
};

/// Read sprite overrides registered by Lua `ayther.sprites.replace()`.
/// Fill out_buf with up to buf_cap entries.  Returns entries written.
uint32_t ayther_script_get_sprite_overrides(const AytherScriptEnv* env,
                                             AytherSpriteOverride*  out_buf,
                                             uint32_t               buf_cap);

// ---------------------------------------------------------------------------
// Lua audio API  (v0.9.2)
// ---------------------------------------------------------------------------

/// Push the current tick's audio occurrences so `ayther.audio.list()` returns
/// correct data inside on_frame callbacks.
/// Call this BEFORE ayther_script_on_frame() each tick.
void ayther_script_update_audio(AytherScriptEnv*             env,
                                 const AytherAudioOccurrence* occs,
                                 uint32_t                     occ_count);

/// Lua-registered audio override: replace hash with the given pack asset.
struct AytherAudioOverride {
    uint64_t hash;
    char     asset_path[256];
};

/// Read audio overrides registered by Lua `ayther.audio.replace()`.
/// Fill out_buf with up to buf_cap entries.  Returns entries written.
uint32_t ayther_script_get_audio_overrides(const AytherScriptEnv* env,
                                            AytherAudioOverride*   out_buf,
                                            uint32_t               buf_cap);

/// Retrieve all tile occurrences from the most-recently-processed frame.
/// A 320×240 frame produces up to 1200 entries (40×30 tiles).
/// Returns the number of entries written to out_buf.
uint32_t ayther_tile_hasher_get_occurrences(const AytherTileHasher*  h,
                                             AytherTileOccurrence*    out_buf,
                                             uint32_t                 buf_cap);

// ---------------------------------------------------------------------------
// TileSubstitutor — hash-to-HD-asset mapping engine  (v0.6.0)
//
// Ownership: caller creates with _new(), must release with _free().
// ---------------------------------------------------------------------------

struct AytherTileSubstitutor;  // opaque — do not dereference

/// Create a TileSubstitutor.  Free with ayther_tile_sub_free().
AytherTileSubstitutor* ayther_tile_sub_new();

/// Destroy a TileSubstitutor.
void ayther_tile_sub_free(AytherTileSubstitutor* s);

/// Load the substitution catalog from `tile_substitutions.toml` in the pack.
void ayther_tile_sub_load_pack(AytherTileSubstitutor* s, const AyArchive* pack);

/// Load the catalog from a NAMED toml in the pack (Fase 2c: planos →
/// `plane_tile_substitutions.toml`). Mismo formato [[sub]].
void ayther_tile_sub_load_pack_named(AytherTileSubstitutor* s,
                                     const AyArchive* pack, const char* file);

/// Búsqueda directa hash → asset (override > catalog). Copia el path NUL-terminado
/// en `out` (cap bytes); devuelve true si había asignación. Para el resolver
/// scroll-aware de tiles de plano (computa la posición por su cuenta).
bool ayther_tile_sub_lookup(const AytherTileSubstitutor* s, uint64_t hash,
                            char* out, uint32_t cap);

/// EM-2 (): evalúa las condiciones del catálogo para este frame y fija el
/// asset vigente por hash. Llamar UNA vez por frame ANTES de los lookups.
/// `ram` es la Work RAM cruda del core; `word_swapped` declara si viene con el
/// `addr ^ 1` del Mega Drive en hosts LE (para el 68k: true) — así una
/// dirección del TOML es la misma que muestran RetroAchievements / Data Crystal.
/// No-op barato si el pack no trae condiciones.
void ayther_tile_sub_begin_frame(AytherTileSubstitutor* s,
                                 uint64_t               frame_number,
                                 const uint8_t*         ram,
                                 uint32_t               ram_len,
                                 bool                   word_swapped);

/// Register a Lua-driven runtime override (hash → asset_path).
void ayther_tile_sub_add_override(AytherTileSubstitutor* s,
                                   uint64_t               hash,
                                   const char*            asset_path);

/// Clear all Lua-registered overrides.  Call at start of each frame.
void ayther_tile_sub_clear_overrides(AytherTileSubstitutor* s);

/// Resolve tile occurrences to HD substitution instructions.
/// Returns the number of entries written to out_buf.
uint32_t ayther_tile_sub_resolve(const AytherTileSubstitutor* s,
                                  const AytherTileOccurrence*  occs,
                                  uint32_t                     occ_count,
                                  AytherTileSub*               out_buf,
                                  uint32_t                     buf_cap);

// ---------------------------------------------------------------------------
// SpriteHasher — VDP SAT sprite detection  (v0.8.0)
//
// Reads raw VRAM (retro_get_memory_data(RETRO_MEMORY_VIDEO_RAM)) each frame,
// parses the Sprite Attribute Table, and produces position-independent
// xxHash3-64 fingerprints per sprite character.
//
// The SAT base is set by VDP register $5 and varies by game and by H32/H40
// mode, so it must not be hardcoded. Pass AYTHER_SAT_AUTODETECT to recover it
// from VRAM structure each frame (the production path).
//
// Ownership: caller creates with _new(), must release with _free().
// Thread safety: NOT thread-safe; drive from the emulation thread only.
// ---------------------------------------------------------------------------

struct AytherSpriteHasher;  // opaque — do not dereference

/// Pass as `sat_base` to auto-detect the SAT base from VRAM (recommended).
/// A fixed address (e.g. 0xD800) only matches a subset of games/modes.
#define AYTHER_SAT_AUTODETECT ((size_t)-1)

/// Allocate a new SpriteHasher.  Free with ayther_sprite_hasher_free().
AytherSpriteHasher* ayther_sprite_hasher_new();

/// Destroy a SpriteHasher.
void ayther_sprite_hasher_free(AytherSpriteHasher* h);

/// Process one frame of raw VRAM.
/// vram_size is typically 65536 (64 KB) for Mega Drive.
/// sat_base: pass AYTHER_SAT_AUTODETECT to derive it from VRAM (recommended),
/// or an explicit byte offset to force a specific SAT base.
/// Returns the number of NEW unique sprite patterns discovered this frame.
uint32_t ayther_sprite_hasher_process_vram(AytherSpriteHasher* h,
                                            const uint8_t*      vram,
                                            size_t              vram_size,
                                            size_t              sat_base);

/// Process the sprites the VDP actually parsed this frame (the fork captures them in
/// parse_satb — AYTHER_MEMORY_PARSED_SPRITES, id 0x10B). `sprites` = `count` records
/// of 8 bytes each (yr/xr/attr u16 LE + w/h u8). Authoritative "what was drawn",
/// robust to mid-frame SAT rewrites/base swaps (Aladdin's Sega-logo genie). Tiles
/// hashed from `vram`. count == 0 → returns 0 (caller falls back to autodetect).
uint32_t ayther_sprite_hasher_process_sprites(AytherSpriteHasher* h,
                                              const uint8_t*      sprites,
                                              size_t              count,
                                              const uint8_t*      vram,
                                              size_t              vram_size);

/// Total unique sprite patterns accumulated since creation.
uint32_t ayther_sprite_hasher_unique_count(const AytherSpriteHasher* h);

// ---------------------------------------------------------------------------
// Sprite substitution — POD structs  (v0.8.0)
// ---------------------------------------------------------------------------

/// Per-frame sprite occurrence from the SAT (position-independent hash).
///
/// Layout (v0.10/R1.5, 24 bytes, align 8) — link + palette consume the old pad:
///   hash(8) | anim_group_id(8) | w_tiles(1) | h_tiles(1) | screen_x(2) | screen_y(2)
///   | link(1) | palette(1)
///
/// anim_group_id: 0 = static sprite (no detected animation cycle).
///                non-zero = xxHash3-64 of the sorted set of sibling frame hashes
///                that cycle at the same SAT slot over a 64-frame rolling window.
/// link:    SAT link field (index of next sprite in the chain, 0–127) — the
///          strongest metasprite grouping hint.
/// palette: VDP palette index 0–3 — secondary grouping hint.
struct AytherSpriteOccurrence {
    uint64_t hash;
    uint64_t anim_group_id;  ///< animation cycle group (0 = none)
    uint8_t  w_tiles;        ///< sprite width  in tiles (1–4)
    uint8_t  h_tiles;        ///< sprite height in tiles (1–4)
    int16_t  screen_x;       ///< top-left X in screen pixels
    int16_t  screen_y;       ///< top-left Y in screen pixels
    uint8_t  link;           ///< SAT link field (metasprite grouping hint)
    uint8_t  palette;        ///< VDP palette index 0–3
    uint8_t  priority;       ///< VDP priority bit (0=low,1=high) — metasprite front/back
    uint8_t  slot;           ///< SAT slot index 0–79 (Ayther hide-by-hash)
    uint8_t  hflip;          ///< VDP h-flip (CU-AN-11: auto-espejo del sheet HD)
    uint8_t  vflip;          ///< VDP v-flip
};

/// Resolved HD sprite substitution.
struct AytherSpriteSub {
    char    asset_path[256];  ///< logical path in the .ay pack
    int16_t screen_x;
    int16_t screen_y;
    uint8_t w_tiles;
    uint8_t h_tiles;
    /// Tamaño EXACTO del destino en píxeles. El bbox unión de una POSE casi
    /// nunca es múltiplo de 8 (p.ej. 29×64): truncarlo a tiles achataba el
    /// HD/snapshot al dibujarlo. 0 = derivar de w_tiles/h_tiles×8 (productores
    /// tile-exactos: per-sprite, tiles de plano, Modo 3).
    uint16_t w_px;
    uint16_t h_px;
    /// Arreglo con que matcheó la POSE: 0 = el capturado · bit0 = espejo H ·
    /// bit1 = espejo V. El asset (snapshot / HD default) es la cara CANÓNICA →
    /// el render lo dibuja pre-volteado por estos bits (la instancia espejada
    /// se ve en SU dirección). 0 en per-sprite/planos/Modo 3 y en poses con
    /// candidatos por variante.
    uint8_t mirror;
    /// Paleta observada del ancla (occ del per-sprite / miembro ancla de la
    /// pose): el fundido E1 se ancla acá, no a la primera occ del bbox (un
    /// ajeno solapado — el jinete sobre el Dragón — daba la paleta equivocada
    /// y el flash de paleta nunca modulaba el HD). 0xFF = desconocida
    /// (productores que no la llenan: tiles de plano, Modo 3 → heurística).
    uint8_t palette;
    /// : paleta AUTORADA del candidato elegido cuando no es la observada —
    /// el motor aproxima el color tintando por la razón CRAM viva
    /// observada/candidata. 0xFF = sin síntesis.
    uint8_t synth_pal;
    /// Referencia autorada del tinte E1 (promedio RGB 0-255 de la línea CRAM
    /// al capturar la pose). {0,0,0} = sin referencia → peak-hold escalar.
    uint8_t ref_rgb[3];
    /// Sub-rect UV del asset (0..1; 0,0,1,1 = quad completo). : los quads
    /// por grupo de paleta de una pose mixta recortan su porción del asset.
    float u0, v0, uw, vh;
    /// Identidad estable de la pose que emitió el sub (in-betweens §6.1/6.2):
    /// el TweenPlayer trackea instancias con ella. AL FINAL del struct (ABI:
    /// recompilar core+engine+lab juntos al cambiarlo).
    uint64_t pose_key;
    /// Vestuario: ruta/asset id de la máscara de TINTE del asset BASE de la
    /// pose ("" = sin máscara; blanco = la zona sigue el tinte de paleta,
    /// negro = sólo la luma). Sólo la llena el pose-sub cuando el asset
    /// elegido es el base. AL FINAL del struct (misma regla ABI que pose_key).
    char mask_path[256];
};

/// Retrieve all sprite occurrences from the last processed VRAM frame.
/// Returns entries written to out_buf (up to buf_cap).
uint32_t ayther_sprite_hasher_get_occurrences(const AytherSpriteHasher* h,
                                               AytherSpriteOccurrence*   out_buf,
                                               uint32_t                  buf_cap);

// ---------------------------------------------------------------------------
// Animation clips (C-S1) — ordered pose sequence + per-frame timing per anim
// group. The hasher detects looping cycles from the SAT slot histories and
// consolidates each into an ordered clip (the §4 "phase" of componentes-plan).
// The Lab's Animación workspace reads these as a starting point for authoring.
// ---------------------------------------------------------------------------

/// One frame (pose) of a detected clip. 16 bytes, align 8: pose(8) | duration(2) | pad(6).
struct AytherAnimFrame {
    uint64_t pose;       ///< pose hash (== a sprite occurrence hash / metasprite member)
    uint16_t duration;   ///< frames held (ticks), in phase with the game
};

/// Number of detected animation clips in the hasher's latest recompute.
uint32_t ayther_sprite_hasher_clip_count(const AytherSpriteHasher* h);

/// Reset the animation detector (clear slot history / groups / clips). Call before a
/// clip-generation run so the result reflects only the recording scanned (C-S5).
void ayther_sprite_hasher_reset_animation_grouper(AytherSpriteHasher* h);

/// Read animation clip `index`. Writes `out_id` (= anim_group_id) and
/// `out_looping` (0/1), and up to `frames_cap` frames into `out_frames` in order.
/// Returns the clip's frame count (may exceed `frames_cap` — grow and retry), or
/// 0xFFFFFFFF if `index` is out of range. Any out pointer may be NULL to skip it.
uint32_t ayther_sprite_hasher_get_clip(const AytherSpriteHasher* h,
                                        uint32_t          index,
                                        uint64_t*         out_id,
                                        uint8_t*          out_looping,
                                        AytherAnimFrame*  out_frames,
                                        uint32_t          frames_cap);

// ---------------------------------------------------------------------------
// SpriteSubstitutor — hash-to-HD-asset mapping engine  (v0.8.0)
// ---------------------------------------------------------------------------

struct AytherSpriteSubstitutor;  // opaque — do not dereference

/// Allocate a new SpriteSubstitutor.  Free with ayther_sprite_sub_free().
AytherSpriteSubstitutor* ayther_sprite_sub_new();

/// Destroy a SpriteSubstitutor.
void ayther_sprite_sub_free(AytherSpriteSubstitutor* s);

/// Load the substitution catalog from `sprite_substitutions.toml` in the pack.
void ayther_sprite_sub_load_pack(AytherSpriteSubstitutor* s, const AyArchive* pack);

/// Register a runtime override (hash → asset_path).
void ayther_sprite_sub_add_override(AytherSpriteSubstitutor* s,
                                     uint64_t                 hash,
                                     const char*              asset_path);

/// Override CON la referencia cromática E1 autorada (): `ref_rgb` apunta a
/// 3 bytes (promedio RGB de la línea CRAM al asignar) o es null (= [0,0,0] →
/// el tinte cae al peak-hold escalar gris).
void ayther_sprite_sub_add_override_ref(AytherSpriteSubstitutor* s,
                                        uint64_t                 hash,
                                        const char*              asset_path,
                                        const uint8_t*           ref_rgb);

/// Clear all runtime overrides.  Call at the start of each frame.
void ayther_sprite_sub_clear_overrides(AytherSpriteSubstitutor* s);

/// Resolve sprite occurrences to HD substitution instructions.
/// Returns the number of entries written to out_buf.
uint32_t ayther_sprite_sub_resolve(const AytherSpriteSubstitutor* s,
                                    const AytherSpriteOccurrence*  occs,
                                    uint32_t                       occ_count,
                                    AytherSpriteSub*               out_buf,
                                    uint32_t                       buf_cap);


// ---------------------------------------------------------------------------
// PoseSetSubstitutor — sustitución por FIRMA DE POSE (multi-sprite ANIMADO).
// Una pose = conjunto de hashes; sólo sustituye cuando TODOS están presentes (sin
// reclamar), en el bbox de los miembros, y los reclama. Resuelve DESPUÉS del
// metasprite y ANTES del per-sprite. `pose_substitutions.toml`: [[pose]] con el
// modelo COMPLETO (hashes + asset, y opcionales: rel/dims "x,y|…" = matching
// instanciado, max_w/max_h = guard, flip = cara del asset, ref = tinte E1
// autorado "r,g,b", [[pose.variant]] = candidatos paleta×flip). Un pack viejo
// (solo hashes/asset) carga con la semántica legacy por set.
// ---------------------------------------------------------------------------
struct PoseSetSubstitutor;  // opaque — do not dereference
PoseSetSubstitutor* ayther_pose_sub_new();
void                ayther_pose_sub_free(PoseSetSubstitutor* p);
/// Load `pose_substitutions.toml` from the pack. Returns the catalog entry count.
uint32_t ayther_pose_sub_load_pack(PoseSetSubstitutor* p, const AyArchive* pack);
/// Preview EN VIVO (Animar): agrega una pose-override (set de hashes → asset),
/// resuelta con prioridad sobre el catálogo y preservada al cargar un pack.
/// `rel_x`/`rel_y` (n elementos, paralelos; pueden ser null) = offsets relativos
/// de cada miembro → matching INSTANCIADO exacto (bbox 1:1, una sub por instancia).
/// `dim_w`/`dim_h` (n elementos, paralelos; pueden ser null) = tamaño en PX de
/// cada miembro: la tolerancia off-screen de un miembro AUSENTE necesita SUS dims
/// reales (sin ellas se aproximan con las del primer miembro visible y el match
/// cae en los bordes). `base_mirror` = cara en que está dibujado el asset respecto
/// de la capturada (bit0 H · bit1 V, flip de presentación de Posar): se XORea al
/// espejo del arreglo detectado.
void     ayther_pose_sub_add_override(PoseSetSubstitutor* p, const uint64_t* hashes,
                                      const int16_t* rel_x, const int16_t* rel_y,
                                      const int16_t* dim_w, const int16_t* dim_h,
                                      const uint8_t* mem_flips,   // : flips SAT por miembro o null
                                      uint32_t n, uint16_t max_w, uint16_t max_h,
                                      uint8_t base_mirror, const uint8_t* ref_rgb,
                                      const uint8_t* ref_lines,   // : 12 B (4 líneas × RGB) o null
                                      const char* asset,
                                      const char* mask);   // Vestuario (null/"" = sin máscara)
/// Como el anterior pero con CANDIDATOS por variante ( paso 2). `default_asset`
/// es el fallback; los candidatos van en arrays paralelos (paleta/hflip/vflip = int8,
/// -1 = cualquiera) + un array de punteros a rutas. Al matchear, el motor elige el
/// candidato más próximo a la variante observada del ancla.
void     ayther_pose_sub_add_override_variants(
             PoseSetSubstitutor* p, const uint64_t* hashes,
             const int16_t* rel_x, const int16_t* rel_y,
             const int16_t* dim_w, const int16_t* dim_h,
             const uint8_t* mem_flips,   // : flips SAT por miembro o null
             uint32_t n, uint16_t max_w, uint16_t max_h, uint8_t base_mirror,
             const uint8_t* ref_rgb,
             const uint8_t* ref_lines,   // : 12 B (4 líneas × RGB) o null
             const char* default_asset,
             const int8_t* var_pal, const int8_t* var_hf, const int8_t* var_vf,
             const uint16_t* var_slots,  // : bitmask de slots por candidato o null
             const uint64_t* var_sig,    // : firma de contenido por candidato o null
             const char* const* var_assets, uint32_t n_var,
             const char* mask);   // Vestuario (null/"" = sin máscara)
/// : CRAM viva del frame (words empaquetadas R0-2/G3-5/B6-8, ≥64 words) —
/// track de estabilidad por línea + latch de firmas de contenido en estado
/// estable. Llamar cada frame ANTES de ayther_pose_sub_resolve.
void     ayther_pose_sub_set_cram(PoseSetSubstitutor* p,
                                  const uint16_t* words, uint32_t n);
/// : firma de contenido de una línea — xxh3 de los colores crudos (9 bits)
/// de los `slots` marcados. La MISMA función del runtime; el Lab la llama al
/// capturar la variante (autoría y runtime no pueden divergir).
uint64_t ayther_palette_signature(const uint16_t* words, uint32_t n,
                                  uint8_t line, uint16_t slots);
/// Limpia todas las pose-overrides en vivo (no toca el catálogo del pack).
void     ayther_pose_sub_clear_overrides(PoseSetSubstitutor* p);
/// Área VISIBLE del display según el modo de video vivo (fb del frame; H32/H40 ×
/// V28/V30). Límite de la tolerancia a miembros off-screen del matching: un
/// ausente sólo se tolera si su rect esperado queda totalmente fuera de
/// [0,w)×[0,h). 0 = ignorado. Actualizar por frame (el modo puede cambiar).
void     ayther_pose_sub_set_screen(PoseSetSubstitutor* p, uint16_t w, uint16_t h);
/// Resolve pose-sets this frame. `claimed` (occ_count bytes, in/out) carries the
/// metasprite claims in and gets the pose-set claims OR'd in. Returns subs written.
uint32_t ayther_pose_sub_resolve(const PoseSetSubstitutor*     p,
                                 const AytherSpriteOccurrence* occs,
                                 uint32_t                      occ_count,
                                 uint8_t*                      claimed,
                                 AytherSpriteSub*              out_buf,
                                 uint32_t                      buf_cap);

// ---------------------------------------------------------------------------
// TweenPlayer v2 — in-betweens por TRANSICIÓN (§6.1/6.2). Filtra el HD ya
// resuelto POR INSTANCIA (tracks por pose_key + centro del bbox): al cambiar la
// POSE de un track hacia un target con transición autorada, reproduce los
// dibujos intermedios en los primeros frames del hold del destino. Escalera:
// par exacto (from→target) > comodín (target) > pop directo.
// `tween_sequences.toml`: [[tween]] target / from (opcional) / frames / ticks.
// ---------------------------------------------------------------------------
struct TweenPlayer;  // opaque — do not dereference
TweenPlayer* ayther_tween_new();
void         ayther_tween_free(TweenPlayer* p);
/// Load `tween_sequences.toml` from the pack. Returns the number of tween entries.
uint32_t     ayther_tween_load_pack(TweenPlayer* p, const AyArchive* pack);
/// Advance in-flight tween timers + expire stale instance tracks. Once per frame.
void         ayther_tween_begin_frame(TweenPlayer* p);
/// Given the resolved `target` HD and the sub's instance identity (pose_key +
/// bbox center in screen px), write into `out_buf` the HD to render this frame.
void         ayther_tween_resolve(TweenPlayer* p, const char* target,
                                  uint64_t pose_key, int32_t cx, int32_t cy,
                                  char* out_buf, uint32_t cap);
/// Drop all instance tracks (non-sequential seeks, pack/take loads) — without
/// this a replay scrub leaves ghost tweens.
void         ayther_tween_clear(TweenPlayer* p);
/// Live override (Lab channel, wins over the pack): from == NULL = wildcard.
void         ayther_tween_set_override(TweenPlayer* p, const char* from,
                                       const char* target, const char* const* frames,
                                       uint32_t n_frames, uint32_t ticks);
void         ayther_tween_clear_overrides(TweenPlayer* p);

// ---------------------------------------------------------------------------
// PackBuilder — assemble + sign a .ay pack in-process  (R8 Deliver)
//
// The Lab's Deliver workspace builds a signed pack without shelling out to the
// ay_pack CLI. Stage entries (manifest.toml, *_substitutions.toml, asset files),
// then finish() to write a dev-signed ZIP that AyArchive verifies on load.
//
// Ownership: caller creates with _new(), must release with _free().
// ---------------------------------------------------------------------------

struct AytherPackBuilder;  // opaque — do not dereference

/// Allocate a new pack builder. Free with ayther_pack_builder_free().
AytherPackBuilder* ayther_pack_builder_new();
void               ayther_pack_builder_free(AytherPackBuilder* b);

/// Stage an entry from raw bytes. Returns false on a null/invalid path
/// ("signature.bin" is reserved and rejected). Paths are normalised to '/'.
bool ayther_pack_builder_add_bytes(AytherPackBuilder* b, const char* path,
                                   const uint8_t* data, size_t len);

/// Stage an entry by reading source_fs_path into the pack at path_in_pack.
/// Returns false if the path is invalid or the source file can't be read.
bool ayther_pack_builder_add_file(AytherPackBuilder* b, const char* path_in_pack,
                                  const char* source_fs_path);

/// Number of entries staged (excludes the signature added by finish).
uint32_t ayther_pack_builder_count(const AytherPackBuilder* b);

/// Write the pack to out_path, dev-signing when `sign` is true. On failure
/// copies a message into err_buf (NUL-terminated, capped at err_cap) and
/// returns false.
bool ayther_pack_builder_finish(AytherPackBuilder* b, bool sign,
                                const char* out_path,
                                char* err_buf, size_t err_cap);

/// ASSET ID () — el nombre con el que un archivo del proyecto vive dentro
/// del pack: los 32 primeros hex del SHA-256 de su contenido, SIN extensión.
///
/// Está en el core y no acá porque tiene que ser el mismo digest que verifica
/// las entradas al leer: dos implementaciones del mismo hash es deriva
/// silenciosa esperando a pasar.
///
/// `out` necesita 33 bytes (32 hex + NUL). Devuelve false si el archivo no se
/// puede leer o el buffer no alcanza — eso es «este asset no entra», nunca un
/// nombre vacío.
bool ayther_asset_id(const char* fs_path, char* out, size_t cap);

/// Igual, pero sobre bytes en memoria: para el contenido GENERADO por el bake
/// (el SoundFont recortado), que no tiene archivo del que salir.
bool ayther_asset_id_bytes(const uint8_t* data, size_t len, char* out, size_t cap);

// ---------------------------------------------------------------------------
// AudioHasher — PCM batch fingerprinting  (v0.9.0)
//
// Fingerprints each retro_audio_sample_batch call using xxHash3-64 over the
// raw i16 stereo buffer.  Silent batches (all zeros) are skipped.
// Occurrence counts let the Lab "Audio" tab distinguish recurring SFX
// (high hits) from one-shot music frames (hits = 1).
//
// Call sequence per frame:
//   // inside retro_audio_sample_batch callback:
//   ayther_audio_hasher_process_batch(h, data, frames);
//
//   // once at the end of run_frame():
//   ayther_audio_hasher_end_tick(h);
//
// Ownership: caller creates with _new(), must release with _free().
// Thread safety: NOT thread-safe; drive from the emulation thread only.
// ---------------------------------------------------------------------------

struct AytherAudioHasher;  // opaque — do not dereference

/// Allocate a new AudioHasher.  Free with ayther_audio_hasher_free().
AytherAudioHasher* ayther_audio_hasher_new();

/// Destroy an AudioHasher.
void ayther_audio_hasher_free(AytherAudioHasher* h);

/// Fingerprint one stereo PCM batch from retro_audio_sample_batch.
/// data   — stereo-interleaved i16 buffer (L₀,R₀,L₁,R₁,…)
/// frames — number of stereo frames (libretro convention; buffer len = frames × 2)
/// Returns the xxHash3-64 of the batch, or 0 if silent (all zeros).
uint64_t ayther_audio_hasher_process_batch(AytherAudioHasher* h,
                                            const int16_t*     data,
                                            size_t             frames);

/// Snapshot this tick's occurrences and reset in-flight counters.
/// Call exactly once at the end of each run_frame() cycle.
void ayther_audio_hasher_end_tick(AytherAudioHasher* h);

/// Total unique PCM batch patterns accumulated since creation.
uint32_t ayther_audio_hasher_unique_count(const AytherAudioHasher* h);

// ---------------------------------------------------------------------------
// Audio substitution — POD structs  (v0.9.0)
// ---------------------------------------------------------------------------

/// One raw write to a sound chip's bus this frame, in temporal (bus) order.
/// Surfaced by the Ayther fork (ids 0x109/0x10A) as the basis for command-based
/// audio identity (replay-stable, unlike PCM output — see audio_event.rs).
/// Layout-identical to the fork's AytherAudioWrite and RetroRunner::AudioWrite.
struct AytherAudioWrite {
    uint32_t cycle;  ///< CPU M-cycle timestamp within the frame (timing diverges across replay — do NOT use for identity)
    /// FM: el REGISTRO YA LATCHEADO, 0x000-0x1FF (bit 0x100 = banco 1, o sea los
    /// canales 4-6). PSG: 0 — ese chip es de un byte y el latch va en el dato.
    ///
    /// NO es el índice de puerto del bus. Lo fue hasta el fork `3fc6ee89`
    /// (2026-08-11), que consolidó la telemetría de audio: antes el core mandaba
    /// el byte crudo con su puerto (0-3) y cada consumidor replicaba el
    /// protocolo de latch. Los tres que lo hacían —el detector, el espejo del
    /// router y el spike de re-síntesis— siguieron haciendo `addr & 3` después
    /// del cambio, y eso dejó al Lab MUDO y al detector sin ver un solo evento
    /// durante dos días, sin que ningún oráculo se quejara: los tres sintetizan
    /// sus escrituras de prueba con la misma convención que su consumidor, así
    /// que un cambio de convención los deja pasando en verde (2026-08-13).
    uint16_t addr;
    uint8_t  data;   ///< byte written to the chip bus
    uint8_t  chip;   ///< 0 = YM2612 (FM), 1 = SN76489 (PSG)
};

/// One active audio-event substitution this frame (C-A3b): an assigned event whose
/// window covers the current take frame. The motor mutes its channel; the playback
/// layer triggers `asset_path` (one-shot) when `is_start` to keep it in sync.
struct AytherAudioActiveSub {
    uint64_t    signature;   ///< event signature this asset is assigned to
    const char* asset_path;  ///< HD asset (session-owned; valid until reassign/clear)
    uint8_t     chip;        ///< 0 = FM, 1 = PSG
    uint8_t     channel;     ///< FM 0-5 | PSG 0-3
    uint8_t     is_start;    ///< 1 if the event starts on this frame (trigger the asset)
    uint8_t     _pad;
};

/// Per-tick PCM batch occurrence (after ayther_audio_hasher_end_tick).
struct AytherAudioOccurrence {
    uint64_t hash;
    uint32_t frame_count;  ///< stereo frames in the original batch
    uint32_t hits;         ///< times this hash appeared in the current tick
};

/// Resolved HD audio substitution: suppress the emulator batch, play asset.
struct AytherAudioSub {
    uint64_t hash;
    char     asset_path[256];  ///< logical path in the .ay pack (e.g. "audio/sfx/ring.wav")
    uint32_t frame_count;      ///< duration hint: stereo frames in the original batch
};

/// Fill out_buf with occurrences from the most-recently-completed tick.
/// Returns the number of entries written (up to buf_cap).
uint32_t ayther_audio_hasher_get_occurrences(const AytherAudioHasher*  h,
                                              AytherAudioOccurrence*    out_buf,
                                              uint32_t                  buf_cap);

// ---------------------------------------------------------------------------
// AudioEventDetector — eventos de audio por comandos al chip  (C-A2)
//
// Detecta el ciclo de vida (inicio/fin) de cada canal de sonido a partir del log
// de escrituras crudas del fork (AytherAudioWrite), produciendo bloques de
// actividad con una FIRMA estable (replay-estable, a diferencia del PCM). Se
// alimenta por frame; al final se cierra con _finish. Ver core/src/audio_event.rs.
//
// Ownership: el caller crea con _new(), libera con _free().
// ---------------------------------------------------------------------------
// Geometric tween (C-S1 Level 1) — glide the HD asset between keyframes.
// The bbox anchor a keyframe's HD is drawn at; interpolating it across the held
// ticks avoids the "pop" of one-HD-per-keyframe playback (modelo-de-autoria 2.3).
// ---------------------------------------------------------------------------
struct AytherTransform { float x, y, w, h; };   // == animation::Transform (16 B)

struct AytherGeometricTween;  // opaque — do not dereference

/// Build from parallel keyframe transforms + hold durations (n each); looping
/// wraps the last key to the first. NULL if n == 0. Free with the _free below.
AytherGeometricTween* ayther_geo_tween_new(const AytherTransform* transforms,
                                           const uint16_t* durs, size_t n, bool looping);
void            ayther_geo_tween_free(AytherGeometricTween* t);
uint32_t        ayther_geo_tween_duration(const AytherGeometricTween* t);
/// Interpolated transform at `tick` (wraps for a loop, clamps for a one-shot).
AytherTransform ayther_geo_tween_sample(const AytherGeometricTween* t, uint32_t tick);

// ---------------------------------------------------------------------------

struct AytherAudioEventDetector;  // opaque — do not dereference

/// El bit de un canal dentro de la máscara de silenciado (32 bits):
///
///   bits  0-5   FM  (YM2612)   0-5
///   bits  6-9   PSG (SN76489)  0-3
///   bits 10-17  PCM (RF5C164 de Sega CD) 0-7
///   bits 18-31  libres — el core los RECHAZA al escribir la región 0x10D
///
/// Vive acá, en el contrato, y no en cada llamador, porque hasta 2026-08-13
/// estuvo escrita a mano como `chip == 0 ? (1<<ch) : (1<<(6+ch))` en más de
/// veinte lugares entre el Engine y el Lab. Con un tercer chip esa forma manda
/// los ocho canales del PCM a la rama del PSG —bits 6 a 13—, pisando al PSG y
/// desbordando la máscara sin que nada falle.
///
/// Devuelve 0 para un chip que no participa de la máscara, que es una respuesta
/// legítima: significa «este canal no se puede silenciar por acá».
inline uint32_t ayther_chan_bit(uint8_t chip, uint8_t channel) {
    if (chip == 0 && channel < 6) return uint32_t(1u) << channel;
    if (chip == 1 && channel < 4) return uint32_t(1u) << (6 + channel);
    if (chip == 3 && channel < 8) return uint32_t(1u) << (10 + channel);
    return 0;
}

/// Todos los canales que la máscara sabe nombrar (18 bits). Es el «mutear todo»
/// de un solo: el valor anterior, 0x3FF, dejaba sonando al chip PCM entero.
inline constexpr uint32_t kAytherAllChannels = 0x3FFFFu;

/// Cuántos canales sabe nombrar la máscara — el largo de cualquier arreglo
/// indexado por `ayther_chan_index`.
inline constexpr int kAytherChanCount = 18;

/// El ÍNDICE de un canal en el orden canónico de la máscara (FM 1-6 · PSG 1-4 ·
/// PCM 1-8), o -1 si el chip no participa. Es el mismo orden de los bits, y por
/// eso el mismo que usan las lanes del timeline: un solo lugar decide en qué
/// fila va cada canal.
inline int ayther_chan_index(uint8_t chip, uint8_t channel) {
    if (chip == 0 && channel < 6) return channel;
    if (chip == 1 && channel < 4) return 6 + channel;
    if (chip == 3 && channel < 8) return 10 + channel;
    return -1;
}

/// Nombre corto del chip, para etiquetas de UI y de export.
inline const char* ayther_chip_name(uint8_t chip) {
    return chip == 0 ? "FM" : chip == 1 ? "PSG" : chip == 3 ? "PCM" : "?";
}

/// One detected audio event: a channel's activity span with a stable signature.
struct AytherAudioEvent {
    uint64_t signature;    ///< stable hash of the channel's register snapshot at key-on
    /// Identidad de instrumento: patch SIN frecuencia ni canal (DAC = la firma).
    /// La misma voz a través de notas/canales comparte instrument aunque la
    /// firma difiera — agrupa "el mismo sonido" para el export DAW (Mezclar).
    uint64_t instrument;
    uint32_t start_frame;  ///< frame of the key-on
    uint32_t end_frame;    ///< frame of the key-off (== start_frame for a 1-frame event)
    uint8_t  chip;         ///< 0 = YM2612 (FM), 1 = SN76489 (PSG), 3 = RF5C164 (PCM de Sega CD)
    uint8_t  channel;      ///< FM 0-5 | PSG 0-3 | PCM 0-7
    /// Nota MIDI al key-on (255 = sin altura: DAC/ruido PSG/fnum 0).
    /// Piano-roll / MIDI (Mezclar).
    uint8_t  pitch;
    /// «Velocidad» al key-on, escala MIDI 1-127 (0 = desconocida: DAC y
    /// residuales). En FM sale del Total Level del operador PORTADOR —el chip
    /// no tiene velocity—; en PSG, de la atenuación.
    ///
    /// Ocupa el byte que era `_pad`: sizeof sigue siendo 32 y ningún campo se
    /// mueve, así que el ABI no cambia. Es la mitad de la información que
    /// `instrument` deja afuera a propósito: el volumen no es identidad de
    /// timbre ().
    uint8_t  velocity;
};

// ---------------------------------------------------------------------------
// SoundFont — síntesis de la voz asignada a un timbre del juego  ()
//
// El sintetizador vive del lado RUST: `ayther_engine` es una lib ESTÁTICA, y
// uno LGPL (FluidSynth) obligaría a distribución dinámica o a entregar objetos
// relinkeables — la misma frontera que en  hizo elegir libvpx sobre FFmpeg.
// Del lado del core la frontera FFI ya existe, así que la pregunta desaparece.
// ---------------------------------------------------------------------------
struct AytherSf2;   // opaque — do not dereference

/// Abre un SoundFont desde bytes. NULL si no se pudo. Liberar con ayther_sf2_free.
AytherSf2* ayther_sf2_new(const uint8_t* data, size_t len, int32_t sample_rate);

/// Igual, pero comparte el SoundFont parseado entre instancias con la misma
/// `key` (el hash de su ruta). El motor crea una instancia POR TIMBRE para
/// poder REALZAR su ganancia escalando el buffer — CC 7 se acaba en 127 ().
AytherSf2* ayther_sf2_new_shared(uint64_t key, const uint8_t* data, size_t len,
                                 int32_t sample_rate);
/// Suelta los SoundFonts cacheados que ya no usa ninguna instancia.
void ayther_sf2_trim_cache(void);
void       ayther_sf2_free(AytherSf2* p);
void       ayther_sf2_program(AytherSf2* p, int32_t ch, int32_t preset);
/// Control Change de MIDI. CC 7 = volumen del canal (0-127, default 100): por
/// ahí va la GANANCIA por timbre — escalar el buffer no serviría porque un
/// mismo SoundFont sirve a varios timbres, cada uno en su canal.
void       ayther_sf2_control(AytherSf2* p, int32_t ch, int32_t cc, int32_t value);
void       ayther_sf2_note_on(AytherSf2* p, int32_t ch, int32_t key, int32_t vel);
void       ayther_sf2_note_off(AytherSf2* p, int32_t ch, int32_t key);
/// Corta todo de inmediato — para los cortes del juego y los seeks.
void       ayther_sf2_all_notes_off(AytherSf2* p);
/// Estéreo INTERCALADO f32: `out` tiene que tener `frames * 2` floats. Con `p`
/// nulo escribe SILENCIO (el llamador encola el buffer igual, y con basura
/// sonaría ruido blanco).
void       ayther_sf2_render(AytherSf2* p, float* out, size_t frames);
/// Presets de un SF2 SIN cargarlo en el sintetizador — para la biblioteca.
/// Devuelve el total (puede superar `cap`).
uint32_t   ayther_sf2_list_presets(const uint8_t* data, size_t len,
                                   uint16_t* out_bank, uint16_t* out_preset,
                                   uint32_t cap);
/// Idem pero desde una RUTA y CON NOMBRE, una línea por preset:
/// `bank:preset|nombre`. Elegir un timbre es leer nombres — una lista de «0:33»
/// no se puede recorrer.
///
/// Toma la ruta y no un buffer porque lee SÓLO el chunk `pdta` saltando por las
/// cabeceras RIFF: una biblioteca de 182 archivos con dos de ~1 GB no se puede
/// recorrer cargando cada uno entero. Funciona sobre SF2 parciales (no valida
/// instrumentos). Devuelve los bytes escritos, 0 si no entra o falla.
/// Acepta también `.sf3` (mismo pdta) y `.sfz` (un instrumento = una línea
/// `0:0|nombre`, sin tocar los samples).
size_t     ayther_sf2_preset_list(const char* path, uint8_t* out, size_t cap);

/// Normaliza un SoundFont de DISCO a SF2 plano en memoria: `.sf2` pasa
/// derecho, `.sf3` se descomprime (samples Vorbis → PCM) y `.sfz` se
/// convierte (texto + samples sueltos → un preset 0:0). Es LA puerta por la
/// que un formato nuevo entra — aguas abajo (sintetizador, horneado, pack)
/// todo sigue siendo SF2 plano. Dos llamadas como ayther_sf2_bake (`cap = 0`
/// consulta el tamaño); el resultado queda cacheado por ruta, así la
/// conversión no se paga dos veces. Devuelve 0 si no se pudo convertir.
///
/// Nota: ayther_sf2_new/new_shared y ayther_sf2_bake ya convierten SF3 por
/// detección de BYTES — esta función hace falta para `.sfz`, que necesita la
/// ruta (sus samples viven al lado del archivo de texto).
size_t     ayther_soundfont_normalize_file(const char* path,
                                           uint8_t* out, size_t cap);

/// Los SoundFonts que un `instruments.toml` referencia, uno por línea:
/// `basename|bank:preset,bank:preset,...`. Texto plano a propósito: cruzar un
/// vector de structs por el FFI para algo que se consume una vez al hornear no
/// vale la complejidad. Devuelve los bytes escritos.
size_t     ayther_instruments_soundfonts(const char* toml_text,
                                         uint8_t* out, size_t cap);

/// Hornea un SF2 recortado a los `(bank, preset)` pedidos. Devuelve los bytes
/// que ocupa el resultado; llamar con `cap = 0` para consultar el tamaño.
///
/// Existe porque un SF2 de origen puede pesar cientos de MB (en una colección
/// real hay uno de 988): nadie descarga esa biblioteca para usar 10-30 timbres
/// — es tamaño de DISTRIBUCIÓN (el pack ya abre lazy desde , la residencia
/// dejó de ser el motivo). Medido: 0,3% del original en un archivo de 97 MB.
size_t     ayther_sf2_bake(const uint8_t* src, size_t src_len,
                           const uint16_t* banks, const uint16_t* presets,
                           uint32_t n, uint8_t* out, size_t cap);

AytherAudioEventDetector* ayther_audio_event_new();
void     ayther_audio_event_free(AytherAudioEventDetector* d);
/// Región del reloj para la decodificación de pitch (0 = NTSC, 1 = PAL).
void     ayther_audio_event_set_pal(AytherAudioEventDetector* d, uint8_t pal);
/* Evidencia de audio p/ eventos residuales: canales que suenan al inicio de la
 * toma (bits 0-5 FM, 6-9 PSG; la sesión lo mide con una sonda de PCM). */
void     ayther_audio_event_set_initial_active(AytherAudioEventDetector* d, uint16_t mask);
void     ayther_audio_event_reset(AytherAudioEventDetector* d);
/// Ingest one frame's raw chip writes (bus order). `writes` is `n` AytherAudioWrite.
/// Equivalent to _process_frame_ex with no PCM events.
void     ayther_audio_event_process_frame(AytherAudioEventDetector* d, uint32_t frame,
                                          const AytherAudioWrite* writes, uint32_t n);

/// AytherPcmEvent::kind. Same values as ayther_audio_event_type_v1, so
/// translating from the core's event is the identity and cannot drift.
enum {
    AYTHER_PCM_KEY_ON  = 1,
    AYTHER_PCM_KEY_OFF = 2,
    AYTHER_PCM_PITCH   = 6,
    AYTHER_PCM_VOLUME  = 7
};

/// One typed event from the Sega CD RF5C164 () — the unpacked {reg, data}
/// of an `ayther_audio_event_v1` with source == PCM. This chip has no bus the
/// core exposes, so it never arrives as AytherAudioWrite.
///
/// `st`/`ls` locate the waveform in Wave RAM and are what says WHICH SAMPLE
/// plays; `fd` is playback rate and `env` volume, and neither identifies a
/// sound on its own. Layout-identical to audio_event::PcmEvent (10 bytes).
struct AytherPcmEvent {
    uint8_t  kind;     ///< AYTHER_PCM_*
    uint8_t  channel;  ///< 0-7
    uint8_t  env;      ///< envelope multiplier (volume)
    uint8_t  pan;
    uint8_t  st;       ///< ST register: Wave RAM start address
    uint8_t  _pad;
    uint16_t ls;       ///< loop address
    uint16_t fd;       ///< 5.11 address increment (rate, NOT a note)
};

/// Ingest one frame by BOTH audio paths: raw FM/PSG writes and typed PCM
/// events. One call per frame so the two share a frame number.
void     ayther_audio_event_process_frame_ex(AytherAudioEventDetector* d, uint32_t frame,
                                             const AytherAudioWrite* writes, uint32_t n,
                                             const AytherPcmEvent* pcm, uint32_t m);
void     ayther_audio_event_finish(AytherAudioEventDetector* d);
uint32_t ayther_audio_event_count(const AytherAudioEventDetector* d);
/// Fill out_buf with up to buf_cap events; returns the number written.
uint32_t ayther_audio_event_get(const AytherAudioEventDetector* d,
                                AytherAudioEvent* out_buf, uint32_t buf_cap);

// ---------------------------------------------------------------------------
// BatchEventDetector — eventos por HASHES DE BATCH de PCM (C-A1, complemento
// del detector por comandos de arriba). Corre sobre el historial de audio de
// una toma (.arp v7, un push POR FRAME: primer hash o 0 = silencio) sin
// necesitar el log de escrituras del fork. Emite el MISMO AytherAudioEvent
// (chip = 255 → mezcla, sin canal). No separa sonidos solapados en la mezcla.
// ---------------------------------------------------------------------------

struct AytherBatchEventDetector;  // opaque — do not dereference

AytherBatchEventDetector* ayther_audio_evdet_new();
void     ayther_audio_evdet_free(AytherBatchEventDetector* d);
/// Toggle re-attack splitting (default on): un retrigger sin silencio (la
/// cabeza determinista reaparece) corta el run en dos instancias.
void     ayther_audio_evdet_set_split_on_reattack(AytherBatchEventDetector* d, bool on);
/// Alimentar un hash de batch (0 = silencio) — una vez por frame de la toma.
void     ayther_audio_evdet_push(AytherBatchEventDetector* d, uint64_t hash);
/// Cerrar el run en vuelo (fin de la toma).
void     ayther_audio_evdet_flush(AytherBatchEventDetector* d);
uint32_t ayther_audio_evdet_event_count(const AytherBatchEventDetector* d);
/// Copia eventos a out_buf (hasta cap); devuelve el total (crecer y reintentar).
uint32_t ayther_audio_evdet_get_events(const AytherBatchEventDetector* d,
                                       AytherAudioEvent* out_buf, uint32_t cap);

/// Un canal key-on AHORA con su firma (sustitución EN VIVO, runtime).
///  F3: instrument/pitch viajan con la voz (capturados al key-on) — el
/// runtime resuelve las reglas de match por instrumento sin esperar a que el
/// evento cierre. 24 bytes.
struct AytherAudioActive {
    uint64_t signature;
    uint64_t instrument; ///< fm_instrument/psg_instrument (0 = desconocido)
    uint8_t  chip;       ///< 0 = FM, 1 = PSG
    uint8_t  channel;    ///< FM 0-5 | PSG 0-3
    uint8_t  pitch;      ///< nota MIDI al key-on; 255 = sin altura
    uint8_t  _pad[5];
};
/// Canales activos ahora (out hasta cap); devuelve la cantidad.
uint32_t ayther_audio_event_active(const AytherAudioEventDetector* d,
                                   AytherAudioActive* out_buf, uint32_t buf_cap);
/// Vacía los eventos cerrados (uso en vivo; no toca el estado de canales).
void     ayther_audio_event_clear_events(AytherAudioEventDetector* d);

// ---------------------------------------------------------------------------
// audio_events.toml — catálogo de sustituciones por evento (C-A5)
// Persistencia firma→asset(+canales) para guardar/cargar proyecto y entrega .ay.
// ---------------------------------------------------------------------------

struct AytherEventSub {
    uint64_t signature;
    char     asset[256];   ///< asset HD (logical path)
    uint32_t channels;     ///< máscara de canales (ver ayther_chan_bit); 0 = re-derivable
    /// SECUENCIA (Mezclar): campos en el viejo _pad[6].
    uint8_t  looping;      ///< 1 = el HD lupea hasta cerrar la ventana
    uint8_t  _pad;
    uint32_t duration_frames;  ///< ventana en frames (0 = sub per-evento clásica)
    ///  F3: regla de match — sizeof 288 (era 272; ambos lados en este repo).
    uint64_t match_instrument; ///< identidad del timbre de la regla (0 sin regla)
    uint8_t  match_rule;       ///< 0 exacta (legacy) · 1 instrumento · 2 instr+nota
    uint8_t  match_pitch;      ///< nota MIDI de la regla 2 (255 = sin altura)
    /// : bus del sonido — 0 sin clasificar · 1 música · 2 efectos · 3 voces.
    ///
    /// Sale de uno de los bytes de relleno que ya estaban, así que el layout NO
    /// cambia: un binario viejo lee 0 donde antes leía relleno, y 0 es
    /// exactamente lo que significa «este pack no lo dijo».
    uint8_t  bus;
    uint8_t  _pad2[5];
};

/// Formatea `subs` (n entradas) a texto audio_events.toml en `out` (con nul) si
/// cabe. Devuelve la longitud SIN nul; si > out_cap no escribe (reintentar mayor).
uint32_t ayther_audio_events_format(const AytherEventSub* subs, uint32_t n,
                                    char* out, uint32_t out_cap);
/// Parsea texto audio_events.toml → out (hasta cap). Devuelve la cantidad escrita.
uint32_t ayther_audio_events_parse(const char* text, AytherEventSub* out, uint32_t cap);

// ---------------------------------------------------------------------------
// AudioSubstitutor — hash-to-HD-asset mapping engine  (v0.9.0)
//
// Two sources of substitution (same priority model as SpriteSubstitutor):
//   1. catalog  — loaded from audio_substitutions.toml at startup.
//   2. overrides — set per-frame by Lua ayther.audio.replace() (beats catalog).
//
// Ownership: caller creates with _new(), must release with _free().
// ---------------------------------------------------------------------------

struct AytherAudioSubstitutor;  // opaque — do not dereference

/// Allocate a new AudioSubstitutor.  Free with ayther_audio_sub_free().
AytherAudioSubstitutor* ayther_audio_sub_new();

/// Destroy an AudioSubstitutor.
void ayther_audio_sub_free(AytherAudioSubstitutor* s);

/// Load substitution catalog from audio_substitutions.toml inside the pack.
/// TOML schema (same [[sub]] convention as tiles/sprites):
///   [[sub]]
///   hash  = "0x0123456789abcdef"
///   asset = "audio/sfx/ring_pickup.wav"
void ayther_audio_sub_load_pack(AytherAudioSubstitutor* s, const AyArchive* pack);

/// Register a runtime override (e.g. from Lua ayther.audio.replace()).
void ayther_audio_sub_add_override(AytherAudioSubstitutor* s,
                                    uint64_t                hash,
                                    const char*             asset_path);

/// Clear all runtime overrides.  Call at the start of each tick.
void ayther_audio_sub_clear_overrides(AytherAudioSubstitutor* s);

/// Number of entries loaded from the TOML catalog.
uint32_t ayther_audio_sub_catalog_len(const AytherAudioSubstitutor* s);

/// Resolve audio occurrences to HD substitution instructions.
/// Returns the number of entries written to out_buf.
uint32_t ayther_audio_sub_resolve(const AytherAudioSubstitutor* s,
                                   const AytherAudioOccurrence*  occs,
                                   uint32_t                      occ_count,
                                   AytherAudioSub*               out_buf,
                                   uint32_t                      buf_cap);

// --- Audio event substitution  (C-A2) --------------------------------------

/// A resolved HD substitution for a whole audio event: mute the emulator over
/// [start_frame, end_frame] and play asset_path aligned to start_frame.
struct AytherAudioEventSub {
    uint64_t signature;
    uint64_t start_frame;
    uint64_t end_frame;
    char     asset_path[256];
    uint8_t  looping;
};

/// Bind an HD asset to an event signature (authoring assign; persists).
void ayther_audio_sub_add_event_override(AytherAudioSubstitutor* s,
                                          uint64_t signature,
                                          const char* asset_path,
                                          uint8_t looping);
void     ayther_audio_sub_clear_event_overrides(AytherAudioSubstitutor* s);
uint32_t ayther_audio_sub_event_catalog_len(const AytherAudioSubstitutor* s);
/// Resolve events (from ayther_audio_evdet_get_events) → substitutions.
uint32_t ayther_audio_sub_resolve_events(const AytherAudioSubstitutor* s,
                                          const AytherAudioEvent* events,
                                          uint32_t event_count,
                                          AytherAudioEventSub* out_buf,
                                          uint32_t buf_cap);

// ---------------------------------------------------------------------------
// Lua shader API  (v0.9.4)
// ---------------------------------------------------------------------------

/// Shader parameters written by `ayther.shader.set_param()` from Lua.
/// All values in [0, 1].  Default: crt_strength=0.0 (passthrough, no effect).
struct AytherShaderParams {
    float crt_strength;   ///< overall CRT effect weight
    float scan_strength;  ///< scanline darkness
    float vignette;       ///< edge-darkening intensity
};

/// Read shader parameters from the Lua script environment.
/// Writes one AytherShaderParams to *out.
/// Pass the result as push constants to VkPostProcess::apply().
void ayther_script_get_shader_params(const AytherScriptEnv* env,
                                      AytherShaderParams*    out);

// ---------------------------------------------------------------------------
// Fondos — BackgroundStitcher + ScrollUnwrapper  (core/src/background.rs)
//
// Reconstruct a scrolling plane into a full level strip (no single frame holds
// it: the nametable wraps). observe() each visible cell in LEVEL space; the
// caller turns on-screen cells into absolute level tiles using the VDP scroll,
// unwrapping the wrapped Hscroll via ScrollUnwrapper (game-agnostic). Validated
// against Sonic 2 EHZ in tools/background_spike (0 conflicts over 1033px of scroll).
// The engine-side consumer is BackgroundExporter (ayther_background_export.h).
// ---------------------------------------------------------------------------
struct AytherBgStitcher;  // opaque

AytherBgStitcher* ayther_bg_stitcher_new();
void   ayther_bg_stitcher_free(AytherBgStitcher* s);
/// Record one visible cell at absolute level-tile (lx, ly) on `plane` (0/1/2).
/// `cell` is the opaque code (nametable word & 0x7FFF: pattern|flips|palette).
void   ayther_bg_stitcher_observe(AytherBgStitcher* s, uint8_t plane,
                                  int32_t lx, int32_t ly, uint32_t cell);
size_t ayther_bg_stitcher_cell_count(const AytherBgStitcher* s, uint8_t plane);
size_t ayther_bg_stitcher_conflicts(const AytherBgStitcher* s, uint8_t plane);
size_t ayther_bg_stitcher_animated_cells(const AytherBgStitcher* s, uint8_t plane);
/// Fill out4 = {min_x, min_y, max_x, max_y} (level tiles). False if plane empty.
bool   ayther_bg_stitcher_bounds(const AytherBgStitcher* s, uint8_t plane, int32_t* out4);
/// Read the code at level cell (x, y) into *out_code. False if never seen.
bool   ayther_bg_stitcher_get(const AytherBgStitcher* s, uint8_t plane,
                              int32_t x, int32_t y, uint32_t* out_code);

struct AytherScrollUnwrapper;  // opaque — one per plane axis

AytherScrollUnwrapper* ayther_scroll_unwrapper_new(int32_t period);
void    ayther_scroll_unwrapper_free(AytherScrollUnwrapper* u);
/// Feed this frame's wrapped scroll ([0, period)); returns the absolute camera.
int64_t ayther_scroll_unwrapper_push(AytherScrollUnwrapper* u, int32_t wrapped);
/// : delta (px) del ultimo push — |delta| no fisico (> ~32 px/frame) =
/// corte de escena; el caller congela la acumulacion del stitch.
int32_t ayther_scroll_unwrapper_last_step(const AytherScrollUnwrapper* u);

// ---------------------------------------------------------------------------
// Game profile — Mode 3 (RAM anchoring) input plumbing  (core/src/game_profile.rs)
//
// A TOML anchor profile declares where entities live in game RAM (base + X/Y
// offsets + count/stride + match box). `entities()` reads their world positions
// (handling the 68k word-swap); `assign()` maps this frame's SAT sprites to them
// using the VDP camera scroll — the exact per-instance identity Mode 2's visual
// clustering can't produce for two identical entities. See Mode3Resolver
// (ayther_mode3.h) for the engine-side consumer.
//
// Ownership: caller creates with _load(), must release with _free().
// ---------------------------------------------------------------------------
struct AytherGameProfile;  // opaque — do not dereference

/// Load a profile from a TOML file. NULL on read/parse error.
AytherGameProfile* ayther_game_profile_load(const char* toml_path);
/// : el caso pack — game_profile.toml vive ADENTRO del .ay y llega como
/// string, nunca como archivo. NULL en error de parse.
AytherGameProfile* ayther_game_profile_load_str(const char* toml);
void     ayther_game_profile_free(AytherGameProfile* p);

/// Read the profile's active entities from `ram` into parallel arrays
/// (id / world_x / world_y) up to `cap`. Returns the number written.
size_t   ayther_game_profile_entities(const AytherGameProfile* p,
                                       const uint8_t* ram, size_t ram_size,
                                       uint64_t* out_id, int32_t* out_wx,
                                       int32_t* out_wy, size_t cap);

/// Assign this frame's SAT sprites (screen-space top-lefts) to the profile's
/// entities using the plane camera `scroll_x/y` and size `plane_w/h` (world→
/// screen wraps mod the plane). `out_ent_id[j]` = the id claiming sprite j, or 0.
/// Returns the number of active entities considered.
size_t   ayther_game_profile_assign(const AytherGameProfile* p,
                                     const uint8_t* ram, size_t ram_size,
                                     int32_t scroll_x, int32_t scroll_y,
                                     int32_t plane_w, int32_t plane_h,
                                     const int16_t* spr_x, const int16_t* spr_y,
                                     size_t n_spr, uint64_t* out_ent_id);

/// Anchor-kind introspection (map a resolved instance id → kind → HD asset).
size_t   ayther_game_profile_kind_count(const AytherGameProfile* p);
int32_t  ayther_game_profile_kind_of_id(const AytherGameProfile* p, uint64_t id);
/// Copy kind `idx`'s name into `buf` (NUL-terminated, up to `cap`); returns the
/// name's byte length (grow and retry if it exceeds cap-1), or 0.
size_t   ayther_game_profile_kind_name(const AytherGameProfile* p, size_t idx,
                                        char* buf, size_t cap);

#ifdef __cplusplus
}  // extern "C"
#endif

// ---------------------------------------------------------------------------
// ayther_api.h — COPIA del contrato del Core Fork. NO editar a mano.
//
// AYTHER_API_VERSION: v1.10 — sincronizar con davidlazarte/Genesis-Plus-GX
//                     core/ayther/ayther_api.h (release ayther-abi-1.10,
//                     core v1.7.4 752a6ff7; integration contract:
//                     docs/EMULATOR_EXTENSION_ABI.md)
//
// COPIA ENTERA desde 1.9 (2026-08-26; 1.10 el mismo dia). Hasta 1.3-r2 se traia un recorte «solo
// lo que se consume»; con seis versiones aditivas encima (SYSTEM, controles en
// Mode 4, LINE_STATE, observabilidad, SPRITE_OUTCOME, Z80_RAM) el recorte ya
// no ahorra nada y esconde lo que hay para descubrir. Declarar un tipo NO es
// consumirlo: lo que el Engine LEE lo dicen sus suscripciones (pide solo los
// bits que consume, no AYTHER_SUB_ALL) y sus chequeos de capability.
//
// Three semantic changes from 1.3 to 1.9 did not change signatures; see
// docs/EMULATOR_EXTENSION_ABI.md#semantic-changes-through-abi-110:
//  1. 0x10E / fallback_reasons en Mode 4 dice la verdad (ya no arranca en
//     UNSUPPORTED_MODE; el Z80 marca CRAM/VRAM a mitad de frame). El Engine
//     nunca tuvo un «es SMS → fallback siempre», asi que solo hay que seguir
//     confiando en la mascara.
//  2. `v_counter` del journal = primera linea que VE el cambio (N+1). El
//     Engine no lo usa.
//  3. `recompose_multilayer` con eventos de CRAM cambia de salida (la nueva es
//     la correcta, pixel-perfect). Medido con abi_multilayer.
//
// Se copia en vez de referenciar el repo del fork desde CMake: un path
// cross-repo ataria el build del Engine al checkout del core, y el contrato
// es justamente lo que permite que los dos repos evolucionen por separado.
// Al actualizarlo, re-copiar entero y anotar el commit de origen arriba.
//
//  (2026-08-12): el contrato SI cambia — entra `ayther_recompose_status`
// con los cuatro motivos de rechazo de la recomposicion. Antes cualquier
// rechazo salia como UNSUPPORTED y no habia motivo que loguear. Van en -20..-23
// y no en el rango natural porque los `AYTHER_RC_ERR_*` INTERNOS del core ya
// ocupan -1..-4, donde viven INVALID_ARGUMENT y compania: devolverlos crudos
// haria que «no es modo 5» se leyera como «argumento invalido». Es aditivo —
// esos valores no se devolvian antes— asi que un caller que solo compara contra
// OK no cambia de comportamiento.
//
//  (2026-08-12): dos defectos que hacian que dos campos de este header
// publicaran datos falsos: `dirty_patterns` llegaba siempre vacio (fork ) y
// `raster_event_count` estaba clavado en 256 (fork ). Y `cram` se leia con
// `movaps` sin estar alineado a 16, asi que `ayther_recompose_multilayer`
// CRASHEABA el proceso con -O2 (fork ). Medido en `abi_frame_delta` y
// `abi_multilayer`.
//
//  (2026-08-12): trae FRAME_DELTA_V1 (poll_frame_delta + dirty_patterns),
// el export directo ayther_recompose_multilayer con sus AYTHER_RC_ERR_*, y el
// Audio Probe v2. OJO: `ayther_audio_event_v1` paso sus campos a una UNION y
// por lo tanto CAMBIO DE TAMANO — el tamano tiene que salir de `event_size` del
// propio core y no de un sizeof local.
//
// 2026-08-24, ABI 1.2: la MULTICAPA SE MUDO AL DESCRIPTOR. El simbolo suelto
// `ayther_recompose_multilayer` ya no se exporta en el perfil standard, asi que
// resolverlo con GetProcAddress devuelve NULL contra un core >= 1.2 y la feature
// queda muda SIN un error a la vista. Se resuelve con AYTHER_IFACE_HAS y se cae
// al export suelto para los cores 1.0/1.1, que siguen valiendo: la ABI es
// aditiva y nada obliga a actualizar el binario.
//
//  (2026-08-12): el AUDIO_EVENT sube a schema 2 — el key-on de PCM ahora
// lleva `st`/`ls`, que es lo unico que dice QUE SAMPLE suena (env es volumen y
// fd es velocidad de reproduccion: ninguno identifica nada). De paso la fuente
// PCM deja de mezclar las dos ramas de la union y usa {reg,data} siempre. Este
// es el camino por el que entra el chip PCM de Sega CD al detector, que sigue
// siendo el DUENO de la identidad: el core da los HECHOS (st, ls, fd, env, pan)
// y `audio_event.rs` calcula la firma, igual que con FM y PSG. Ver el comentario
// de la union para el empaquetado exacto.
// ---------------------------------------------------------------------------
/*
 * AYTHER public core interface
 *
 * This header is the versioned, in-process contract between the AYTHER fork
 * and a frontend. It deliberately does not include libretro headers.
 */

#ifndef AYTHER_API_H
#define AYTHER_API_H

#include <stdint.h>
#include <stddef.h>   /* offsetof, para AYTHER_IFACE_HAS */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define AYTHER_CALL __cdecl
#else
#define AYTHER_CALL
#endif

#if defined(AYTHER_CORE_EXPORTS)
#  if defined(_WIN32) || defined(__CYGWIN__) || defined(__MINGW32__)
#    if defined(__GNUC__)
#      define AYTHER_API __attribute__((__dllexport__))
#    else
#      define AYTHER_API __declspec(dllexport)
#    endif
#  elif defined(__GNUC__) && (__GNUC__ >= 4)
#    define AYTHER_API __attribute__((__visibility__("default")))
#  else
#    define AYTHER_API
#  endif
#else
#  define AYTHER_API
#endif

#define AYTHER_ABI_VERSION_1_0 UINT32_C(0x00010000)
/* 1.1 (): agrega `recompose_stats_size` + `get_recompose_stats` al final del
 * descriptor. Es un cambio ADITIVO: el descriptor es uno solo y `struct_size`
 * dice hasta dónde llega, así que un cliente compilado contra 1.0 lo sigue
 * usando sin cambios — pide 1.0, recibe este mismo puntero y nunca lee más allá
 * de su propio sizeof. La regla para el cliente es "major igual, minor >= el que
 * necesito", no "version == la mía". */
#define AYTHER_ABI_VERSION_1_1 UINT32_C(0x00010001)
/* 1.2 (): `recompose_multilayer` entra al descriptor. Era el unico simbolo
 * AYTHER exportado ademas de `ayther_get_interface`, y contradecia el principio
 * de un solo punto de entrada versionado: un consumidor tenia que resolverlo
 * por nombre, sin `struct_size` ni capability que le dijeran si estaba. El
 * export directo sobrevive solo en el perfil legacy. */
#define AYTHER_ABI_VERSION_1_2 UINT32_C(0x00010002)
/* 1.3 (): region ATTRIBUTION + suscripcion propia + capability. Aditiva. */
#define AYTHER_ABI_VERSION_1_3 UINT32_C(0x00010003)
#define AYTHER_ABI_VERSION_1_4 UINT32_C(0x00010004)
/* 1.5 (.B): region SYSTEM. Aditiva: un descriptor de solo lectura con lo
 * que hoy el frontend tiene que deducir decodificando registros del VDP -- y
 * decodificarlos es reimplementar el core afuera del core, con las reglas
 * duplicadas y sin nadie que avise cuando se separan. */
#define AYTHER_ABI_VERSION_1_5 UINT32_C(0x00010005)
/* 1.6 (): estado del VDP POR SCANLINE. Aditiva: dos regiones nuevas, una
 * suscripcion propia y una capability. El frontend podia deducir el scroll de
 * una linea leyendo la tabla de hscroll, pero no la CRAM vigente EN esa linea:
 * un efecto raster de paleta solo existe mientras el frame se dibuja, y para
 * cuando la ABI se puede consultar ya termino. */
#define AYTHER_ABI_VERSION_1_6 UINT32_C(0x00010006)
/* 1.7 ( A/D/E): tres regiones de solo lectura que cierran huecos que el
 * frontend hoy tapa reimplementando el core afuera del core.
 *
 *   JOURNAL  -- el journal raster ya se llenaba; lo unico que se exponia era
 *               su CANTIDAD. Saber que hubo 17 eventos y no cuales es saber
 *               que el frame tiene splits y nada mas.
 *   FRAME_HASH -- para detectar desincronizacion habia que serializar el
 *               estado entero y hashearlo afuera: ~1 MB por comparacion.
 *   PALETTE  -- la conversion 9-bit -> formato del build, con shadow y
 *               highlight ya aplicados, es una tabla que el core arma igual;
 *               el frontend la rehacia a mano y con sus propias reglas.
 *
 * Aditiva: ninguna estructura existente cambia de tamanio ni de orden. */
#define AYTHER_ABI_VERSION_1_7 UINT32_C(0x00010007)
/* 1.8 (.C): resultado de render por sprite. Aditiva: una region nueva,
 * indexada por slot de la SAT.
 *
 * El issue pedia ampliar `ayther_sprite_v1` con los bits de descarte. No se
 * hizo asi a proposito: ese struct viaja por la ABI con su tamanio anunciado
 * en el descriptor, y un consumidor de 1.0 que lo transcribio con sus propios
 * tipos -- que es exactamente lo que hace `tests/ci/abi_compat_1_0.c`--
 * leeria el array corrido a partir del segundo elemento. Una region paralela
 * dice lo mismo sin romper a nadie. */
#define AYTHER_ABI_VERSION_1_8 UINT32_C(0x00010008)
/* 1.9 (): la RAM del Z80. Aditiva: una region nueva, nada cambia de
 * tamanio ni de orden.
 *
 * POR QUE HACE FALTA. El 68k y el Z80 se reparten el sonido, y varios juegos
 * dejan el id del tema a tocar en la RAM del Z80 (0xA00000-0xA01FFF desde el
 * bus del 68k) y no en work RAM. Sin esta region, una herramienta que quiera
 * disparar un sonido por id -- grabar un tema limpio desde el Sound Test--
 * puede BUSCAR la casilla en work RAM, descartar los candidatos y quedarse
 * sin donde mirar. Paso en Golden Axe: los dos candidatos de work RAM se
 * descartaron por confirmacion automatica, y la unica hipotesis que quedaba
 * era esta RAM, que no se exponia.
 *
 * ESCRIBIBLE, y ahi esta el cuidado: escribir la RAM del Z80 mientras el Z80
 * corre es una carrera. El consumidor tiene que hacerlo con el bus tomado, o
 * aceptar que lo que escribio puede durar un frame -- que para disparar un
 * sonido por id alcanza, y para cualquier otra cosa no. */
#define AYTHER_ABI_VERSION_1_9 UINT32_C(0x00010009)
/* 1.10 (): SYSTEM dice de que frame habla. Aditiva: el byte reservado del
 * descriptor pasa a ser `flags`, mismo offset y tamanio.
 *
 * `viewport_w/h` e `interlace` describen el frame EMITIDO, porque el core
 * aplica la geometria de los registros al inicio del frame siguiente
 * (system_frame_gen, bloque `changed & 2`). `vdp_mode` y `shadow_highlight`
 * salen de los registros AHORA. `h40` salia de reg 12 y se contradecia con
 * `viewport_w` en el frame de transicion -- Sonic 2 escribe H40 durante el
 * primer frame, que sale con la geometria de reset: `h40 = 1` junto a
 * `viewport = 256x192`, en el mismo struct--. Ahora `h40` describe el frame
 * emitido, como `viewport_w`, y AYTHER_SYSTEM_GEOMETRY_PENDING dice que los
 * registros ya cambiaron la geometria y el frame que viene la aplica. */
#define AYTHER_ABI_VERSION_1_10 UINT32_C(0x0001000A)
#define AYTHER_ABI_VERSION_LATEST AYTHER_ABI_VERSION_1_10

/* La version mas nueva, COMO TEXTO, derivada y no copiada.
 *
 * El `ayther_build_id` la traia escrita a mano y ya se desincronizo una vez
 * (: decia 1.3 con el header en 1.7). El comentario que quedo de ese
 * arreglo dice «si cambia la version, se cambia aca tambien» -- y eso es
 * precisamente lo que no se puede garantizar. Al agregar 1.9 volvio a pasar:
 * el string seguia diciendo 1.8.
 *
 * Derivarlo lo hace imposible: el numero y el texto salen de las mismas dos
 * constantes, asi que no hay dos lugares que puedan discrepar. */
#define AYTHER_ABI_VERSION_MAJOR_LATEST 1
#define AYTHER_ABI_VERSION_MINOR_LATEST 10
#define AYTHER__STR2(x) #x
#define AYTHER__STR(x)  AYTHER__STR2(x)
#define AYTHER_ABI_VERSION_LATEST_STR    AYTHER__STR(AYTHER_ABI_VERSION_MAJOR_LATEST) "."    AYTHER__STR(AYTHER_ABI_VERSION_MINOR_LATEST)

#define AYTHER_ABI_VERSION_MAJOR(v) ((uint32_t)(v) >> 16)
#define AYTHER_ABI_VERSION_MINOR(v) ((uint32_t)(v) & UINT32_C(0xFFFF))

#define AYTHER_GENERATION_ANY UINT64_MAX
#define AYTHER_LEGACY_MEMORY_NONE UINT32_MAX

/* ayther_get_interface(0) returns the latest supported interface. An explicit
 * unsupported version returns NULL. A missing symbol identifies a stock or
 * pre-ABI AYTHER core and must be treated as zero capabilities. */

enum ayther_status
{
  AYTHER_STATUS_OK                = 0,
  AYTHER_STATUS_INVALID_ARGUMENT  = -1,
  AYTHER_STATUS_NOT_FOUND         = -2,
  AYTHER_STATUS_BUFFER_TOO_SMALL  = -3,
  AYTHER_STATUS_OUT_OF_BOUNDS     = -4,
  AYTHER_STATUS_READ_ONLY         = -5,
  AYTHER_STATUS_STALE_GENERATION  = -6,
  AYTHER_STATUS_BUSY              = -7,
  AYTHER_STATUS_UNSUPPORTED       = -8,
  AYTHER_STATUS_NOT_SUBSCRIBED    = -9,
  /* : `frame_delta_since` pidio una generacion que ya salio del ring.
     El `out` viene con TODO marcado sucio, que es la respuesta correcta y
     conservadora: no se sabe que cambio, asi que hay que asumir que todo.
     Es un aviso, no un fallo -- el consumidor puede seguir. */
  AYTHER_STATUS_DELTA_HISTORY_LOST = -10,
  /* : el control existe, pero no en el modo de video que corre ahora.

     Es distinto de UNSUPPORTED -- que dice "esta build no lo tiene"-- y de
     NOT_SUBSCRIBED -- "no lo pediste"-. Este dice "pedilo de nuevo cuando el
     VDP este en Mode 5", y es informacion que el frontend puede usar: hasta
     aca, escribir sprite_suppress con un juego de Master System devolvia OK y
     no hacia absolutamente nada. Un exito que no hace nada es peor que un
     error: el frontend cree que oculto el sprite y dibuja su reemplazo encima
     del original. */
  AYTHER_STATUS_UNSUPPORTED_MODE  = -11
};

/* Motivos INTERNOS del recompositor. Sus valores colisionan con los de
 * `ayther_status` (-1..-4), asi que no se devuelven crudos por la ABI: el
 * wrapper los traduce a los AYTHER_STATUS_RC_* de abajo. */
enum ayther_recompose_error
{
  AYTHER_RC_ERR_NOT_MODE5 = -1,
  AYTHER_RC_ERR_INTERLACE2 = -2,
  AYTHER_RC_ERR_NTSC_FILTER = -3,
  AYTHER_RC_ERR_INVALID_PARAMS = -4,
  /* : el journal raster del frame desbordo. Reproducir el prefijo que si
     entro daria una imagen plausible y equivocada, asi que la recomposicion se
     declara incapaz en vez de devolver un exito parcial. */
  AYTHER_RC_ERR_JOURNAL_OVERFLOW = -5
};

/* Por que una recomposicion no se pudo hacer, con nombre propio.
 *
 * Hasta aca cualquier rechazo salia por la ABI como AYTHER_STATUS_UNSUPPORTED y
 * el frontend no podia registrar mas que un «fallo». Son cuatro problemas
 * distintos, y tres dependen de lo que el JUEGO esta haciendo en ese momento:
 * un frontend que apaga la sustitucion HD necesita poder decir cual fue.
 *
 * Van en un rango propio, lejos de los status generales, precisamente porque
 * los `AYTHER_RC_ERR_*` internos ocupan -1..-4. Un frontend que solo compara
 * contra AYTHER_STATUS_OK no nota la diferencia; el que quiera el motivo, lo
 * tiene. Los valores son ADITIVOS: nunca se devolvieron antes. */
enum ayther_recompose_status
{
  AYTHER_STATUS_RC_NOT_MODE5      = -20,
  AYTHER_STATUS_RC_INTERLACE2     = -21,
  AYTHER_STATUS_RC_NTSC_FILTER    = -22,
  AYTHER_STATUS_RC_INVALID_PARAMS = -23,
  AYTHER_STATUS_RC_JOURNAL_OVERFLOW = -24
};

enum ayther_endianness
{
  AYTHER_ENDIAN_LITTLE = 1,
  AYTHER_ENDIAN_BIG    = 2
};

/* Capabilities are additive. Unknown bits must be ignored. */
#define AYTHER_CAP_LEGACY_MEMORY       (UINT64_C(1) << 0)
#define AYTHER_CAP_REGION_QUERY        (UINT64_C(1) << 1)
#define AYTHER_CAP_REGION_READ         (UINT64_C(1) << 2)
#define AYTHER_CAP_CONTROL_WRITE       (UINT64_C(1) << 3)
#define AYTHER_CAP_FRAME_SNAPSHOT      (UINT64_C(1) << 4)
#define AYTHER_CAP_PARSED_SPRITES_V1   (UINT64_C(1) << 5)
#define AYTHER_CAP_AUDIO_WRITES_V1     (UINT64_C(1) << 6)
#define AYTHER_CAP_RASTER_FALLBACK_V1  (UINT64_C(1) << 7)
#define AYTHER_CAP_RECOMPOSE_V1        (UINT64_C(1) << 8)
#define AYTHER_CAP_AUDIO_PROBE_V1      (UINT64_C(1) << 9)
#define AYTHER_CAP_SUBSCRIPTIONS_V1    (UINT64_C(1) << 10)
#define AYTHER_CAP_FRAME_DELTA_V1      (UINT64_C(1) << 11)
#define AYTHER_CAP_RECOMPOSE_STATS_V1  (UINT64_C(1) << 12)
#define AYTHER_CAP_ATTRIBUTION_V1      (UINT64_C(1) << 13)
/* : el delta dejo de consumirse al leerlo y hay historial por generacion.
   Sin este bit, `poll_frame_delta` vacia el bitmap al leerlo y un segundo
   lector del mismo frame recibe cero. */
#define AYTHER_CAP_FRAME_DELTA_SINCE_V1 (UINT64_C(1) << 14)
/* .B: descriptor de sistema. Sin este bit, saber en que modo esta el VDP
   exige decodificar VDP_REGS del lado del consumidor. */
#define AYTHER_CAP_SYSTEM_V1           (UINT64_C(1) << 15)
/* : los controles de render funcionan tambien en Mode 4 (SMS/GG/PBC).
   Mientras este bit NO este, los controles que dependen de Mode 5 -- supresion
   de sprites, peel, supresion por celda de plano-- devuelven
   AYTHER_STATUS_UNSUPPORTED_MODE en Mode 4 en vez de aceptar y no hacer nada.
   Lo que SI funciona en los dos modos: la mascara de sprites de layer_mask,
   layer_dim y todos los controles de audio. */
#define AYTHER_CAP_MODE4_CONTROLS      (UINT64_C(1) << 16)
/* : estado de render por scanline (registros/scroll y CRAM por linea). */
#define AYTHER_CAP_LINE_STATE_V1       (UINT64_C(1) << 17)
/* .A/D/E: contenido del journal, hashes por frame y paleta resuelta. Van
   juntas en un bit porque llegan juntas y ninguna tiene sentido sin la ABI
   1.7; separarlas seria prometer que una puede faltar, y no puede. */
#define AYTHER_CAP_OBSERVABILITY_V1    (UINT64_C(1) << 18)
/* .C: por que gano o perdio cada sprite. */
#define AYTHER_CAP_SPRITE_OUTCOME_V1   (UINT64_C(1) << 19)

/* Observation and control work is opt-in. A requested mask becomes active at
 * the beginning of the next frame; unknown bits are rejected. */
#define AYTHER_SUB_VDP_MEMORY       (UINT32_C(1) << 0)
#define AYTHER_SUB_SPRITE_CAPTURE   (UINT32_C(1) << 1)
#define AYTHER_SUB_RENDER_CONTROLS  (UINT32_C(1) << 2)
#define AYTHER_SUB_RASTER_TRACKING  (UINT32_C(1) << 3)
#define AYTHER_SUB_AUDIO_WRITES     (UINT32_C(1) << 4)
#define AYTHER_SUB_RECOMPOSITION    (UINT32_C(1) << 5)
#define AYTHER_SUB_AUDIO_EVENTS     (UINT32_C(1) << 6)
/* : buffer de atribucion por pixel. Bit propio y no parte de RENDER_CONTROLS
 * porque su costo es de otro orden -un byte por pixel por frame- y un consumidor
 * que solo oculta capas no tiene por que pagarlo. */
#define AYTHER_SUB_ATTRIBUTION      (UINT32_C(1) << 7)
/* : el estado por scanline. Bit propio y no parte de VDP_MEMORY porque el
   costo es de otro orden -- una copia por LINEA y no una lectura por frame-, y
   quien quiere el mapa de celdas no necesariamente quiere pagar la CRAM. */
#define AYTHER_SUB_LINE_STATE       (UINT32_C(1) << 8)
#define AYTHER_SUB_LINE_CRAM        (UINT32_C(1) << 9)
#define AYTHER_SUB_LINE_CELLS       (UINT32_C(1) << 10)
/* .D: los hashes son lo unico de esta tanda que CUESTA -- unos 100 KB
   recorridos por frame-, asi que es lo unico que se suscribe aparte. El
   journal y la paleta ya se mantienen para otra cosa: cobrarles una
   suscripcion propia seria cobrar por trabajo que ya esta hecho. */
#define AYTHER_SUB_FRAME_HASH       (UINT32_C(1) << 11)
#define AYTHER_SUB_ALL              UINT32_C(0xFFF)

enum ayther_region_id
{
  AYTHER_REGION_VRAM = 1,
  AYTHER_REGION_CRAM,
  AYTHER_REGION_VDP_REGS,
  AYTHER_REGION_VSRAM,
  AYTHER_REGION_LAYER_MASK,
  AYTHER_REGION_SPRITE_SUPPRESS,
  AYTHER_REGION_TILE_SUPPRESS,
  AYTHER_REGION_PLANE_TILE_SUPPRESS,
  AYTHER_REGION_PLANE_SUPPRESS_ACTIVE,
  AYTHER_REGION_LAYER_DIM,
  AYTHER_REGION_AUDIO_WRITES,
  AYTHER_REGION_AUDIO_WRITE_COUNT,
  AYTHER_REGION_PARSED_SPRITES,
  AYTHER_REGION_PARSED_SPRITE_COUNT,
  AYTHER_REGION_AUDIO_MUTE,
  AYTHER_REGION_RASTER_FALLBACK_REASONS,
  /* : un byte por pixel del frame emitido, con quien pinto cada uno. */
  AYTHER_REGION_ATTRIBUTION,
  /* .B: que hardware, que modo, que viewport y que ROM. */
  AYTHER_REGION_SYSTEM,
  /* : registros y scroll tal como el VDP los uso EN CADA LINEA. */
  AYTHER_REGION_LINE_REGS,
  /* : la CRAM vigente en cada linea. */
  AYTHER_REGION_LINE_CRAM,
  /* .C: que entrada de la name table le toco a cada columna. */
  AYTHER_REGION_LINE_CELLS,
  /* .A: los eventos raster del frame, no solo cuantos hubo. */
  AYTHER_REGION_RASTER_JOURNAL,
  /* .D: hashes del frame, para detectar desincronizacion sin serializar. */
  AYTHER_REGION_FRAME_HASH,
  /* .E: la paleta ya resuelta al formato de pixel del build. */
  AYTHER_REGION_PALETTE,
  /* .C: que le paso a cada sprite de la SAT en este frame. */
  AYTHER_REGION_SPRITE_OUTCOME,
  /*  (ABI 1.9): los 8 KB de RAM del Z80. Es donde varios juegos dejan el
     id del tema a tocar, y sin esto no habia donde mirar cuando la casilla no
     esta en work RAM. */
  AYTHER_REGION_Z80_RAM,
  AYTHER_REGION_COUNT
};

/* Deprecated compatibility IDs. New frontends should use region IDs and the
 * functions in ayther_interface_v1 instead of mutable direct pointers. */
enum ayther_legacy_memory_id
{
  AYTHER_LEGACY_MEMORY_VRAM                  = 0x003,
  AYTHER_LEGACY_MEMORY_CRAM                  = 0x100,
  AYTHER_LEGACY_MEMORY_VDP_REGS              = 0x101,
  AYTHER_LEGACY_MEMORY_LAYER_MASK            = 0x102,
  AYTHER_LEGACY_MEMORY_SPRITE_SUPPRESS       = 0x103,
  AYTHER_LEGACY_MEMORY_TILE_SUPPRESS         = 0x104,
  AYTHER_LEGACY_MEMORY_PLANE_TILE_SUPPRESS   = 0x105,
  AYTHER_LEGACY_MEMORY_PLANE_SUPPRESS_ACTIVE = 0x106,
  AYTHER_LEGACY_MEMORY_VSRAM                 = 0x107,
  AYTHER_LEGACY_MEMORY_LAYER_DIM             = 0x108,
  AYTHER_LEGACY_MEMORY_AUDIO_WRITES          = 0x109,
  AYTHER_LEGACY_MEMORY_AUDIO_WRITE_COUNT     = 0x10A,
  AYTHER_LEGACY_MEMORY_PARSED_SPRITES        = 0x10B,
  AYTHER_LEGACY_MEMORY_PARSED_SPRITE_COUNT   = 0x10C,
  AYTHER_LEGACY_MEMORY_AUDIO_MUTE            = 0x10D,
  AYTHER_LEGACY_MEMORY_RASTER_DIRTY          = 0x10E,
  /*  (ABI 1.9): RAM del Z80, 8 KB. */
  AYTHER_LEGACY_MEMORY_Z80_RAM               = 0x10F
};

#define AYTHER_REGION_ACCESS_READ          (UINT32_C(1) << 0)
#define AYTHER_REGION_ACCESS_CONTROL_WRITE (UINT32_C(1) << 1)
#define AYTHER_REGION_FRAME_SCOPED         (UINT32_C(1) << 2)
#define AYTHER_REGION_NATIVE_ENDIAN        (UINT32_C(1) << 3)
#define AYTHER_REGION_DEPRECATED_LEGACY    (UINT32_C(1) << 4)
/* : la region se entrega en el layout INTERNO word-swapped del emulador en
 * hosts little-endian: el byte logico `off` vive en `off ^ 1`. Aplica a VRAM y
 * a la Work RAM legacy. Estaba documentado en prosa y en ningun lado del
 * contrato, asi que un consumidor que solo leyera el descriptor no tenia como
 * enterarse — y el sintoma es una imagen con los bytes de cada tile cruzados,
 * que se lee como un bug del frontend. No es lo mismo que la ausencia de
 * NATIVE_ENDIAN: eso habla del orden de los campos multi-byte, esto del orden
 * de los BYTES dentro de la memoria emulada. */
#define AYTHER_REGION_WORD_SWAPPED_LE      (UINT32_C(1) << 5)

#define AYTHER_LAYOUT_RAW_V1         UINT32_C(1)
#define AYTHER_LAYOUT_SPRITE_V1      UINT32_C(1)
#define AYTHER_LAYOUT_AUDIO_WRITE_V1 UINT32_C(1)
/* Bumped to 2: PCM events moved off the `voice` arm and gained st/ls (see the
 * union comment on ayther_audio_event_v1). Every event carries this in its
 * `schema` byte, so a consumer can tell the two apart at runtime. */
#define AYTHER_LAYOUT_AUDIO_EVENT_V1 UINT32_C(2)
#define AYTHER_LAYOUT_FRAME_DELTA_V1 UINT32_C(1)

/* : atribucion por pixel. Un byte por pixel del frame EMITIDO, en el mismo
 * orden que el framebuffer (fila 0 primero, `width` bytes por fila).
 *
 * Existe porque hoy el frontend deduce esto recomponiendo el frame varias veces
 * con distintas mascaras y diffeando: N pasadas de render para una respuesta que
 * el VDP ya conoce mientras dibuja. Con esto, "que asset reemplaza este pixel"
 * se contesta leyendo un byte.
 *
 *   bits 7-6  capa: 0 = backdrop, 1 = Plano B, 2 = Plano A, 3 = Window
 *   bit  5    prioridad de la celda de fondo que gano
 *   bits 4-3  linea de paleta (0-3) del pixel de fondo
 *   bits 2-1  shadow/highlight: 0 = normal, 1 = shadow, 2 = highlight
 *   bit  0    el pixel visible lo puso un sprite
 *
 * La capa se decide con la MISMA regla de prioridad que la LUT de merge, no
 * comparando valores: dos capas pueden producir el mismo byte y ahi comparar da
 * una respuesta arbitraria. */
#define AYTHER_LAYOUT_ATTRIBUTION_V1 UINT32_C(1)
#define AYTHER_LAYOUT_SYSTEM_V1      UINT32_C(1)
#define AYTHER_LAYOUT_LINE_REGS_V1   UINT32_C(1)
#define AYTHER_LAYOUT_LINE_CRAM_V1   UINT32_C(1)
#define AYTHER_LAYOUT_LINE_CELLS_V1  UINT32_C(1)
#define AYTHER_LAYOUT_JOURNAL_V1     UINT32_C(1)
#define AYTHER_LAYOUT_FRAME_HASH_V1  UINT32_C(1)
#define AYTHER_LAYOUT_PALETTE_V1     UINT32_C(1)
#define AYTHER_LAYOUT_SPR_OUTCOME_V1 UINT32_C(1)

/* AYTHER (): el estado de render POR SCANLINE, capturado del lado de la
 * LECTURA -- cuando el renderer lo usa- y no reconstruido desde el lado de la
 * escritura.
 *
 * Reconstruirlo desde las escrituras es lo que hace el raster journal, y es
 * aproximado por construccion: hay que adivinar en que ciclo de que linea cayo
 * cada write y que efecto tuvo. Capturarlo donde el renderer lo consume es
 * exacto y ademas mas barato.
 *
 * Para que sirve: la celda de pantalla que el frontend puede ocultar hoy
 * (id 0x104) es frame-space, y con scroll por linea NUNCA coincide con un tile
 * del plano. Con `xscroll_a` de la linea, el frontend puede mapear "la celda
 * (x,y) de la pantalla" a "este tile del plano A", que es lo que un pipeline de
 * sustitucion HD necesita para keyear un asset.
 */
typedef struct ayther_line_regs_v1
{
  uint16_t xscroll_a, xscroll_b;   /* ya resueltos desde la tabla de hscroll */
  uint16_t yscroll_a, yscroll_b;   /* VSRAM[0..1]                            */
  uint16_t ntab, ntbb, ntwb;       /* bases de las name tables, resueltas    */
  uint16_t hscb, satb;             /* bases de hscroll y de la SAT           */
  uint8_t  reg1, reg7, reg11, reg12, reg13, reg16, reg17, reg18;
  uint8_t  clip_a_start, clip_a_end, clip_w_start, clip_w_end;
  uint8_t  flags;                  /* AYTHER_LINE_* de abajo                 */
  uint8_t  reserved0;
} ayther_line_regs_v1;

/* La linea se dibujo con el plano A recortado por la ventana. */
#define AYTHER_LINE_WINDOW_ACTIVE  UINT8_C(0x01)
/* La linea se dibujo con vscroll por columna (reg 11 bit 2). */
#define AYTHER_LINE_VSCROLL_COLUMN UINT8_C(0x02)

/* Cabecera comun de las regiones por linea: el consumidor tiene que poder
   interpretar el buffer sin adivinar cuantas lineas trae ni de que frame es. */
typedef struct ayther_line_header_v1
{
  uint32_t struct_size;      /* tamanio de esta cabecera                     */
  uint32_t entry_size;       /* tamanio de cada entrada que sigue            */
  uint32_t lines;            /* entradas validas                             */
  uint32_t flags;            /* AYTHER_LINES_* de abajo                      */
  uint64_t frame_generation; /* el frame al que pertenece                    */
} ayther_line_header_v1;

/* AYTHER (.C): procedencia por CELDA de cada linea.
 *
 * Con LINE_REGS un frontend sabe el scroll de la linea; con esto sabe, sin
 * volver a leer VRAM ni reimplementar el calculo de direcciones, QUE ENTRADA de
 * la name table le toco a cada columna de 16 px, y en que fila del tile.
 *
 * El VDP lee la name table de a pares -- dos celdas por acceso de 32 bits-, y
 * eso es lo que se guarda: `name_pair` es el valor crudo, tal como el renderer
 * lo consumio. Guardar los pares y no las celdas sueltas es lo que hace que la
 * captura no cueste una lectura extra: el valor ya estaba en un registro.
 *
 * Ese "tal cual" incluye el orden de bytes: en un host little-endian la VRAM
 * esta guardada con los bytes intercambiados, asi que la region se declara con
 * AYTHER_REGION_WORD_SWAPPED_LE. Leerla sin mirar ese flag da celdas al reves
 * sin ningun sintoma que lo delate.
 *
 * Fila y desplazamiento fino son por LINEA y por PLANO, no por columna: el
 * renderer los calcula una vez. Guardarlos por columna seria repetir 21 veces
 * el mismo byte.
 */
#define AYTHER_LINE_CELL_COLUMNS 21   /* 21 x 16 px cubre H40 con el preambulo */

typedef struct ayther_line_cells_v1
{
  uint32_t name_a[AYTHER_LINE_CELL_COLUMNS];
  uint32_t name_b[AYTHER_LINE_CELL_COLUMNS];
  uint32_t name_w[AYTHER_LINE_CELL_COLUMNS];
  uint8_t  row_a, row_b, row_w;   /* fila dentro del tile, en pixeles      */
  uint8_t  shift_a, shift_b;      /* desplazamiento fino de la linea (0-15) */
  uint8_t  cols_a, cols_b, cols_w;
  uint8_t  reserved0;
} ayther_line_cells_v1;

/* .A: el journal raster del frame, entero.
 *
 * El core ya lo llenaba -- es el insumo del raster replay-- y de el solo se
 * exponia `raster_event_count`. Saber que un frame tuvo 17 eventos y no
 * CUALES alcanza para decidir "no lo recompongo" y para nada mas: el
 * frontend que quiere entender el split tiene que volver a mirar la memoria
 * del VDP frame por frame y adivinar cuando cambio, que es justamente lo que
 * el journal ya sabe.
 *
 * `dropped` no es un bit sino una cuenta, igual que adentro del core: el
 * frontend que dimensiona su propio buffer necesita distinguir "se paso por
 * uno" de "este frame es un festival de splits".
 *
 * Se llena bajo AYTHER_SUB_RASTER_TRACKING, que es la suscripcion que ya
 * hace que el journal exista. Sin ella la region contesta NOT_SUBSCRIBED en
 * vez de devolver el journal fosil del ultimo frame observado.
 */
#define AYTHER_JOURNAL_MAX_EVENTS 256

typedef struct ayther_journal_event_v1
{
  uint16_t v_counter;   /* linea en que ocurrio                        */
  uint16_t reason;      /* AYTHER_RASTER_REASON_* del core             */
  uint16_t address;     /* segun el motivo: REG = numero de registro;   */
                        /* CRAM = indice de entrada (0-63); VSRAM =      */
                        /* byte par; HSCROLL = direccion de VRAM ()  */
  uint16_t data;        /* los 16 bits que el bus puso                 */
} ayther_journal_event_v1;

typedef struct ayther_journal_v1
{
  uint32_t layout_version;
  uint32_t struct_size;
  uint32_t count;       /* eventos validos en `events`                 */
  uint32_t dropped;     /* eventos que no entraron (0 = journal completo) */
  ayther_journal_event_v1 events[AYTHER_JOURNAL_MAX_EVENTS];
} ayther_journal_v1;

/* .D: hashes del frame.
 *
 * Existe para contestar "¿seguimos sincronizados?" sin serializar. Hasta aca
 * la unica forma era pedir el savestate entero -- ~1 MB-- y hashearlo
 * afuera, una vez por frame, para comparar un numero.
 *
 * NO hay `state_hash`, y no es un olvido: calcularlo obliga a serializar, o
 * sea a hacer exactamente lo que esta region existe para evitar. Las cuatro
 * memorias del VDP mas el video y el audio emitidos cubren la desincroni-
 * zacion que un frontend puede ver; para el resto del estado esta
 * `retro_serialize`, que es donde esa pregunta corresponde.
 *
 * El algoritmo es FNV-1a de 64 bits sobre los bytes en el orden en que el
 * core los tiene -- el mismo que usa `full_core_replay`-, asi que un
 * frontend puede recalcularlo y comparar. `frame_index` dice de que frame
 * son: leer la region dos veces en el mismo frame da lo mismo.
 */
typedef struct ayther_frame_hash_v1
{
  uint32_t layout_version;
  uint32_t struct_size;
  uint64_t frame_index;
  uint64_t video_hash;  /* pixeles emitidos, viewport w*h              */
  uint64_t audio_hash;  /* samples entregados en el frame              */
  uint64_t vram_hash;
  uint64_t cram_hash;
  uint64_t vsram_hash;
} ayther_frame_hash_v1;

/* .E: la paleta ya resuelta al formato de pixel del build.
 *
 * El core arma esta tabla igual, en `color_update_m5`: indexada por el byte
 * fusionado del line buffer, con shadow y highlight YA aplicados. Un
 * frontend que quiere el color de un indice tenia que rehacer la conversion
 * 9-bit -> RGB y las reglas de S/H por su cuenta, con dos copias de la misma
 * regla y sin nadie que avise cuando se separan.
 *
 * Se expone la tabla ENTERA (256 entradas), no las 192 "utiles": el indice
 * es el byte del line buffer tal cual, y recortarla obligaria al consumidor
 * a saber cual es el recorte para volver a mapear. El ancho de cada entrada
 * lo dice `element_size` de la region, porque depende del formato con que se
 * compilo el core.
 *
 * Es la tabla VIGENTE, no una por linea. Un juego que escribe CRAM a mitad de
 * frame -- un raster de paleta-- produce una imagen con colores de VARIAS
 * tablas, y esta contesta la ultima. Eso no es una limitacion escondida: para
 * el caso por linea esta LINE_CRAM (), y `AYTHER_REGION_RASTER_JOURNAL`
 * dice si el frame tuvo escrituras de CRAM. Con journal sin eventos de CRAM,
 * esta tabla explica todos los colores del frame; con eventos, no puede.
 */
#define AYTHER_PALETTE_ENTRIES 256

/* .C: el resultado de render de cada sprite, un byte por slot de la SAT.
 *
 * `PARSED_SPRITES` dice que sprites vio el parser. No dice que les paso
 * despues, y "aparece en la lista" no es lo mismo que "se dibujo": entre una
 * cosa y la otra el VDP puede descartarlo por el limite de sprites de la
 * linea, por el presupuesto de pixeles, o taparlo con la mascara de x=0.
 *
 * Un frontend que quiere saber por que su sprite no aparece tenia que deducir
 * esas tres reglas por su cuenta, contando sprites por linea y reimplementando
 * el orden de la cadena de la SAT. El core ya las conoce: son las condiciones
 * exactas que evalua mientras dibuja.
 *
 * Los bits se ACUMULAN sobre el frame, no por linea: un sprite de 32 px de
 * alto puede dibujarse en las primeras lineas y caerse por presupuesto en las
 * ultimas, y las dos cosas son ciertas. Un frontend que necesite el detalle
 * por linea tiene el journal y el estado por scanline.
 *
 * El indice es el slot de la SAT -- el mismo espacio que la mascara de
 * supresion (id 0x103) y que `sat_idx` de `ayther_sprite_v1`-, no el orden de
 * la cadena: el orden cambia entre frames, el slot no.
 */
#define AYTHER_SPRITE_SAT_SLOTS 80

/* Entro en la lista de la linea al menos una vez. */
#define AYTHER_SPR_OUT_PARSED        UINT8_C(0x01)
/* Llego al bucle de dibujo al menos una vez. */
#define AYTHER_SPR_OUT_DRAWN         UINT8_C(0x02)
/* Descartado por el limite de sprites POR LINEA (16 en H32, 20 en H40). */
#define AYTHER_SPR_OUT_DROP_LINE     UINT8_C(0x04)
/* Descartado por el presupuesto de PIXELES de la linea. */
#define AYTHER_SPR_OUT_DROP_PIXEL    UINT8_C(0x08)
/* Tapado por la mascara de sprites: alguno anterior estaba en x=0. */
#define AYTHER_SPR_OUT_MASKED_X0     UINT8_C(0x10)
/* Suprimido por el frontend con la mascara 0x103. Se distingue de los
   descartes del hardware a proposito: "no se dibujo porque vos lo pediste"
   y "no se dibujo porque el VDP no daba" son respuestas distintas. */
#define AYTHER_SPR_OUT_SUPPRESSED    UINT8_C(0x20)

/* La CRAM no cambio en todo el frame: solo la entrada 0 es significativa. */
#define AYTHER_LINES_CRAM_UNIFORM   UINT32_C(0x01)
/* Alguna linea no se lleno -- un renderer sin hooks-, y el buffer tiene huecos. */
#define AYTHER_LINES_OVERFLOW       UINT32_C(0x02)

/* AYTHER (.B): descriptor del sistema emulado.
 *
 * Todo esto era derivable, pero solo decodificando VDP_REGS del lado del
 * consumidor: H40 sale del bit 0 de reg 12, el interlace de los bits 1-2, las
 * lineas activas del bit 3 de reg 1... Decodificarlo afuera es reimplementar
 * las reglas del core en otro repositorio, y cuando el core las corrija -- que
 * ya paso:  arreglo justo esas mascaras-- nadie avisa que la copia quedo
 * vieja. El core sabe la respuesta; darla cuesta un struct.
 *
 * Se refresca al empezar cada frame y queda fijo durante el. `system_hw` usa
 * los mismos valores que SYSTEM_* del core.
 */
/* 1.10: los registros cambiaron la geometria (ancho, alto, interlace) durante
   el ultimo frame y el core la aplica al inicio del siguiente. Mientras esta
   puesto, `viewport_w/h` y `h40` describen el frame que ya salio y VDP_REGS
   describe el que viene: son dos frames distintos, y el flag es lo que le
   dice al frontend que no los mezcle. Un frontend que quiere la geometria
   "definitiva" espera a que se apague; uno que dibuja el frame que recibio
   usa el viewport tal cual. */
#define AYTHER_SYSTEM_GEOMETRY_PENDING UINT8_C(0x01)

#define AYTHER_SYSTEM_HW_SG      0x01
#define AYTHER_SYSTEM_HW_MARKIII 0x10
#define AYTHER_SYSTEM_HW_SMS     0x20
#define AYTHER_SYSTEM_HW_SMS2    0x21
#define AYTHER_SYSTEM_HW_GG      0x40
#define AYTHER_SYSTEM_HW_GGMS    0x41
#define AYTHER_SYSTEM_HW_MD      0x80
#define AYTHER_SYSTEM_HW_PBC     0x81
#define AYTHER_SYSTEM_HW_PICO    0x82
#define AYTHER_SYSTEM_HW_MCD     0x84

typedef struct ayther_system_v1
{
  uint32_t struct_size;
  uint32_t layout_version;

  uint8_t  system_hw;        /* SYSTEM_* del core                         */
  uint8_t  region_pal;       /* 1 = PAL (313 lineas), 0 = NTSC            */
  uint8_t  vdp_mode;         /* 4 o 5; 0 si el VDP todavia no eligio      */
  uint8_t  interlace;        /* 0 = progresivo, 1 = interlace 1, 2 = im2  */

  uint8_t  h40;              /* 1 = el frame EMITIDO mide 320 px (== viewport_w); reg 12 puede ir un frame adelante: ver flags */
  uint8_t  shadow_highlight; /* 1 = S/H activo (reg 12 bit 3)             */
  uint16_t lines_per_frame;  /* 262 NTSC / 313 PAL                        */

  /* Viewport del frame emitido, en pixeles, incluido el overscan que el
     build entrega. Es el mismo rectangulo que describe ATTRIBUTION.

     `viewport_w/h` son las dimensiones del frame que sale por video_refresh:
     320x224 (H40) o 256x224 (H32) en Mega Drive -- 240 lineas en V30--,
     256x192 en Master System, 160x144 en Game Gear.
     Es la misma cuenta que hace el propio frontend del core.

     `viewport_x/y` son el offset donde ese frame empieza dentro del area
     activa del VDP. Solo la Game Gear los tiene distintos de cero (48,24),
     porque es la unica que RECORTA. Cuando el build entrega overscan el frame
     es mas grande que el area activa en vez de mas chico: ahi no hay offset
     que dar y valen cero, con `viewport_w/h` ya incluyendo los bordes. */
  uint16_t viewport_x, viewport_y, viewport_w, viewport_h;

  uint32_t cpu_clock;        /* Hz del 68000 (o del Z80 en 8 bits)        */
  uint32_t master_clock;     /* Hz del oscilador maestro                  */

  uint8_t  fm_core;          /* 0 = MAME (ym2612), 1 = Nuked (ym3438)     */
  uint8_t  psg_present;
  uint8_t  pcm_present;      /* RF5C164 del Mega CD                       */
  uint8_t  flags;            /* AYTHER_SYSTEM_* (1.10); antes reservado, siempre 0 */

  uint32_t rom_crc32;        /* crc32 del archivo cargado                 */
  uint32_t rom_bytes;
} ayther_system_v1;

#define AYTHER_ATTRIB_LAYER_MASK     UINT8_C(0xC0)
#define AYTHER_ATTRIB_LAYER_SHIFT    6
#define AYTHER_ATTRIB_LAYER_BACKDROP 0
#define AYTHER_ATTRIB_LAYER_PLANE_B  1
#define AYTHER_ATTRIB_LAYER_PLANE_A  2
#define AYTHER_ATTRIB_LAYER_WINDOW   3
#define AYTHER_ATTRIB_PRIORITY       UINT8_C(0x20)
#define AYTHER_ATTRIB_PALETTE_MASK   UINT8_C(0x18)
#define AYTHER_ATTRIB_PALETTE_SHIFT  3
#define AYTHER_ATTRIB_SH_MASK        UINT8_C(0x06)
#define AYTHER_ATTRIB_SH_SHIFT       1
#define AYTHER_ATTRIB_SH_NORMAL      0
#define AYTHER_ATTRIB_SH_SHADOW      1
#define AYTHER_ATTRIB_SH_HIGHLIGHT   2
#define AYTHER_ATTRIB_SPRITE         UINT8_C(0x01)

/* Un sprite que el parser vio, con el MISMO layout en Mode 5 y en Mode 4.
 *
 * No lleva un campo `mode`, y eso es deliberado. Esta struct viaja como
 * ARRAY: agregarle un byte corre todas las entradas siguientes y rompe el
 * indexado de cualquier consumidor ya compilado -- la razon exacta por la que
 * SPRITE_OUTCOME (.C) es una region paralela y no un campo mas. Una region
 * paralela de modos, a su vez, llevaria el mismo byte hasta 64 veces para
 * contestar algo que es constante por frame. El modo se lee de
 * `AYTHER_REGION_SYSTEM` (`vdp_mode`), que existe para eso (.B).
 *
 * Lo que cambia entre modos es el SIGNIFICADO de los campos, no su forma:
 *
 *   yr        Y CRUDA de la SAT, sin convertir a coordenada de pantalla. En
 *             Mode 5 son 9 bits con el sesgo de 128 del VDP; en Mode 4 es la
 *             Y de pantalla MENOS uno, que es lo que el hardware quiere.
 *   xr        X cruda: 9 bits con sesgo en Mode 5, X de pantalla en Mode 4.
 *   attr      La word de atributos tal cual. En Mode 5 trae patron (11 bits),
 *             paleta (2 bits), prioridad y los dos flips. En Mode 4 es el
 *             indice de patron sobre la base de reg[6], con UNA de dos
 *             paletas de 16 (bit 3) y sin flips ni prioridad por sprite.
 *   w, h      Tamanio en CELDAS de 8x8, no en pixeles. Mode 5 da 1..4 en cada
 *             eje; Mode 4 siempre 1 de ancho y 1 o 2 de alto (reg[1] bit 1).
 *   sat_idx   Indice en la SAT, y por lo tanto el bit que le corresponde en la
 *             mascara de supresion 0x103. 64 entradas en Mode 4, 80 en Mode 5.
 *   chain_pos Paso en el recorrido: la prioridad real de dibujo. En Mode 5 la
 *             SAT es una cadena enlazada y no coincide con sat_idx; en Mode 4
 *             es una lista plana y el recorrido va en orden.
 *
 * Native-endian in-process layout. Multi-byte fields use host endianness as
 * reported by ayther_interface_v1.host_endianness. Pointers are never stored
 * in captured data. */
typedef struct ayther_sprite_v1
{
  uint16_t yr;
  uint16_t xr;
  uint16_t attr;
  uint8_t w;
  uint8_t h;
  uint8_t sat_idx;
  uint8_t chain_pos;
} ayther_sprite_v1;

typedef struct ayther_audio_write_v1
{
  uint32_t cycle;
  uint16_t addr;
  uint8_t data;
  uint8_t chip;
} ayther_audio_write_v1;

enum ayther_audio_source_v1
{
  AYTHER_AUDIO_SOURCE_FM  = 0,
  AYTHER_AUDIO_SOURCE_PSG = 1,
  AYTHER_AUDIO_SOURCE_DAC = 2,
  AYTHER_AUDIO_SOURCE_PCM = 3
};

enum ayther_audio_event_type_v1
{
  AYTHER_AUDIO_EVENT_RAW_WRITE = 0,
  AYTHER_AUDIO_EVENT_NOTE_ON,
  AYTHER_AUDIO_EVENT_NOTE_OFF,
  AYTHER_AUDIO_EVENT_DAC_START,
  AYTHER_AUDIO_EVENT_DAC_STOP,
  AYTHER_AUDIO_EVENT_PATCH,
  AYTHER_AUDIO_EVENT_PITCH,
  AYTHER_AUDIO_EVENT_VOLUME,
  AYTHER_AUDIO_EVENT_RESET,
  AYTHER_AUDIO_EVENT_STATE_LOAD,
  AYTHER_AUDIO_EVENT_FRAME
};

typedef struct ayther_audio_voice_v1
{
  uint8_t op_tl[4];
  uint8_t op_ar[4];
  uint8_t op_dr[4];
  uint8_t op_sr[4];
  uint8_t op_rr[4];
  uint8_t op_mul[4];
  uint8_t op_dt[4];
  uint8_t algorithm;
  uint8_t feedback;
  uint8_t ams;
  uint8_t fms;
  uint8_t pan;
  uint32_t block_fnum;
} ayther_audio_voice_v1;

/* `group` is non-zero only for logical trigger anchors (NOTE_ON and
 * DAC_START). A fixed coincidence window is measured from the first anchor in
 * a group; raw writes, frame markers and release events always use group 0.
 *
 * WHICH ARM OF THE UNION APPLIES IS DECIDED BY `source`, NOT BY `type`:
 *
 *   FM  -> the `voice` arm on NOTE_ON (resolved snapshot + canonical hashes);
 *          every other FM event uses {reg, data}.
 *   PSG -> {reg, data}.
 *   PCM -> {reg, data}, ALWAYS. The RF5C164 has no operators, so the `voice`
 *          arm never applied to it; up to schema 1 the PCM path wrote `data`
 *          (one arm) and `voice.pan` (the other) into the same event, which
 *          only worked because those two fields happen not to overlap.
 *          Schema 2 packs every PCM field into {reg, data}:
 *
 *            NOTE_ON   reg  = st | (ls << 8)
 *                      data = fd | (env << 16) | (pan << 24)
 *            NOTE_OFF  reg  = st | (ls << 8),  data = 0
 *            VOLUME    data = env | (pan << 8)
 *            PITCH     data = fd
 *
 *          `st` is the ST register byte (Wave RAM start address >> 19) and
 *          `ls` the 16-bit loop address: TOGETHER THEY SAY WHICH SAMPLE PLAYS.
 *          `fd` is the 5.11 address increment — playback rate RELATIVE to that
 *          sample, not an absolute note. `env` is the envelope multiplier.
 *
 *          A consumer that has to identify sounds needs st/ls. Schema 1 shipped
 *          only env/pan/fd — volume and rate — so two SFX playing different
 *          samples at the same rate and level were indistinguishable. */
/* The direct `event.reg` / `event.voice` field names are part of the published
 * source contract. C11 and MSVC support the anonymous aggregate layout used to
 * preserve those names; Clang and GCC accept it as an extension in C++. Keep
 * the exception scoped to this one ABI type so project warnings remain errors. */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#  pragma clang diagnostic ignored "-Wnested-anon-types"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
typedef struct ayther_audio_event_v1
{
  uint64_t t_global;
  uint32_t t_frame;
  uint32_t t_cycles;
  uint8_t source;
  uint8_t type;
  uint8_t channel;
  uint8_t schema;
  uint32_t group;
  union {
    struct {
      uint32_t reg;
      uint32_t data;
    };
    struct {
      ayther_audio_voice_v1 voice;
      uint64_t voice_hash;
      uint64_t timbre_hash;
    };
  };
} ayther_audio_event_v1;
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#define AYTHER_AUDIO_TRANSPORT_CALLBACK_ACTIVE (UINT32_C(1) << 0)
#define AYTHER_AUDIO_TRANSPORT_OBSERVATION_ACTIVE (UINT32_C(1) << 1)

/* Values are a concurrent snapshot. `capacity` is the effective number of
 * events (one slot is reserved by the SPSC full/empty protocol). The dropped
 * counter saturates at UINT32_MAX instead of wrapping. */
typedef struct ayther_audio_transport_stats_v1
{
  uint32_t struct_size;
  uint32_t transport_version;
  uint32_t event_size;
  uint32_t capacity;
  uint32_t pending;
  uint32_t high_water_mark;
  uint32_t dropped_events;
  uint32_t flags;
} ayther_audio_transport_stats_v1;

typedef struct ayther_region_info_v1
{
  uint32_t struct_size;
  uint32_t region_id;
  uint32_t data_version;
  uint32_t element_size;
  uint32_t capacity;
  uint32_t byte_size;
  uint32_t access_flags;
  uint32_t legacy_memory_id;
} ayther_region_info_v1;

#define AYTHER_SNAPSHOT_CONTENT_LOADED (UINT32_C(1) << 0)
#define AYTHER_SNAPSHOT_FRAME_ACTIVE   (UINT32_C(1) << 1)

#define AYTHER_OVERFLOW_PARSED_SPRITES (UINT32_C(1) << 0)
#define AYTHER_OVERFLOW_AUDIO_WRITES   (UINT32_C(1) << 1)

typedef struct ayther_frame_snapshot_v1
{
  uint32_t struct_size;
  uint32_t snapshot_version;
  uint64_t snapshot_generation;
  uint64_t frame_generation;
  uint32_t flags;
  uint32_t overflow_flags;
  uint32_t fallback_reasons;
  uint32_t parsed_sprite_count;
  uint32_t audio_write_count;
  uint32_t reserved0;
} ayther_frame_snapshot_v1;

typedef struct ayther_subscription_state_v1
{
  uint32_t struct_size;
  uint32_t state_version;
  uint32_t supported_mask;
  uint32_t active_mask;
  uint32_t requested_mask;
  uint32_t reserved0;
  uint64_t activation_frame;
} ayther_subscription_state_v1;

typedef int32_t (AYTHER_CALL *ayther_query_region_v1_fn)(
    uint32_t region_id, ayther_region_info_v1 *out, uint32_t out_size);

typedef int32_t (AYTHER_CALL *ayther_read_region_v1_fn)(
    uint32_t region_id, uint32_t offset, void *out, uint32_t byte_count,
    uint64_t expected_generation, uint64_t *actual_generation);

typedef int32_t (AYTHER_CALL *ayther_write_control_v1_fn)(
    uint32_t region_id, uint32_t offset, const void *data,
    uint32_t byte_count, uint64_t expected_generation,
    uint64_t *new_generation);

typedef int32_t (AYTHER_CALL *ayther_capture_snapshot_v1_fn)(
    ayther_frame_snapshot_v1 *out, uint32_t out_size);

typedef int32_t (AYTHER_CALL *ayther_recompose_frame_v1_fn)(
    uint16_t *out_pixels, uint32_t pixel_capacity, uint32_t flags,
    uint32_t *out_width, uint32_t *out_height);

typedef int32_t (AYTHER_CALL *ayther_poll_audio_events_v1_fn)(
    ayther_audio_event_v1 *out, uint32_t event_capacity,
    uint32_t *out_event_count);

typedef int32_t (AYTHER_CALL *ayther_get_audio_transport_stats_v1_fn)(
    ayther_audio_transport_stats_v1 *out, uint32_t out_size);

typedef int32_t (AYTHER_CALL *ayther_get_subscriptions_v1_fn)(
    ayther_subscription_state_v1 *out, uint32_t out_size);

typedef int32_t (AYTHER_CALL *ayther_set_subscriptions_v1_fn)(
    uint32_t requested_mask);

/* : cuantos frames de historial guarda el ring. Ocho es suficiente para
   que un consumidor que se salteo unos frames -- una UI de debug que
   repinta a 30 Hz sobre 60-- recupere sin perder nada, y son 16 KB. */
#define AYTHER_FRAME_DELTA_HISTORY 8

typedef struct ayther_frame_delta_v1
{
  uint32_t struct_size;
  uint32_t delta_version;
  uint64_t frame_generation;
  uint32_t raster_event_count;
  uint32_t parsed_sprite_count;
  uint32_t audio_write_count;
  /* : eventos raster que no entraron en el journal. Ocupa el `reserved0`
     que siempre valio 0 y que el contrato mandaba ignorar, asi que un lector
     de 1.0 no cambia de comportamiento. Distinto de cero significa que la
     recomposicion multicapa de este frame devuelve
     AYTHER_STATUS_RC_JOURNAL_OVERFLOW: solo se podria reproducir un prefijo. */
  uint32_t raster_events_dropped;
  uint8_t dirty_patterns[2048];
} ayther_frame_delta_v1;

typedef int32_t (AYTHER_CALL *ayther_poll_frame_delta_v1_fn)(
    ayther_frame_delta_v1 *out, uint32_t out_size);

/* : todo lo ensuciado DESDE `generation_from` inclusive, OR-eando el ring.
 *
 * `poll_frame_delta` contesta "que cambio en el ultimo frame". Esta contesta
 * "que cambio desde la ultima vez que mire", que es lo que necesita un
 * consumidor que no lee todos los frames -- y antes no tenia forma de pedirlo
 * sin perder informacion.
 *
 * Si `generation_from` ya salio del ring devuelve AYTHER_STATUS_DELTA_HISTORY_LOST
 * con todo marcado sucio: conservador a proposito, porque la alternativa es
 * devolver un subconjunto y que el consumidor crea que vio todo. */
typedef int32_t (AYTHER_CALL *ayther_frame_delta_since_v1_fn)(
    uint64_t generation_from, ayther_frame_delta_v1 *out, uint32_t out_size);

/* : estado observable de los caches de recomposición.
 *
 * Existe para que "el cache sigue sirviendo" sea afirmable sin cronometrar:
 * un test que mide tiempo en un runner compartido mide ruido. `controls_
 * fingerprint` es además la respuesta a "¿por qué no acertó?" cuando el
 * frontend cree no haber tocado nada: si la huella cambió, algo escribió una
 * máscara — posiblemente por el puntero mutable legacy, que no pasa por
 * `write_control` y por eso no mueve `snapshot_generation`. */
typedef struct ayther_recompose_stats_v1
{
  uint32_t struct_size;
  uint32_t reserved0;
  uint64_t single_calls;
  uint64_t single_hits;
  uint64_t multilayer_calls;
  uint64_t multilayer_hits;
  uint64_t controls_fingerprint;
} ayther_recompose_stats_v1;

typedef int32_t (AYTHER_CALL *ayther_get_recompose_stats_v1_fn)(
    ayther_recompose_stats_v1 *out, uint32_t out_size);

typedef int32_t (AYTHER_CALL *ayther_recompose_multilayer_v1_fn)(
    uint16_t *out_bg_a, uint16_t *out_bg_b, uint16_t *out_window,
    uint16_t *out_sprites, uint16_t *out_composite,
    uint32_t pixel_capacity, uint32_t flags,
    uint32_t *out_width, uint32_t *out_height);

typedef struct ayther_interface_v1
{
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t capabilities;
  uint32_t host_endianness;
  uint32_t pointer_size;
  uint32_t region_info_size;
  uint32_t frame_snapshot_size;
  uint32_t sprite_size;
  uint32_t audio_write_size;
  const char *build_id;
  uint32_t build_id_size;
  uint32_t reserved0;
  ayther_query_region_v1_fn query_region;
  ayther_read_region_v1_fn read_region;
  ayther_write_control_v1_fn write_control;
  ayther_capture_snapshot_v1_fn capture_snapshot;
  ayther_recompose_frame_v1_fn recompose_frame;
  uint32_t audio_event_size;
  uint32_t audio_transport_stats_size;
  ayther_poll_audio_events_v1_fn poll_audio_events;
  ayther_get_audio_transport_stats_v1_fn get_audio_transport_stats;
  uint32_t subscription_state_size;
  uint32_t reserved1;
  ayther_get_subscriptions_v1_fn get_subscriptions;
  ayther_set_subscriptions_v1_fn set_subscriptions;
  uint32_t frame_delta_size;
  ayther_poll_frame_delta_v1_fn poll_frame_delta;
  /* --- ABI 1.1 (). Todo lo de acá abajo sólo se puede leer si
     `struct_size` llega hasta el campo. --- */
  uint32_t recompose_stats_size;
  uint32_t reserved2;
  ayther_get_recompose_stats_v1_fn get_recompose_stats;
  /* --- ABI 1.2 () --- */
  ayther_recompose_multilayer_v1_fn recompose_multilayer;
  /* --- ABI 1.4 () --- */
  ayther_frame_delta_since_v1_fn frame_delta_since;
} ayther_interface_v1;

/* Un campo opcional es legible sólo si el descriptor llega hasta él. Esta es la
 * comprobación que reemplaza a `abi_version == la mía`. */
#define AYTHER_IFACE_HAS(iface, field) \
  ((iface)->struct_size >= (offsetof(ayther_interface_v1, field) + \
                            sizeof(((const ayther_interface_v1 *)0)->field)))

typedef const ayther_interface_v1 *(AYTHER_CALL *ayther_get_interface_fn)(
    uint32_t requested_version);

AYTHER_API const ayther_interface_v1 *AYTHER_CALL ayther_get_interface(
    uint32_t requested_version);

/* DEPRECADO (). Desde ABI 1.2 esta funcion vive en el descriptor como
 * `recompose_multilayer` y hay que resolverla por ahi, con `AYTHER_IFACE_HAS`.
 * El simbolo suelto solo se exporta si el core se compilo con
 * AYTHER_LEGACY_PROFILE=1, para no romper a los consumidores que ya lo
 * resuelven con GetProcAddress mientras migran. */
#ifdef AYTHER_LEGACY_PROFILE
AYTHER_API int32_t AYTHER_CALL ayther_recompose_multilayer(
    uint16_t *out_bg_a, uint16_t *out_bg_b, uint16_t *out_window,
    uint16_t *out_sprites, uint16_t *out_composite,
    uint32_t pixel_capacity, uint32_t flags,
    uint32_t *out_width, uint32_t *out_height);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AYTHER_API_H */

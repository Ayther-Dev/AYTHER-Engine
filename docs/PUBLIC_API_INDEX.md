# AYTHER Engine — índice de headers instalados

> **GENERADO — no editar a mano.** `pwsh tools/gen_api_reference.ps1`.
> Sale de `include/ayther/**/*.h`, la misma raíz que instala
> `cmake/AytherInstall.cmake`: si un header entra
> o sale de la superficie, esta página lo refleja sin que nadie edite una
> lista paralela.

La superficie instalada y su estabilidad se describen en
[`API_COMPATIBILITY.md`](API_COMPATIBILITY.md).
Aparecer en este índice no implica por sí solo una garantía de estabilidad.

## Los 46 headers

| header | qué aporta |
|---|---|
| [`audio_asset_level.h`](#audio-asset-level-h) | Header público instalado. |
| [`audio_bus_balance.h`](#audio-bus-balance-h) | Header público instalado. |
| [`audio_hd_mixer.h`](#audio-hd-mixer-h) | Header público instalado. |
| [`audio_live_resume.h`](#audio-live-resume-h) | Header público instalado. |
| [`audio_match_rule.h`](#audio-match-rule-h) | Header público instalado. |
| [`audio_player.h`](#audio-player-h) | Header público instalado. |
| [`audio_seq_anchor.h`](#audio-seq-anchor-h) | Header público instalado. |
| [`ayther_animation.h`](#ayther-animation-h) | Header público instalado. |
| [`ayther_audio_events.h`](#ayther-audio-events-h) | Header público instalado. |
| [`ayther_background_export.h`](#ayther-background-export-h) | Header público instalado. |
| [`ayther_components_toml.h`](#ayther-components-toml-h) | Header público instalado. |
| [`ayther_config.h`](#ayther-config-h) | Header público instalado. |
| [`ayther_core_ffi.h`](#ayther-core-ffi-h) | Header público instalado. |
| [`ayther_env.h`](#ayther-env-h) | Header público instalado. |
| [`ayther_layers.h`](#ayther-layers-h) | Header público instalado. |
| [`ayther_mode3.h`](#ayther-mode3-h) | Header público instalado. |
| [`ayther_rank.h`](#ayther-rank-h) | Header público instalado. |
| [`ayther_recording.h`](#ayther-recording-h) | Header público instalado. |
| [`ayther_renderer.h`](#ayther-renderer-h) | Header público instalado. |
| [`ayther_result.h`](#ayther-result-h) | Header público instalado. |
| [`ayther_sdk_version.h`](#ayther-sdk-version-h) | Header público instalado. |
| [`ayther_sdk.h`](#ayther-sdk-h) | Header público instalado. |
| [`ayther_session.h`](#ayther-session-h) | Header público instalado. |
| [`ayther_unique_handle.h`](#ayther-unique-handle-h) | Header público instalado. |
| [`ayther_version.h`](#ayther-version-h) | Header público instalado. |
| [`ayther_video.h`](#ayther-video-h) | Header público instalado. |
| [`cram_palette.h`](#cram-palette-h) | Header público instalado. |
| [`failure_escalation.h`](#failure-escalation-h) | Header público instalado. |
| [`libretro_host/ayther_api.h`](#libretro-host-ayther-api-h) | Header público instalado. |
| [`libretro_host/core_loader.h`](#libretro-host-core-loader-h) | Header público instalado. |
| [`libretro_host/libretro.h`](#libretro-host-libretro-h) | Header público instalado. |
| [`libretro_host/retro_runner.h`](#libretro-host-retro-runner-h) | Header público instalado. |
| [`output_profile.h`](#output-profile-h) | Header público instalado. |
| [`pano_bands.h`](#pano-bands-h) | Header público instalado. |
| [`panorama_cover.h`](#panorama-cover-h) | Header público instalado. |
| [`parallax_bands.h`](#parallax-bands-h) | Header público instalado. |
| [`psg_synth.h`](#psg-synth-h) | Header público instalado. |
| [`rewind_buffer.h`](#rewind-buffer-h) | Header público instalado. |
| [`voice_router.h`](#voice-router-h) | Header público instalado. |
| [`vulkan_backend/tile_tex_cache.h`](#vulkan-backend-tile-tex-cache-h) | Header público instalado. |
| [`vulkan_backend/vk_context.h`](#vulkan-backend-vk-context-h) | Header público instalado. |
| [`vulkan_backend/vk_indexed_plane.h`](#vulkan-backend-vk-indexed-plane-h) | Header público instalado. |
| [`vulkan_backend/vk_render_target.h`](#vulkan-backend-vk-render-target-h) | Header público instalado. |
| [`vulkan_backend/vk_sprite.h`](#vulkan-backend-vk-sprite-h) | Header público instalado. |
| [`vulkan_backend/vk_texture.h`](#vulkan-backend-vk-texture-h) | Header público instalado. |
| [`widescreen.h`](#widescreen-h) | Header público instalado. |

---

## audio_asset_level.h

audio_asset_level.h — measured level of a decoded audio asset.

`AudioPlayer` produces this value from decoded PCM and `AytherSession`
exposes it to authoring clients. Keeping the value type independent avoids a
cyclic include between those components.

Values answer three pre-publication questions: whether the asset clips,
whether it will be masked by native audio, and which non-destructive gain
adjustment is appropriate.

**Declara:** `AudioAssetLevel`, `ayther`

_El header instalado (`include/ayther/audio_asset_level.h`) lleva la documentación completa de cada símbolo._

---

## audio_bus_balance.h

audio_bus_balance.h — normalización ENTRE BUSES (, segunda mitad).

La primera mitad —medir cada asset (peak, RMS, clipping, corrección
sugerida)— ya está en `audio_asset_level.h` y se ve en Mezclar. Con eso el
autor arregla un asset que clipea o uno que se pierde. Lo que no arregla es
el problema que aparece recién cuando el pack está completo: **la música y
los efectos, cada uno bien por su cuenta, no se llevan bien entre sí**. Un
pack donde cada golpe tapa el tema está compuesto de assets impecables.

Por qué es un cálculo aparte y no «el mismo, promediado»:

  · Se promedia ENERGÍA, no decibeles. El promedio de dB de -6 y -30 da -18,
    que no es el nivel que se oye: el de -6 domina. Promediar la energía y
    recién ahí pasar a dB es lo que da un número que corresponde a lo que
    suena.
  · Se pondera por DURACIÓN. Un tema de tres minutos y un golpe de 200 ms
    no aportan lo mismo a la sensación de volumen de su bus, y contarlos
    igual haría que un pack con muchos efectos cortos pareciera fuerte.
  · Cuenta la ganancia YA AUTORADA. El autor pudo bajar un asset a mano; el
    balance tiene que mirar lo que se va a oír, no lo que dice el archivo.

LA REFERENCIA ES LA MÚSICA. Es el material continuo: es contra lo que el
oído fija el nivel de la escena, y es lo que sigue sonando cuando no pasa
nada. Sin música clasificada, la referencia es el bus con más material
medido — y si sólo hay uno, no hay nada que balancear y se dice.

Lo que este cálculo NO hace: tocar el archivo, ni la ganancia por asset. Su
salida es una corrección POR BUS, que es exactamente lo que
`AytherSession::set_bus_volume` aplica en vivo y el proyecto persiste.
Corregir por bus y no por asset también es lo honesto con el dato: lo que se
midió desbalanceado es el bus.

Sin estado y en el header público —con su oráculo, `audio_bus_balance`—
porque la regla es del CÁLCULO y así se prueba sin motor de audio.

**Declara:** `audio_bus_balance`, `AudioBusBalance`, `AudioBusLevel`, `AudioBusSample`, `ay_db_to_lin`, `ay_lin_to_db`, `ayther`

_El header instalado (`include/ayther/audio_bus_balance.h`) lleva la documentación completa de cada símbolo._

---

## audio_hd_mixer.h

audio_hd_mixer.h — mezclador de voces HD sobre la línea de tiempo de
MUESTRAS del stream principal ().

EL PROBLEMA QUE CIERRA. Los reemplazos HD corrían en streams SDL propios:
arrancaban «ya» respecto del reloj de pared, mientras el original viajaba
por emu_stream_ con ~70 ms de colchón DRC — o sea que la fase entre original
y HD dependía del backlog, de los stalls y del tamaño del catch-up. Acá cada
voz se COLOCA en un sample absoluto de la línea de tiempo del bloque staged
y se suma DENTRO de ese bloque: un disparo en el frame N cae en el mismo
sample ejecutando 1×1 o catch-up 16, y todo —original, router, HD— cruza el
MISMO DRC/backlog. La pausa corta un solo stream y corta todo ().

QUÉ ES ESTE MÓDULO. Sólo la mezcla: voces con PCM ya decodificado y
convertido (S16 estéreo 44100 — lo garantiza el cache del AudioPlayer),
colocación por muestra, loops con fase, ganancia, fade de corte y el
contrato de vida por frame de  (end + tail). NO toca SDL: la mezcla es
una función pura sobre un buffer, y por eso el oráculo de identidad
1×1-vs-catch-up puede ser exacto, byte a byte, sin device.

El ciclo de vida por FRAME (end_frame/cut_frame) se mantiene en frames a
propósito: es el mismo contrato que tick_events y que las ventanas de la
sesión (/) — la conversión frame→sample vive en UN solo lugar (la
colocación del arranque), no repartida por todos los sweeps.

**Declara:** `HdMixer`, `voices_`

_El header instalado (`include/ayther/audio_hd_mixer.h`) lleva la documentación completa de cada símbolo._

---

## audio_live_resume.h

audio_live_resume.h — decisión PURA de reanudación de un reemplazo live
().

Una pausa corta físicamente los streams HD (), pero la instancia LÓGICA
del reemplazo —qué asset, anclado a qué frame, hasta cuándo— sigue viva en
la sesión. Al reanudar hay que volver a sonar DESDE EL OFFSET que dicta el
reloj emulado, no desde cero: limpiar los flancos y re-disparar corrige el
silencio pero introduce desfase, y eso está explícitamente prohibido.

La estrategia elegida es RECREAR el stream desde el offset (no congelar el
stream físico): es la misma aritmética que ya usa el camino de toma
((f - anclaje) / fps), sobrevive a Assets OFF/ON y al cambio de workspace
con el mismo código, y es compatible con la migración futura al mixer
unificado () — la instancia lógica no sabe nada de SDL.

Este header es PURO (sin SDL, sin core) para que la decisión sea testeable
sin sesión ni ROM — mismo criterio que transport_gate.h ().

**Declara:** `ayther`, `live_instance_over`, `live_resume_decide`, `live_resume_offset_bytes`, `LiveResumeDecision`, `uint8_t`

_El header instalado (`include/ayther/audio_live_resume.h`) lleva la documentación completa de cada símbolo._

---

## audio_match_rule.h

audio_match_rule.h — event-substitution matching policy.

Exact signatures distinguish note, channel, and pan changes. An author can
opt into broader matching per assignment, persisted as `match` in
audio_events.toml. Legacy packs remain exact without migration because the
persisted primary key is still `signature`.

This header has no SDL dependency. Its deterministic table lookup is covered
by tests/audio_match_rule_test.cpp.

**Declara:** `AudioMatchIndex`, `AudioMatchRuleInfo`, `ayther`, `clear`, `Entry`, `uint8_t`

_El header instalado (`include/ayther/audio_match_rule.h`) lleva la documentación completa de cada símbolo._

---

## audio_player.h

audio_player.h — SDL3 audio device: emulator passthrough + HD WAV playback.

PCM passthrough via continuous emu_stream_ + one-shot HD SFX streams.
v0.9.1:  Mute-on-substitution — emulator PCM is suppressed for any hash that
         had a resolved HD substitution in the previous tick.  This left a
         1-tick (~16 ms) bleed on the first appearance of a new substitution,
         because the audio callback fires *during* run_frame(), before the
         substitution for the current frame has been resolved.
v0.9.7:  Deferred passthrough — the audio callback no longer pushes PCM
         directly.  It buffers each batch (tagged with its hash) via
         buffer_emulator(); the main loop resolves substitutions for the
same* frame, refreshes the mute set, then calls flush_emulator(),
         which pushes only the non-muted batches.  The mute decision now uses
         the current frame's resolution, eliminating the 1-tick bleed.

Design:
  emu_stream_     Continuous emulator PCM passthrough (S16 stereo, 44100 Hz).

  pending_pcm_    Per-frame interleaved PCM staged by buffer_emulator() and
  pending_batches_  drained by flush_emulator().  Capacity is reused across
                  frames (no steady-state allocation).

  sfx_streams_    One-shot streams for HD WAV substitutions.
                  Created in play_substitutions(), reaped in tick() once drained.

  mute_hashes_    Set of hashes whose emulator PCM should be suppressed this
                  frame.  Refreshed via set_mute_hashes() from resolved subs.

  wav_cache_      WAV assets decoded once per asset_path and held in memory.
                  Freed in shutdown().

Thread model: all public methods are called from the main/emulation thread.
SDL3 audio streams are internally thread-safe for the push side.

Typical per-tick call sequence:

  // inside runner audio callback (called during run_frame()):
  uint64_t hash = ayther_audio_hasher_process_batch(hasher, data, frames);
  audio_player.buffer_emulator(hash, data, frames);

  // after ayther_audio_sub_resolve() — unconditional, clears set when 0:
  audio_player.play_substitutions(pack, audio_subs, n_audio_subs);
  audio_player.set_mute_hashes(audio_subs, n_audio_subs);
  audio_player.flush_emulator();   // pushes non-muted batches to emu_stream_

  // end of frame:
  audio_player.tick();

**Declara:** `AudioPlayer`

_El header instalado (`include/ayther/audio_player.h`) lleva la documentación completa de cada símbolo._

---

## audio_seq_anchor.h

audio_seq_anchor.h — anclas de las Secuencias en replay, con RECLAMO entre
Secuencias (). Header-only y puro: testeable sin SDL, sin core, sin GPU.

Una Secuencia (sub) abre una ventana en cada ocurrencia de su firma
DISPARADORA dentro de los eventos detectados de la toma. Reglas:

 1. Segmentación greedy (reporte 2026-07-23): el paso es el SPAN de los
    eventos; una ocurrencia del disparador que cae dentro del paso de la
    ventana anterior es INTERNA (la melodía repite su primera nota) y no
    re-ancla. Una repetición REAL (tras el paso) sí re-ancla y re-dispara.

 2. RECLAMO (, reporte 2026-08-21): una ocurrencia del disparador de S
    que cae dentro de la ventana (con HD) de OTRA Secuencia T que tiene esa
    firma como MIEMBRO es de T — S no ancla ni dispara. «La que se estaba
    escuchando gana.» Caso real (Golden Axe): «The Battle - Intro» y
    «- Loop» comparten 26 firmas; el hi-hat que abre la Intro reaparece
    cada 63 frames dentro del Loop, y el bajo que abre el Loop aparece
    dentro de la Intro → sonaban las dos a la vez.

 3. CABEZA (reporte 2026-08-21, 2ª vuelta): el disparador solo es frágil —
    en la 3ª pasada del Loop el bajo que lo abre es OTRA firma (variante)
    y los otros 5 canales arrancan idénticos; sin disparador el Loop no
    re-anclaba, su ventana vencía y la Intro se colaba (intro, loop,
    intro, loop…). La cabeza = las firmas que arrancan en el MISMO frame
    que el disparador; una Secuencia también ancla cuando arranca la
    MAYORÍA de su cabeza (≥ ⌈n/2⌉, con n ≥ 2) aunque falte el disparador.

 4. Empate de frame (dos Secuencias arrancan en el MISMO frame): primero
    la CONTINUACIÓN — una en loop cuya ventana vence justo en ese frame
    sigue («siempre la que viene sonando, salvo que haya terminado o
    cambien los eventos»); después la más ESPECÍFICA (menos firmas
    miembro); desempate por id. Determinista.

Los eventos se recorren en orden ascendente de start_frame — el detector
NO los entrega ordenados (los emite por canal), así que la tabla los ordena
(estable). El mismo recorrido alimenta el playback, el mute de los frames
bare y el mixdown del export: UNA tabla, un solo criterio.

**Declara:** `ayther`, `seq_anchor_frame`, `seq_head_quorum`, `seq_sub_before`, `seq_sub_claims`, `SeqAnchorState`, `SeqAnchorSub`

_El header instalado (`include/ayther/audio_seq_anchor.h`) lleva la documentación completa de cada símbolo._

---

## ayther_animation.h

ayther_animation.h — C-S2: play an HD animation **in phase** with the game.

## What this is

An action is an ordered set of timed poses (AnimationClip, C-S1). C-S2
draws it in HD: for whatever pose the game shows **this frame**, draw that
pose's HD frame at the metasprite's on-screen bbox — so the HD animation runs
exactly in sync with gameplay (no free-running loop), driven by the observed
sprite occurrences (each carries `anim_group_id` = the clip, `hash` = the pose,
and its screen bbox).

Tween levels (authoring model section 2.3):
  0 · Pop           — draw the pose's HD frame at the observed bbox (the game
                      already moves the bbox every frame → smooth position).
  1 · Geometric     — interpolate the *transform* (bbox pos/size) between the
                      pose keyframes across the ticks each is held, via the
                      GeometricTween (C-S1, ayther_geo_tween_*), so a single HD
                      per pose glides between keyframes instead of popping.

## Renderer contract

VkSprite today draws a whole texture per AytherSpriteSub. An HD **sheet** (one
image, many pose frames) needs a **sub-rect (UV)** and a float dst rect for
sub-pixel-smooth tweening — this header's `AnimHdFrame`. The renderer-side work
is a `VkSprite::draw_anim(const AnimHdFrame*, n, pack)` variant that samples
the sheet with these UVs (the existing whole-texture path is unchanged).

## Integration status

The source is part of `Ayther::engine`; `AytherSession` resolves frames,
`AytherRenderer` submits the animation lane, and `VkSprite::draw_anim`
samples sheet sub-rectangles. Pack authoring writes animations.toml and the
animation smoke tool exercises the complete path.

This component is a pose-hash-to-frame map, not an independent timeline. The
game supplies the cursor. Level-1 phase state must be reset after detection
loss or non-sequential seeks; callers must not assume backward-seek safety
until that reset contract is enforced.

**Declara:** `AnimationDef`, `AnimationPlayer`, `AnimHdFrame`, `ayther`, `HdPose`, `Impl`

_El header instalado (`include/ayther/ayther_animation.h`) lleva la documentación completa de cada símbolo._

---

## ayther_audio_events.h

ayther_audio_events.h — C-A2: HD substitution of whole audio EVENTS.

## What this is

A per-batch substitution (AudioSubstitutor, ayther_session.h) replaces one
recurring PCM batch hash with an HD asset — right for a short SFX. A sound
with an attack and a tail (a jingle, a voice, music) spans many changing
batches → it needs to be grabbed and replaced as one **event** (a contiguous
run of non-silent batches; AudioEventDetector, core/src/audio_event.rs, keyed
by a stable head `signature`, split on silence or a re-attack — C-A1).

This coordinator turns the detected events + the author's `signature → asset`
assignments into the two per-frame decisions the session's audio needs:

  1. **Range-mute**: while a frame falls inside a substituted event, the
     emulator PCM is suppressed for the WHOLE event — not per hash, because the
     event's batches all have different hashes. The session applies it by
     calling AudioPlayer::discard_emulator() that frame instead of
     flush_emulator() (reusing the existing capability — no player change).

  2. **HD trigger**: at the event's start_frame the HD asset starts playing,
     aligned to the attack (looping until end_frame if the asset loops).

## Session wiring (produce_frame / replay)

  // once, over a take (the Lab authors on a recording; the full timeline is
  // known → events resolve exactly):
  AytherAudioEvent evs[N];
  uint32_t n = ayther_audio_evdet_get_events(detector, evs, N);
  coord.resolve(evs, n);

  // each frame at `frame`:
  if (coord.mute_at(frame)) audio.discard_emulator();
  else                      audio.flush_emulator();
  AudioEventTrigger t[4];
  for (uint32_t i = 0, k = coord.triggers_at(frame, t, 4); i < k; ++i)
      audio.play_event_hd(pack, t[i].asset, t[i].looping);   // OGG/WAV

Live play (ayther_play) uses the same assignments with a short head-latency
variant (a run is recognised once its signature head is complete) — a
follow-up; this contract covers the recording/replay path the Lab authors on.

## Integration status

The detector, substitution FFI, engine coordinator, `AudioPlayer` event
playback, and root target are integrated. Session replay and live paths share
the persisted assignments but retain distinct scheduling policies.

**Declara:** `AudioEventAssignment`, `AudioEventSubstitution`, `AudioEventTrigger`, `ayther`, `Impl`

_El header instalado (`include/ayther/ayther_audio_events.h`) lleva la documentación completa de cada símbolo._

---

## ayther_background_export.h

ayther_background_export.h — Fondos: reconstruct a plane's level strip into a
*per-layer PNG** the artist repaints in HD (componentes-plan §3, the
first-order deliverable).

## What this is

`BackgroundStitcher` (core/src/background.rs) accumulates a scrolling plane's
visible cells into a full level strip in level space over a `.arp` take — no
single frame holds a level, since the nametable wraps. This exporter turns
that accumulated strip into a dense RGBA image and writes it as a PNG whose
name carries the **index** (plane + level origin + a layout hash), so when the
artist brings the repainted HD back it maps unambiguously to its plane and
position: `bg_A_t000x000_<hash>.png`.

The pixels come from decoding each cell's pattern (via a `TileDecodeFn` — the
engine passes AytherSession::decode_plane_tile). Cells the stitcher flagged as
*animated** are still written (as their representative frame) and reported in
`LayerImage::animated`, so the authoring UI can mark them.

## STATUS — contract/skeleton (env-blocked)

The core stitcher + FFI are done and validated (tools/background_spike: 167-tile
strip, 0 conflicts). This header + ayther_background_export.cpp are the
engine-side consumer, ready to wire once the engine builds (needs vcpkg for
stb_image_write). ayther_background_export.cpp is NOT yet in
the root CMakeLists.txt — it type-checks standalone (see its header comment).

**Declara:** `ayther`, `BackgroundExporter`, `LayerImage`

_El header instalado (`include/ayther/ayther_background_export.h`) lleva la documentación completa de cada símbolo._

---

## ayther_components_toml.h

ayther_components_toml.h — round-trip TOML de la capa de Componentes:
`animations.toml` (C-S4) y `audio_events.toml` (C-A4).

El horneo (bake_*) lo llama el Deliver del Lab al construir el pack; el
parseo (parse_*) lo llama AytherSession al cargar un pack (load_pack_into),
re-poblando el AnimationPlayer / el mirror de asignaciones por evento. Ambos
lados viven en el ENGINE (funciones libres, sin UI ni Vulkan) para que el
round-trip sea testeable headless con strings.

Formatos:
  animations.toml (engine-owned):
    [[animation]]
    clip  = "0x<16hex>"          # handle de autoría (clip id)
    sheet = "sheets/run.png"
    tween = 1                     # 0 Pop · 1 tween geométrico
    [[animation.pose]]
    pose   = "0x<16hex>"          # hash de la pose (identidad estable)
    src    = [x, y, w, h]         # sub-rect del sheet (px)
    anchor = [x, y, w, h]         # keyframe dst (Nivel 1)
    ticks  = 6

  audio_events.toml — MISMO esquema que parsea el core Rust
  (AudioSubstitutor::parse_events_toml, cargado por load_from_pack):
    [[event]]
    signature = "0x<16hex>"
    asset     = "audio/music/zone1.ogg"
    loop      = true              # opcional (default false)

  plane_sets.toml — Utilería (CU002) y Glifos (CU005): sustitución HD por
  ELEMENTO multi-tile de plano. Hasta acá el catálogo de Pintar sólo existía
  en la sesión de autoría (inyectado por API), así que el `.ay` entregado NO
  reproducía ninguna sustitución multi-tile; este archivo cierra ese hueco.
    [[font]]
    id = "0x<16hex>" · name = "HUD" · cell_w = 1 · cell_h = 2
    [[set]]
    id      = "0x<16hex>"        # pintar_element_id (determinista por captura)
    name    = "Cofre"            # informativo (overlay/debug)
    type    = "utileria"         # utileria | glifo
    plane   = 0                   # 0=A · 1=B · 2=Window
    w_cells = 3 · h_cells = 2     # bbox
    asset   = "cofre.png"         # basename (el bake lo rutea al tier)
    tiles   = "0x<hash>:cx,cy|…"  # miembros con offset RELATIVO en celdas
    font    = "0x<16hex>" · ch = "A"    # sólo type="glifo"

  Los FLIPS observados al capturar NO se hornean a propósito: el hash de
  tile de plano es flip-invariante y el matcher no los exige (una utilería
  espejada matchea igual). Viven sólo en pintar_elements.toml, que los usa
  para el export fiel del PNG base.

**Declara:** `ayther`, `bake_animations_toml`, `bake_audio_events_toml`, `bake_elements_toml`, `bake_enhance_toml`, `bake_kinematics_toml`, `bake_panoramas_toml`, `bake_plane_sequences_toml`, `bake_plane_sets_toml`, `bake_screens_toml`, `PackEnhance`, `PackInstrument`, `PackKinematic`, `PackKinematicStep`, `PackPanorama`, `PackPanoramaCell`, `PackPlaneFont`, `PackPlaneSeqStep`, `PackPlaneSequence`, `PackPlaneSet`, `PackPlaneSetMember`, `PackScreen`, `PackScreenCell`, `parse_animations_toml`, `parse_audio_events_toml`, `parse_elements_toml`, `parse_enhance_toml`, `parse_instruments_toml`, `parse_kinematics_toml`, `parse_panoramas_toml`, `parse_plane_sequences_toml`, `parse_plane_sets_toml`, `parse_screens_toml`, `plane_sequence_step_at`, `plane_sequence_total`

_El header instalado (`include/ayther/ayther_components_toml.h`) lleva la documentación completa de cada símbolo._

---

## ayther_config.h

ayther_config.h — persistent user configuration for Ayther Engine + Lab.

Storage: %APPDATA%\Ayther\config.toml  (Windows)
         ~/.config/Ayther/config.toml   (Linux / macOS — future)

Created automatically on first run with sensible defaults.
The same config instance is shared between the engine (launcher, ROM paths)
and the Lab (projects directory).  Pass a pointer via ILabPlugin::set_config.

Call sequence:
  AytherConfig config = AytherConfig::load();
  // ... use config.rom_library etc. ...
  config.push_recent(rom_path);
  config.save();

**Declara:** `AytherConfig`

_El header instalado (`include/ayther/ayther_config.h`) lleva la documentación completa de cada símbolo._

---

## ayther_core_ffi.h

C declarations for symbols exported from the Rust ayther_core static lib.
Keep this header in sync with core/src/lib.rs.

Most of the type-safe surface now goes through cxx::bridge (core/src/ffi.rs,
integrated via corrosion). The extern-C wrappers below remain ONLY for the
zero-copy hot path (process_frame / update_ram / set_pack — raw pointers cxx
does not bridge); keep them in sync with lib.rs by hand.

`<stdint.h>` en vez de `<cstdint>` y `extern "C"` bajo guarda: es un paso
hacia poder incluirlo desde C, y desde C++ no cambia nada.

AVISO, para no prometer a medias: este header **todavía no es C puro**. Usa
`bool` y nombres de struct sin `typedef`, así que un `.c` no lo compila. La
API C del SDK es `ayther_sdk.h` —ahí está la superficie pensada para C, con
lectura de packs incluida— y éste es un header de contrato compartido en C++.
Lo destapó el ejemplo `pack_read`, al intentar usarlo desde C.

**Declara:** `AudioEventGate`, `AyArchive`, `ayther_apply_rom_patch`, `ayther_asset_id`, `ayther_asset_id_bytes`, `ayther_audio_evdet_event_count`, `ayther_audio_evdet_flush`, `ayther_audio_evdet_free`, `ayther_audio_evdet_get_events`, `ayther_audio_evdet_new`, `ayther_audio_evdet_push`, `ayther_audio_evdet_set_split_on_reattack`, `ayther_audio_event_active`, `ayther_audio_event_clear_events`, `ayther_audio_event_count`, `ayther_audio_event_finish`, `ayther_audio_event_free`, `ayther_audio_event_get`, `ayther_audio_event_new`, `ayther_audio_event_process_frame`, `ayther_audio_event_process_frame_ex`, `ayther_audio_event_reset`, `ayther_audio_event_set_initial_active`, `ayther_audio_event_set_pal`, `ayther_audio_events_format`, `ayther_audio_events_parse`, `ayther_audio_gate_eval`, `ayther_audio_gate_free`, `ayther_audio_gate_new`, `ayther_audio_hasher_end_tick`, `ayther_audio_hasher_free`, `ayther_audio_hasher_get_occurrences`, `ayther_audio_hasher_new`, `ayther_audio_hasher_process_batch`, `ayther_audio_hasher_unique_count`, `ayther_audio_sub_add_event_override`, `ayther_audio_sub_add_override`, `ayther_audio_sub_catalog_len`, `ayther_audio_sub_clear_event_overrides`, `ayther_audio_sub_clear_overrides`, `ayther_audio_sub_event_catalog_len`, `ayther_audio_sub_free`, `ayther_audio_sub_load_pack`, `ayther_audio_sub_new`, `ayther_audio_sub_resolve`, `ayther_audio_sub_resolve_events`, `ayther_bg_stitcher_animated_cells`, `ayther_bg_stitcher_bounds`, `ayther_bg_stitcher_cell_count`, `ayther_bg_stitcher_conflicts`, `ayther_bg_stitcher_free`, `ayther_bg_stitcher_get`, `ayther_bg_stitcher_new`, `ayther_bg_stitcher_observe`, `ayther_chan_bit`, `ayther_chan_index`, `ayther_chip_name`, `ayther_compat_free`, `ayther_compat_grade`, `ayther_compat_json`, `ayther_compat_reason`, `ayther_compat_unverified`, `ayther_compat_unverified_count`, `ayther_core_version`, `ayther_credits_assets`, `ayther_credits_attribution`, `ayther_credits_author`, `ayther_credits_count`, `ayther_credits_free`, `ayther_credits_licenses`, `ayther_credits_role`, `ayther_engine_version`, `ayther_game_profile_assign`, `ayther_game_profile_entities`, `ayther_game_profile_free`, `ayther_game_profile_kind_count`, `ayther_game_profile_kind_name`, `ayther_game_profile_kind_of_id`, `ayther_game_profile_load`, `ayther_game_profile_load_str`, `ayther_geo_tween_duration`, `ayther_geo_tween_free`, `ayther_geo_tween_new`, `ayther_geo_tween_sample`, `ayther_instruments_soundfonts`, `ayther_is_rom_patch`, `ayther_manifest_schema_supported`, `ayther_pack_build_id`, `ayther_pack_builder_add_bytes`, `ayther_pack_builder_add_file`, `ayther_pack_builder_count`, `ayther_pack_builder_finish`, `ayther_pack_builder_free`, `ayther_pack_builder_new`, `ayther_pack_close`, `ayther_pack_compat`, `ayther_pack_credits`, `ayther_pack_declares_systems`, `ayther_pack_default_profile`, `ayther_pack_entry_count`, `ayther_pack_entry_name`, `ayther_pack_entry_streamable`, `ayther_pack_file_size`, `ayther_pack_game_id`, `ayther_pack_meta_field`, `ayther_pack_open`, `ayther_pack_profile_count`, `ayther_pack_profile_field`, `ayther_pack_profile_index`, `ayther_pack_profile_muted_buses`, `ayther_pack_profile_systems`, `ayther_pack_read`, `ayther_pack_read_range`, `ayther_pack_report_code`, `ayther_pack_report_count`, `ayther_pack_report_free`, `ayther_pack_report_has_errors`, `ayther_pack_report_message`, `ayther_pack_report_severity`, `ayther_pack_schema`, `ayther_pack_set_region`, `ayther_pack_set_tier`, `ayther_pack_set_tier_for_height`, `ayther_pack_systems`, `ayther_pack_tiers`, `ayther_pack_validate`, `ayther_pack_watcher_free`, `ayther_pack_watcher_new`, `ayther_pack_watcher_poll`, `ayther_palette_signature`, `ayther_pose_sub_add_override`, `ayther_pose_sub_add_override_variants`, `ayther_pose_sub_clear_overrides`, `ayther_pose_sub_free`, `ayther_pose_sub_load_pack`, `ayther_pose_sub_new`, `ayther_pose_sub_resolve`, `ayther_pose_sub_set_cram`, `ayther_pose_sub_set_screen`, `ayther_rom_patch_error`, `ayther_script_free`, `ayther_script_get_audio_overrides`, `ayther_script_get_shader_params`, `ayther_script_get_sprite_overrides`, `ayther_script_get_tile_overrides`, `ayther_script_load_string`, `ayther_script_new`, `ayther_script_on_frame`, `ayther_script_set_pack`, `ayther_script_update_audio`, `ayther_script_update_sprites`, `ayther_script_update_tiles`, `ayther_scroll_unwrapper_free`, `ayther_scroll_unwrapper_last_step`, `ayther_scroll_unwrapper_new`, `ayther_scroll_unwrapper_push`, `ayther_sf2_all_notes_off`, `ayther_sf2_bake`, `ayther_sf2_control`, `ayther_sf2_free`, `ayther_sf2_list_presets`, `ayther_sf2_new`, `ayther_sf2_new_shared`, `ayther_sf2_note_off`, `ayther_sf2_note_on`, `ayther_sf2_preset_list`, `ayther_sf2_program`, `ayther_sf2_render`, `ayther_sf2_trim_cache`, `ayther_sonic_read_velocity`, `ayther_sonic_read_xy`, `ayther_soundfont_normalize_file`, `ayther_sprite_hasher_clip_count`, `ayther_sprite_hasher_free`, `ayther_sprite_hasher_get_clip`, `ayther_sprite_hasher_get_occurrences`, `ayther_sprite_hasher_new`, `ayther_sprite_hasher_process_sprites`, `ayther_sprite_hasher_process_vram`, `ayther_sprite_hasher_reset_animation_grouper`, `ayther_sprite_hasher_unique_count`, `ayther_sprite_sub_add_override`, `ayther_sprite_sub_add_override_ref`, `ayther_sprite_sub_clear_overrides`, `ayther_sprite_sub_free`, `ayther_sprite_sub_load_pack`, `ayther_sprite_sub_new`, `ayther_sprite_sub_resolve`, `ayther_subsystem_count`, `ayther_subsystem_name`, `ayther_tile_brightness_factor`, `ayther_tile_hasher_dump_toml`, `ayther_tile_hasher_free`, `ayther_tile_hasher_get_occurrences`, `ayther_tile_hasher_new`, `ayther_tile_hasher_process_frame`, `ayther_tile_hasher_unique_count`, `ayther_tile_mean_level`, `ayther_tile_shape_hash`, `ayther_tile_sub_add_override`, `ayther_tile_sub_begin_frame`, `ayther_tile_sub_clear_overrides`, `ayther_tile_sub_free`, `ayther_tile_sub_load_pack`, `ayther_tile_sub_load_pack_named`, `ayther_tile_sub_lookup`, `ayther_tile_sub_new`, `ayther_tile_sub_resolve`, `ayther_tween_begin_frame`, `ayther_tween_clear`, `ayther_tween_clear_overrides`, `ayther_tween_free`, `ayther_tween_load_pack`, `ayther_tween_new`, `ayther_tween_resolve`, `ayther_tween_set_override`, `ayther_widescreen_gate_eval`, `ayther_widescreen_gate_free`, `ayther_widescreen_gate_new`, `AytherAnimFrame`, `AytherAudioActive`, `AytherAudioActiveSub`, `AytherAudioEvent`, `AytherAudioEventDetector`, `AytherAudioEventSub`, `AytherAudioHasher`, `AytherAudioOccurrence`, `AytherAudioOverride`, `AytherAudioSub`, `AytherAudioSubstitutor`, `AytherAudioWrite`, `AytherBatchEventDetector`, `AytherBgStitcher`, `AytherCompat`, `AytherCredits`, `AytherEventSub`, `AytherGameProfile`, `AytherGeometricTween`, `AytherPackBuilder`, `AytherPackReport`, `AytherPackWatcher`, `AytherPcmEvent`, `AytherScriptEnv`, `AytherScrollUnwrapper`, `AytherSf2`, `AytherShaderParams`, `AytherSpriteHasher`, `AytherSpriteOccurrence`, `AytherSpriteOverride`, `AytherSpriteSub`, `AytherSpriteSubstitutor`, `AytherTileHasher`, `AytherTileOccurrence`, `AytherTileOverride`, `AytherTileSub`, `AytherTileSubstitutor`, `AytherTransform`, `PoseSetSubstitutor`, `struct`, `TweenPlayer`, `WidescreenGate`

_El header instalado (`include/ayther/ayther_core_ffi.h`) lleva la documentación completa de cada símbolo._

---

## ayther_env.h

ayther_env.h — getenv con fallback al prefijo legacy AETHER_ (rebrand de
código 2026-07-25): scripts/arneses viejos que exporten AETHER_* siguen
funcionando sin cambios. Usar SIEMPRE esto para variables AYTHER_*.

**Declara:** `ayther`, `env_get`

_El header instalado (`include/ayther/ayther_env.h`) lleva la documentación completa de cada símbolo._

---

## ayther_layers.h

AytherLayerStack is the engine's first-class layer model.

The stack combines the VDP planes and renderer lanes into one explicit draw
order. Each layer has independent visibility, and custom layers can be
inserted at any position to support effects such as parallax.

Element-level visibility is deliberately not represented here. It belongs
to the session inventory and is controlled through
AytherSession::set_hidden_elements().

**Declara:** `AytherLayer`, `AytherLayerContent`, `AytherLayerStack`, `uint8_t`

_El header instalado (`include/ayther/ayther_layers.h`) lleva la documentación completa de cada símbolo._

---

## ayther_mode3.h

ayther_mode3.h — Mode 3 (RAM anchoring) resolver: metasprite substitution
*per entity instance**.

## What Mode 3 is

Mode 2 (define_metasprite, ayther_session.h) groups SAT sprites by
anim_group_id and substitutes the whole bounding box with one HD asset. It
cannot tell two *identical* on-screen entities apart (two of the same enemy,
Sonic + Tails): their sprites share hashes/anim-groups, so a purely visual
split guesses. Mode 3 removes the guess by reading each entity's **world
position** from game RAM — every instance has a distinct world_pos, so the SAT
sprites split exactly, and each instance gets its HD asset placed at its own
bbox.

## How it plugs in

  AytherSession::produce_frame(), once per frame:
    1. reads the foreground plane camera scroll from the VDP (already computed
       for the Fase 2c plane resolver): Hscroll table (VRAM) + VSRAM.
    2. calls Mode3Resolver::resolve(work_ram, scroll, plane size, sprite_occs).
    3. publishes resolver.subs() on the FrameView (a per-instance HD lane), and
       resolver.instances() for authoring overlays (the Lab draws each anchored
       entity's box + id).

The resolver owns the Rust game profile (game_profile.rs, via the
ayther_game_profile_* FFI) and does the geometry through
ayther_game_profile_assign — the exact path validated against Sonic 2 EHZ in
tools/mode3_spike (VDP scroll read == RAM camera; Sonic/Tails split by
world_pos).

## Integration status

The Rust profile implementation, FFI, engine resolver, session wiring, and
root CMake target are integrated. The design mirrors the existing
SpriteSubstitutor/metasprite flow and publishes results through FrameView.

**Declara:** `ayther`, `EntityInstance`, `Impl`, `Mode3Resolver`

_El header instalado (`include/ayther/ayther_mode3.h`) lleva la documentación completa de cada símbolo._

---

## ayther_rank.h

ayther_rank.h — la ESCALERA de resolución: qué entidad gana cuando varias
matchean el mismo contenido.

Regla del producto (2026-07-26): el match prioriza SIEMPRE de complejidad
MAYOR a MENOR. La entidad que gana RECLAMA su cobertura y las de menor rango
contenidas en ella no se dibujan — no se «tapan» por orden de dibujo, ni
siquiera se emiten.

Por qué acá y no en el renderer: el orden de lanes es una consecuencia, no la
decisión. Cuando la prioridad vive en el orden de dibujo, dos entidades
terminan pintando la misma región y el resultado depende de quién va último
— que es exactamente el bug que tenía el Cuadro (los quads de Utilería de esa
pantalla se dibujaban ENCIMA del Cuadro que ya los contenía).

Antes de esto no existía ninguna noción de prioridad ENTRE tipos: seis
matchers corrían aislados, cada uno escribía su buffer del FrameView y el
renderer los dibujaba todos. Las únicas escaleras eran INTRA-dominio, con dos
arrays de claim incompatibles: `claimed[]` sobre las occurrences de sprite y
`consumed[]` sobre las celdas de plano.

**Declara:** `ayther`, `outranks`, `rank_name`, `uint8_t`

_El header instalado (`include/ayther/ayther_rank.h`) lleva la documentación completa de cada símbolo._

---

## ayther_recording.h

ayther_recording.h — deterministic gameplay recording (.arp).  Ayther R7.

An Ayther Replay Package is NOT a video: it is the deterministic *input* to
the Lab. A recording = an initial savestate + the per-frame input stream.
Replaying = restore the state and re-inject the inputs → the exact same
gameplay, frame for frame, scrubbable (see lab.md §4, lab-engine-split §7).

  take.arp = game_id + name
           + initial savestate (zstd-compressed on disk)
           + input stream (one RetroPad bitmask per frame)

The occurrence history ({slot,hash,anim_group} per frame) + trim marks land
in R7b; this is the recording/replay foundation.

In memory the initial state is kept RAW (ready to unserialize); compression
happens only at save() time. The motor (AytherSession) records into this and
replays from it via replay_seek().

**Declara:** `ayther`, `AytherRecording`, `FrameStat`

_El header instalado (`include/ayther/ayther_recording.h`) lleva la documentación completa de cada símbolo._

---

## ayther_renderer.h

AytherRenderer — the motor's visual (HD) layer (R3).

Consumes a FrameView (the deterministic CPU output of AytherSession::step())
+ a borrowed VkContext, and renders the HD frame into an offscreen VkImage
(VkRenderTarget). The frontend then presents that image:
  - ayther_play blits it to its swapchain;
  - ayther_lab samples it in an ImGui viewport.

Kept SEPARATE from AytherSession on purpose: the session stays Vulkan-free /
headless (CI, determinism, future mobile); the renderer is the swappable GPU
layer. See docs/architecture/r3-render-to-texture.md.

Lifecycle: init(ctx, w, h) → render(ctx, cmd, fv) per frame → shutdown(ctx).
Single-owner; driven from the same thread as the session.

R3.0: scaffold — owns the offscreen target; render() clears it. The emu-frame,
HD-tile, sprite and post-process passes land in R3.1 / R3.2.

**Declara:** `AyArchive`, `ayther`, `AytherLayerStack`, `AytherRenderer`, `FrameView`, `SceneElement`, `VkContext`

_El header instalado (`include/ayther/ayther_renderer.h`) lleva la documentación completa de cada símbolo._

---

## ayther_result.h

ayther_result.h — no-throw error model for the runtime / FFI boundary.

The FFI never propagates exceptions or panics across the binary boundary
(see docs/architecture/ayther-engine.md §4.1). Expected failures (corrupt
pack, malformed TOML, missing ROM) surface as an ayther::Result, so the
caller — especially ayther_lab — can show *why* something failed, not just
that* it failed.

C++20: no std::expected (that is C++23). This is the project's vehicle.

**Declara:** `ayther`, `Error`, `ErrorCode`, `Result`

_El header instalado (`include/ayther/ayther_result.h`) lleva la documentación completa de cada símbolo._

---

## ayther_sdk_version.h

ayther_sdk_version.h — SDK version and compatibility contract.

AYTHER has independent core-ABI, emulator-extension, and pack-format version
axes. This header defines the native SDK version that installed consumers
compile against.

SemVer rule for the current 0.x series: a minor-version change may break the
contract. The CMake package enforces the same rule with SameMinorVersion.

Two values must remain distinguishable:

  - compile-time: `AYTHER_SDK_VERSION_*` from the installed headers;
  - link/run-time: `ayther::sdk_version()` from the linked library.

They can differ when a program finds an unexpected library at run time.
`sdk_version_check()` reports both values so incompatibility fails with a
diagnostic rather than at the first changed layout or call contract.

**Declara:** `ayther`, `SdkVersion`

_El header instalado (`include/ayther/ayther_sdk_version.h`) lleva la documentación completa de cada símbolo._

---

## ayther_sdk.h

**Declara:** `ay_add_audio_observer`, `ay_add_frame_observer`, `ay_add_post_filter`, `ay_audio_events`, `ay_capabilities`, `ay_clear_pack`, `ay_compat_close`, `ay_compat_grade`, `ay_compat_json`, `ay_compat_reason`, `ay_compat_runnable`, `ay_compat_unverified`, `ay_compat_unverified_count`, `ay_core_option_count`, `ay_core_option_desc`, `ay_core_option_key`, `ay_create`, `ay_destroy`, `ay_error_message`, `ay_export_frame`, `ay_export_frame_size`, `ay_extension_active`, `ay_extension_failures`, `ay_frame`, `ay_game_id`, `ay_get_input`, `ay_has_pack`, `ay_last_create_error`, `ay_memory_size`, `ay_pack_close`, `ay_pack_compat`, `ay_pack_entry_count`, `ay_pack_entry_name`, `ay_pack_entry_size`, `ay_pack_entry_streamable`, `ay_pack_game_id`, `ay_pack_open`, `ay_pack_read_entry`, `ay_pack_read_range`, `ay_read_memory`, `ay_remove_extension`, `ay_set_input`, `ay_set_pack`, `ay_step`, `AyAudioEvent`, `AyButton`, `AyCapability`, `AyCompat`, `AyFrame`, `AyPack`, `AyPixelFormat`, `AySession`, `AySessionConfig`, `AyStatus`, `enum`, `int`, `struct`, `void`

_El header instalado (`include/ayther/ayther_sdk.h`) lleva la documentación completa de cada símbolo._

---

## ayther_session.h

ayther_session.h — AytherSession, the motor's control facade (R2).

AytherSession is the single, coherent surface the frontends drive instead of
wiring the ~9 raw ayther_core handles by hand. It owns the whole deterministic
pipeline — emulator host + tile/sprite/audio hashing + substitution + Lua
scripting + HD audio output — behind one object, and exposes the *result* of
each frame as a plain-data FrameView for the frontend to render.

Motor / frontend boundary (see docs/architecture/ayther-engine.md §7):

  AytherSession (motor, this object)        Frontend (ayther_play / ayther_lab)
  ----------------------------------        -----------------------------------
  run_frame() the libretro core             window + SDL events + input source
  hash tiles/sprites/audio                   upload FrameView.fb -> VkTexture
  fire Lua on_frame, resolve overrides       draw tile/sprite substitutions
  resolve substitutions                      CRT post-process / present
  play + mute + flush HD audio   <-- audio   Lab authoring UI / timeline
  produce a FrameView  ----------------->    consume FrameView, render it

Audio output lives entirely inside the session (the AudioPlayer is owned by
the motor in R1): play/mute/flush happen in step(), nothing audio crosses the
boundary. The frontend never touches Vulkan-from-the-motor or SDL-audio.

No-throw: construction and fallible operations return ayther::Result (§4.1.1);
nothing throws across the FFI users. Opaque handles are held as
ayther::unique_handle inside the pimpl — no raw Rust pointer lives loose (§4.1).

Threading: a session is single-owner and must be driven from one thread (the
emulation thread), matching ayther_core's rule. Non-copyable; movable.

**Declara:** `ayther`, `ayther_plane_tile_hash_repalette`, `ayther_plane_tile_hash_variants`, `AytherRecording`, `AytherSession`, `ElementEffect`, `EnhancedElement`, `FrameView`, `HiddenElement`, `PlaneCellHit`, `PlaneTileOccurrence`, `SceneElement`, `subsystem_bit`, `uint8_t`

_El header instalado (`include/ayther/ayther_session.h`) lleva la documentación completa de cada símbolo._

---

## ayther_unique_handle.h

ayther_unique_handle.h — RAII for opaque Rust handles.

No raw opaque pointer from ayther_core lives loose in C++ (see
docs/architecture/ayther-engine.md §4.1, §10). Every handle is wrapped at
creation so Rust frees it deterministically on scope exit — even through an
early return — which matters during hot-reload / scene restart.

The deleter is the matching `_free()` function, carried as a *stateless*
template parameter, so the wrapper is exactly pointer-sized (zero overhead).

Usage:
  using TileHasherPtr =
      ayther::unique_handle<AytherTileHasher, &ayther_tile_hasher_free>;
  TileHasherPtr h{ ayther_tile_hasher_new() };   // freed automatically

**Declara:** `ayther`, `handle_deleter`

_El header instalado (`include/ayther/ayther_unique_handle.h`) lleva la documentación completa de cada símbolo._

---

## ayther_version.h

Canonical AYTHER release and compatibility version contract.

AYTHER_VERSION_* identifies the product release shared by Cargo, CMake, the
native SDK, the engine compatibility check, and the Lua API. The remaining
values are independent protocol revisions and do not follow SemVer.

_El header instalado (`include/ayther/ayther_version.h`) lleva la documentación completa de cada símbolo._

---

## ayther_video.h

ayther_video.h — decoder de video para la Cinemática ().

QUÉ ES: un clip VP9 en contenedor IVF, decodificado a BGRA8 listo para subir
a una textura. No sabe nada de Vulkan ni de la sesión: se le pide un índice
de frame y devuelve píxeles.

POR QUÉ VP9/IVF Y NO FFmpeg — no es gusto, es la frontera GPL del proyecto.
`ayther_engine` es una lib ESTÁTICA; el núcleo de FFmpeg es LGPL-2.1+, así
que linkearlo obligaría a distribución dinámica o a entregar objetos
relinkeables, y cualquier componente --enable-gpl lo volvería GPL. libvpx es
BSD-3 + patent grant. IVF son 32 bytes de header y 12 por frame, o sea que el
demuxer entra en este archivo y no arrastra libavformat. El ENCODER sigue
siendo un ffmpeg.exe EXTERNO del PATH (lab/src/app/ffmpeg_pipe.h): proceso
aparte, sin linkeo, sin pregunta de licencia.

POR QUÉ EL HEADER NO INCLUYE vpx: si `vpx/vpx_decoder.h` entrara acá, el
include dir de libvpx tendría que ser PUBLIC en el CMake del engine y el Lab,
el runtime y los tools pasarían a depender de una librería OPCIONAL. Pimpl.

SIN libvpx (AYTHER_HAVE_VPX apagado) esto compila igual: open() devuelve
false con motivo y validate() rechaza. Un colaborador sin libvpx no queda
bloqueado, y el bake nunca hornea un video sin validar.

STREAMING (): el clip NO reside en RAM. Lee de una `VideoSource` —el pack
por rango o un archivo— y se queda sólo con el índice de paquetes, el paquete
en curso y el frame convertido. Antes copiaba el .ivf entero, y ése era el
motivo real del tope de 32 MB por video del bake: no el formato, sino que
`ayther_pack_read` es todo-o-nada. Retirado el tope, un clip de 8K ocupa lo
mismo que uno de 3 s.

**Declara:** `ayther`, `Impl`, `video_i420_to_bgra_px`, `video_index_build`, `video_index_frames`, `video_index_path`, `video_probe`, `video_source_from_file`, `video_source_from_pack`, `VideoClip`, `VideoFrameView`, `VideoInfo`, `VideoPlane`, `VideoProbe`, `VideoSource`

_El header instalado (`include/ayther/ayther_video.h`) lleva la documentación completa de cada símbolo._

---

## cram_palette.h

cram_palette.h — la CRAM de la Mega Drive, leída ( EM-9.4).

Las cuatro líneas de paleta del VDP: 64 colores de 9 bits que deciden de qué
color se ve cada índice de cada tile. Todo lo demás del pipeline —el hash de
tile de plano, la firma de variante por paleta (), el tinte— se apoya en
esto, y hasta ahora la conversión vivía suelta en tres lugares distintos de
`ayther_session.cpp`.

# El formato: EMPAQUETADO, no el del bus

La CRAM que publica el fork viene EMPAQUETADA —R en los bits 0-2, G en 3-5,
B en 6-8— y **no** en el formato del bus de la Genesis, que deja huecos
(R=1-3, G=5-7, B=9-11). Confundirlos da colores que parecen razonables:
todo sale a la mitad de intensidad y desplazado de tono, que es peor que
salir mal del todo — nadie lo mira dos veces.

Verificado contra el juego: blanco = 0x1FF, azul = 0x1E3 → R3 G4 B7.

# Los 3 bits a 8: NO es `x << 5`

Un componente de 3 bits que se lleva a 8 con un corrimiento nunca llega a
255: el blanco máximo daría 224 y toda la imagen quedaría lavada. La
expansión correcta repite el patrón de bits, que es lo que hace que 7 → 255 y
0 → 0 con los intermedios repartidos parejo.

**Declara:** `ayther`, `cram_c8`, `cram_color`, `cram_color_at`, `cram_line_signature`, `CramColor`

_El header instalado (`include/ayther/cram_palette.h`) lleva la documentación completa de cada símbolo._

---

## failure_escalation.h

failure_escalation.h — cuándo dejar de intentar ().

El fallback de  ya evita que un asset roto corte la sesión: se oye el
original y listo. Lo que falta es la ESCALADA — un pack con muchos assets
rotos reintenta cada uno, cada frame, y paga la resolución completa por algo
que ya se sabe que no va a andar. Ése es el riesgo que  anota.

LA REGLA, que es lo único que hay acá:

  Se cuentan ASSETS DISTINTOS, no ocurrencias.

Un archivo roto que suena mil veces es UN problema; doce archivos distintos
es un pack mal armado o una carpeta que no llegó. Contar ocurrencias apagaría
el subsistema por un solo asset que se repite mucho — que es justo el caso
que NO hay que castigar, porque el fallback ya lo resuelve bien.

Y se cuenta POR SUBSISTEMA: que falte la música no dice nada sobre los
efectos, y apagar los dos por uno sería llevarse puesto lo que sí funciona.

Header-only y sin dependencias: se testea sin sesión, sin audio y sin ROM.

**Declara:** `ayther`, `FailureEscalation`, `threshold_`

_El header instalado (`include/ayther/failure_escalation.h`) lleva la documentación completa de cada símbolo._

---

## libretro_host/ayther_api.h

ayther_api.h — COPIA del contrato del Core Fork. NO editar a mano.

AYTHER_API_VERSION: v1.10 — sincronizar con davidlazarte/Genesis-Plus-GX
                    core/ayther/ayther_api.h (release ayther-abi-1.10,
                    core v1.7.4 752a6ff7; guia: docs/ayther_integration_1.9.md §5.1,
                    que sirve para 1.10)

COPIA ENTERA desde 1.9 (2026-08-26; 1.10 el mismo dia). Hasta 1.3-r2 se traia un recorte «solo
lo que se consume»; con seis versiones aditivas encima (SYSTEM, controles en
Mode 4, LINE_STATE, observabilidad, SPRITE_OUTCOME, Z80_RAM) el recorte ya
no ahorra nada y esconde lo que hay para descubrir. Declarar un tipo NO es
consumirlo: lo que el Engine LEE lo dicen sus suscripciones (pide solo los
bits que consume, no AYTHER_SUB_ALL) y sus chequeos de capability.

Tres cambios de SIGNIFICADO de 1.3 a 1.9, sin cambio de firma (guia §2):
 1. 0x10E / fallback_reasons en Mode 4 dice la verdad (ya no arranca en
    UNSUPPORTED_MODE; el Z80 marca CRAM/VRAM a mitad de frame). El Engine
    nunca tuvo un «es SMS → fallback siempre», asi que solo hay que seguir
    confiando en la mascara.
 2. `v_counter` del journal = primera linea que VE el cambio (N+1). El
    Engine no lo usa.
 3. `recompose_multilayer` con eventos de CRAM cambia de salida (la nueva es
    la correcta, pixel-perfect). Medido con abi_multilayer.

Se copia en vez de referenciar el repo del fork desde CMake: un path
cross-repo ataria el build del Engine al checkout del core, y el contrato
es justamente lo que permite que los dos repos evolucionen por separado.
Al actualizarlo, re-copiar entero y anotar el commit de origen arriba.

 (2026-08-12): el contrato SI cambia — entra `ayther_recompose_status`
con los cuatro motivos de rechazo de la recomposicion. Antes cualquier
rechazo salia como UNSUPPORTED y no habia motivo que loguear. Van en -20..-23
y no en el rango natural porque los `AYTHER_RC_ERR_*` INTERNOS del core ya
ocupan -1..-4, donde viven INVALID_ARGUMENT y compania: devolverlos crudos
haria que «no es modo 5» se leyera como «argumento invalido». Es aditivo —
esos valores no se devolvian antes— asi que un caller que solo compara contra
OK no cambia de comportamiento.

 (2026-08-12): dos defectos que hacian que dos campos de este header
publicaran datos falsos: `dirty_patterns` llegaba siempre vacio (fork ) y
`raster_event_count` estaba clavado en 256 (fork ). Y `cram` se leia con
`movaps` sin estar alineado a 16, asi que `ayther_recompose_multilayer`
CRASHEABA el proceso con -O2 (fork ). Medido en `abi_frame_delta` y
`abi_multilayer`.

 (2026-08-12): trae FRAME_DELTA_V1 (poll_frame_delta + dirty_patterns),
el export directo ayther_recompose_multilayer con sus AYTHER_RC_ERR_*, y el
Audio Probe v2. OJO: `ayther_audio_event_v1` paso sus campos a una UNION y
por lo tanto CAMBIO DE TAMANO — el tamano tiene que salir de `event_size` del
propio core y no de un sizeof local.

2026-08-24, ABI 1.2: la MULTICAPA SE MUDO AL DESCRIPTOR. El simbolo suelto
`ayther_recompose_multilayer` ya no se exporta en el perfil standard, asi que
resolverlo con GetProcAddress devuelve NULL contra un core >= 1.2 y la feature
queda muda SIN un error a la vista. Se resuelve con AYTHER_IFACE_HAS y se cae
al export suelto para los cores 1.0/1.1, que siguen valiendo: la ABI es
aditiva y nada obliga a actualizar el binario.

 (2026-08-12): el AUDIO_EVENT sube a schema 2 — el key-on de PCM ahora
lleva `st`/`ls`, que es lo unico que dice QUE SAMPLE suena (env es volumen y
fd es velocidad de reproduccion: ninguno identifica nada). De paso la fuente
PCM deja de mezclar las dos ramas de la union y usa {reg,data} siempre. Este
es el camino por el que entra el chip PCM de Sega CD al detector, que sigue
siendo el DUENO de la identidad: el core da los HECHOS (st, ls, fd, env, pan)
y `audio_event.rs` calcula la firma, igual que con FM y PSG. Ver el comentario
de la union para el empaquetado exacto.

**Declara:** `ayther_audio_event_type_v1`, `ayther_audio_event_v1`, `ayther_audio_source_v1`, `ayther_audio_transport_stats_v1`, `ayther_audio_voice_v1`, `ayther_audio_write_v1`, `ayther_endianness`, `ayther_frame_delta_v1`, `ayther_frame_hash_v1`, `ayther_frame_snapshot_v1`, `ayther_get_interface`, `ayther_interface_v1`, `ayther_journal_event_v1`, `ayther_journal_v1`, `ayther_legacy_memory_id`, `ayther_line_cells_v1`, `ayther_line_header_v1`, `ayther_line_regs_v1`, `ayther_recompose_error`, `ayther_recompose_multilayer`, `ayther_recompose_stats_v1`, `ayther_recompose_status`, `ayther_region_id`, `ayther_region_info_v1`, `ayther_sprite_v1`, `ayther_status`, `ayther_subscription_state_v1`, `ayther_system_v1`, `int32_t`

_El header instalado (`include/ayther/libretro_host/ayther_api.h`) lleva la documentación completa de cada símbolo._

---

## libretro_host/core_loader.h

**Declara:** `CoreLoader`

_El header instalado (`include/ayther/libretro_host/core_loader.h`) lleva la documentación completa de cada símbolo._

---

## libretro_host/libretro.h

**Declara:** `bool`, `float`, `int`, `int16_t`, `int64_t`, `retro_api_version`, `retro_audio_buffer_status_callback`, `retro_audio_callback`, `retro_av_enable_flags`, `retro_camera_buffer`, `retro_camera_callback`, `retro_cheat_reset`, `retro_cheat_set`, `retro_controller_description`, `retro_controller_info`, `retro_core_option_definition`, `retro_core_option_display`, `retro_core_option_v2_category`, `retro_core_option_v2_definition`, `retro_core_option_value`, `retro_core_options_intl`, `retro_core_options_update_display_callback`, `retro_core_options_v2`, `retro_core_options_v2_intl`, `retro_deinit`, `retro_device_power`, `retro_disk_control_callback`, `retro_disk_control_ext_callback`, `retro_exec_mem_alloc`, `retro_exec_mem_free`, `retro_fastforwarding_override`, `retro_frame_time_callback`, `retro_framebuffer`, `retro_game_geometry`, `retro_game_info`, `retro_game_info_ext`, `retro_get_memory_size`, `retro_get_proc_address_interface`, `retro_get_region`, `retro_get_system_av_info`, `retro_get_system_info`, `retro_hw_context_type`, `retro_hw_render_callback`, `retro_hw_render_context_negotiation_interface`, `retro_hw_render_context_negotiation_interface_type`, `retro_hw_render_interface`, `retro_hw_render_interface_type`, `retro_init`, `retro_input_descriptor`, `retro_key`, `retro_keyboard_callback`, `retro_language`, `retro_led_interface`, `retro_load_game`, `retro_load_game_special`, `retro_location_callback`, `retro_log_callback`, `retro_log_level`, `retro_memory_descriptor`, `retro_memory_map`, `retro_message`, `retro_message_ext`, `retro_message_target`, `retro_message_type`, `retro_microphone_interface`, `retro_microphone_params`, `retro_microphone_t`, `retro_midi_interface`, `retro_mod`, `retro_netpacket_callback`, `retro_perf_callback`, `retro_perf_counter`, `retro_perf_tick_t`, `retro_pixel_format`, `retro_power_state`, `retro_proc_address_t`, `retro_reset`, `retro_rumble_effect`, `retro_rumble_interface`, `retro_run`, `retro_savestate_context`, `retro_sensor_action`, `retro_sensor_interface`, `retro_serialize`, `retro_serialize_size`, `retro_set_audio_sample`, `retro_set_audio_sample_batch`, `retro_set_controller_port_device`, `retro_set_environment`, `retro_set_input_poll`, `retro_set_input_state`, `retro_set_video_refresh`, `retro_subsystem_info`, `retro_subsystem_memory_info`, `retro_subsystem_rom_info`, `retro_system_av_info`, `retro_system_content_info_override`, `retro_system_info`, `retro_system_timing`, `retro_throttle_state`, `retro_time_t`, `retro_unload_game`, `retro_unserialize`, `retro_usec_t`, `retro_variable`, `retro_vfs_dir_handle`, `retro_vfs_file_handle`, `retro_vfs_interface`, `retro_vfs_interface_info`, `size_t`, `uint64_t`, `uintptr_t`, `unsigned`, `void`

_El header instalado (`include/ayther/libretro_host/libretro.h`) lleva la documentación completa de cada símbolo._

---

## libretro_host/retro_runner.h

**Declara:** `RetroRunner`

_El header instalado (`include/ayther/libretro_host/retro_runner.h`) lleva la documentación completa de cada símbolo._

---

## output_profile.h

output_profile.h — perfiles de SALIDA ().

NO son los perfiles de remasterización (), y confundirlos sería el peor
resultado de este issue. Los de  dicen **qué se sustituye** y los decide
el autor del pack; éstos dicen **cómo se ve en TU pantalla** y los decide
quien juega. Un CRT no cambia qué assets entran, y un perfil «Fiel» no cambia
si tenés un plasma o un portátil.

Por eso viven en headers distintos y por eso el vocabulario los separa:
«perfil de remasterización» contra «perfil de salida».

El perfil configura tres cosas: el ESCALADO, el SUAVIZADO y los SHADERS de
presentación. Es todo lo que hay entre el frame ya compuesto y el monitor.

Header-only y sin Vulkan: se testea sin GPU.

**Declara:** `ayther`, `output_profile_by_id`, `output_profiles`, `output_rect`, `OutputProfile`, `OutputRect`, `uint8_t`

_El header instalado (`include/ayther/output_profile.h`) lleva la documentación completa de cada símbolo._

---

## pano_bands.h

pano_bands.h — la cámara de una Panorámica, VOTADA POR BANDA ().

EL PROBLEMA. La Panorámica modela una tira rígida con UNA cámara: cada celda
visible vota `cam_px = lx*8 - screen_x` y gana la moda. Cuando el plano tiene
line-scroll —bandas que se desplazan a distinto ritmo dentro de la MISMA
capa del VDP— no existe una posición que las explique a todas: las celdas de
la banda rápida votan contra las del fondo. En el mejor caso gana la moda y
la banda minoritaria queda mal ubicada; en el peor el voto se parte y el
anclaje no fija.

QUE EL CASO EXISTE está medido, no supuesto (2026-08-24, hscroll_bands_probe):

  Golden Axe   3 tomas, 40.854 frames   reg $B modo 0   0 bandas
  Ecco         1.800 frames             reg $B modo 0   0 bandas
  Aladdin      1.800 frames             reg $B modo 0   0 bandas
  Sonic 3 & K  1.800 frames             tabla por línea en 1.766
                                        plano A: 1 banda · plano B: 37

Golden Axe NO es el corpus de esta feature —sus nubes de título se
resolvieron como dos Acetatos en paralaje, —; Sonic 3 & Knuckles sí.

LA FORMA DE LA SOLUCION. Con 37 bandas, declarar una deriva por tira (la
dirección 2 de la issue) no alcanza: serían 37 velocidades que el autor
tendría que mantener a mano. Se vota POR BANDA, que es lo que hace el
hardware.

Este archivo es sólo el VOTO: agrupa y decide, no lee VRAM, no toca Vulkan y
no sabe qué es una Panorámica. Igual que `widescreen.h`, se puede medir sin
GPU y sin ROM — que es como se encontró el bug de bandas de EM-8.0.

**Declara:** `ayther`, `BandCam`, `pano_band_edges`, `pano_vote_by_band`, `PanoVote`

_El header instalado (`include/ayther/pano_bands.h`) lleva la documentación completa de cada símbolo._

---

## panorama_cover.h

panorama_cover.h — la regla de COBERTURA de una Panorámica ().

«¿Lo que se ve en esta posición ES la lámina, o es otra cosa dibujada encima
del mismo plano?» Una vez que la cámara ancló, hay que contestarla celda por
celda, y de esa cuenta sale `FrameView.panorama_cover`.

VIVE EN UN HEADER Y NO ADENTRO DE LA SESIÓN porque es una regla del FORMATO
de la tira —igual que `ayther_plane_tile_hash_variants`, con el que se
apoya— y porque el defecto que arregla no se podía probar sin una ROM y una
toma de veinte minutos. Acá se prueba con tres hashes inventados.

EL DEFECTO (, medido en Sonic 3 & Knuckles f2092). Una posición de la
tira puede tener VARIOS hashes: una celda animada tiene uno por estado, y un
barrido que cruzó de zona apila dos tramos del nivel en la misma posición. El
índice los guarda todos —cada estado tiene que poder ANCLAR— pero el PNG
conserva UNO (`Cell::last` del stitcher).

Aceptar cualquiera para verificar la cobertura declara «anclada, cobertura
100 %» sobre una lámina que muestra otro tramo del nivel: el recorte exportado
era Angel Island —cielo, agua, pasto— mientras el frame era una cueva.

POR QUÉ CASI NUNCA SE VE: el área nativa se corrige sola, porque las celdas
vivas que la tira no reclamó se dibujan encima y tapan el anclaje flojo. Lo
delata el ensanchado ( EM-8.1), donde el área extendida no tiene con qué
corregirse — ahí se ve exactamente lo que la tira tiene.

LO QUE NO ES EL ARREGLO: un piso de cobertura. Se probaron los dos números
disponibles y ninguno separa los casos (Golden Axe extiende BIEN con 69 %;
Sonic 3 & K extiende MAL con 100 %). Un umbral afinado contra dos puntos es
un parche frágil disfrazado de arreglo.

EL ARREGLO es alinear el índice con el dibujo: se verifica contra el hash que
la lámina CONSERVA y no contra cualquiera de los que pasaron por ahí. Los
demás no se tiran — siguen en el índice de anclaje, donde la multiplicidad
ayuda a votar dónde está la cámara y un voto de más se compensa con los otros
treinta. Lo que no pueden es decidir QUÉ SE DIBUJA donde nadie va a
corregirlo.

**Declara:** `ayther`, `panorama_pos_matches`

_El header instalado (`include/ayther/panorama_cover.h`) lleva la documentación completa de cada símbolo._

---

## parallax_bands.h

parallax_bands.h — la columna de nivel POR BANDA ( EM-8.0).

El plano B lleva parallax por bandas: cada entrada de la tabla Hscroll tiene
su propio desplazamiento, así que «columna de nivel» **no es una sola cosa en
ese plano** — depende de la fila. Con una cámara única por plano, todas las
bandas colapsan en las mismas columnas y se apilan unas sobre otras.

MEDIDO en Sonic 2 (`background_spike`, 1200 frames): el plano A reconstruía 607
columnas de nivel y el B sólo **37** —menos de una pantalla— con 45 bandas por
frame. No era que faltara arte: estaba todo apilado en el lugar equivocado.

DOS COSAS QUE ESTE ARCHIVO APRENDIÓ A LOS GOLPES

1. La regla vive acá y no adentro del bucle de `ayther_session.cpp`. La
   primera versión quedó enterrada ahí, donde el oráculo del stitcher no la
   veía —llama al stitcher directamente—, así que al medirla no movió un solo
   número. No estaba mal: no se estaba ejecutando.

2. No alcanza con restar los H de dos bandas dentro del mismo frame. El campo
   del VDP es de 10 bits y envuelve, y la separación entre bandas CRECE sin
   límite a lo largo de un nivel (medido: 17 px contra 566 px en 1033 px de
   scroll). Cada banda necesita su propio des-enrollado, igual que la cámara
   del plano. Por eso `BandCameras` tiene estado: una resta pura no puede
   saber cuántas vueltas dio cada banda.

**Declara:** `ayther`, `band_count`, `BandCameras`, `hscroll_base`, `hscroll_mask`, `hscroll_of_line`, `State`, `wrap_px`

_El header instalado (`include/ayther/parallax_bands.h`) lleva la documentación completa de cada símbolo._

---

## psg_synth.h

PsgSynth — SN76489 (PSG) propio, con salida POR CANAL. Fase 1 de .

POR QUÉ PROPIO. ymfm cubre el YM2612 pero no el SN76489, y las
implementaciones que andan dando vueltas (MAME, el propio GPGX) son GPL:
ayther_engine es una lib ESTÁTICA y meterle GPL la contamina entera. El chip
es chico de verdad —tres generadores de onda cuadrada y un LFSR— así que
escribirlo sale más barato que discutir licencias.

`gpgx-src/core/sound/psg.c` es la referencia de COMPORTAMIENTO (constantes,
tabla de volumen, red de realimentación del ruido), no de código.

TASA INTERNA. El chip corre a MCLK/15 y divide por 16, o sea un tick cada
15*16 = 240 M-cycles → 223721,56 Hz con el reloj NTSC. Es exactamente 4,2×
la tasa del YM2612 (MCLK/1008), y no es casualidad: 1008/240 = 4,2.

Ese detalle importa. TODOS los incrementos de frecuencia del PSG son
múltiplos de 240 M-cycles, así que cada transición de la onda cae JUSTO en un
borde de tick — muestrear a 223721 Hz es exacto, sin jitter de sub-muestra y
sin necesidad de síntesis band-limited acá. El aliasing se maneja después, al
decimar a la tasa de salida con el resampler.

**Declara:** `ayther`, `PsgSynth`, `shift_noise`

_El header instalado (`include/ayther/psg_synth.h`) lleva la documentación completa de cada símbolo._

---

## rewind_buffer.h

rewind_buffer.h — compressed savestate ring buffer for rewind (R6).

Holds a sliding window of emulator savestates so the frontend can step the
emulation *backwards*. Each ~1 MB savestate is zstd-compressed (level 1,
~30–60 KB) on push, so a 10-second window at 60 fps fits in ~18–36 MB
instead of ~600 MB raw (see ayther-engine.md §6.1).

Model: push() the current state each frame (END of step). rewind_step()
drops the current state and restores the previous one, so holding rewind
walks back one frame per display frame. Disabled by default — when off,
push() is a no-op and there is zero per-frame cost.

Single-threaded; driven from the emulation thread alongside AytherSession.

**Declara:** `ayther`, `RewindBuffer`, `ring_`

_El header instalado (`include/ayther/rewind_buffer.h`) lleva la documentación completa de cada símbolo._

---

## voice_router.h

voice_router — el router de canales por voz (, Fase 2).

LA INVERSIÓN. Hasta ahora la sustitución era SUSTRACTIVA: el chip sonaba
entero y se tapaban canales con una máscara derivada de VENTANAS de eventos.
Una ventana es un MODELO del sonido; el chip ES el sonido, así que todo
instante que la ventana no cubriera pero el chip siguiera sonando era una
fuga —  (huecos entre nota y nota) y  (juntura entre Secuencias) son
el mismo defecto visto en dos lugares.

Acá el default se da vuelta: el chip queda MUDO y todo lo que se oye lo
produce este router. Una voz se toma el canal DESDE EL KEY-ON DEL PROPIO CHIP
hasta el fin de su cola. No hay ventana en la que equivocarse.

LAS DOS PIEZAS
  ChipMirror  — un YM2612 (ymfm) y un SN76489 (PsgSynth) alimentados con el
                mismo log de escrituras que recibe el core, produciendo los
                10 canales POR SEPARADO. Es el sustrato: corre siempre,
                tomen o no las voces su salida.
  ChannelRouter — 10 slots. En cada key-on le pregunta a la política qué debe
                sonar y apunta el slot a esa fuente.

POR QUÉ EL ESPEJO CORRE SIEMPRE: su estado de registros ES la identidad del
sonido. Si se apagara mientras una voz está sustituida, al volver no sabría
con qué timbre sonaba — medido en la Fase 0: arrancar el sintetizador sin el
estado previo baja la correlación de envolvente de 0,975 a 0,889.

**Declara:** `ayther`, `begin`, `buf_`, `ChannelRouter`, `ChipMirror`, `gen_until`, `IVoiceSource`, `reset`, `StreamResampler`, `VoiceContext`, `VoicePolicy`, `Ym2612Mirror`, `ymfm`

_El header instalado (`include/ayther/voice_router.h`) lleva la documentación completa de cada símbolo._

---

## vulkan_backend/tile_tex_cache.h

TileTexCache — lazy-load + cache HD tile textures from an .ay pack.

First access per asset_path:  read bytes from pack → decode PNG (stb_image) →
swap R↔B (RGBA→BGRA) → VkTexture init()+upload(). Subsequent accesses return
the cached VkTexture directly. Owned by AytherRenderer.

**Declara:** `AyArchive`, `StagingRelease`, `TileTexCache`, `VkContext`

_El header instalado (`include/ayther/vulkan_backend/tile_tex_cache.h`) lleva la documentación completa de cada símbolo._

---

## vulkan_backend/vk_context.h

VkContext — Vulkan instance, physical device, logical device, queues, VMA.

One instance per application lifetime.  Destroyed in reverse creation order
inside shutdown() (also called by the destructor).

Uses vk-bootstrap for the boilerplate-heavy device selection / creation.

**Declara:** `SDL_Window`, `vk_verbose_logging`, `VkContext`, `VmaAllocator_T`

_El header instalado (`include/ayther/vulkan_backend/vk_context.h`) lleva la documentación completa de cada símbolo._

---

## vulkan_backend/vk_indexed_plane.h

VkIndexedPlane — el pipeline INDEXADO del render propio (R-2, ).

En vez de decodificar tiles a texturas RGBA, sube el ESTADO CRUDO del VDP y
resuelve el color en el fragment shader:
  - VRAM (2048 patrones 4bpp) → textura R8_UINT de 512×256 (64 tiles/fila,
    cada tile 8×8 texels = su nibble de índice de color desempaquetado).
  - CRAM (64 colores empaquetados R0-2/G3-5/B6-8) → textura 64×1 RGBA8,
    convertida con la MISMA expansión de color que el renderer del core
    (3 bits → nivel ×2 → RGB565 → 888 por replicación de bits), para que el
    resultado sea comparable BIT A BIT contra el framebuffer del emulador.
  - Cada quad de 8×8 px lleva patrón + línea de paleta + flips por push
    constant; el shader hace índice → color. El índice 0 se descarta
    (semántica del VDP: transparente).

Por qué así (de la épica ): un fundido de paleta ya no invalida nada
(cambian 64 texels), y un efecto por elemento es un uniform por quad, no una
lane nueva. Las subidas son INCREMENTALES: shadow CPU de VRAM/CRAM y sólo
los tiles/paleta que cambiaron viajan a la GPU.

Contrato de layouts (idéntico a VkSprite): el render target debe estar en
COLOR_ATTACHMENT_OPTIMAL al entrar a draw_cells(); el pass lo deja en
TRANSFER_DST_OPTIMAL. upload_*() se graban FUERA de un render pass.
Contrato de orden: al menos un upload_vram()+upload_cram() antes del primer
draw_cells() (las imágenes nacen UNDEFINED; draw sin upload es no-op).

**Declara:** `create_images`, `VkContext`, `VkIndexedPlane`, `VkRenderTarget`, `VmaAllocation_T`

_El header instalado (`include/ayther/vulkan_backend/vk_indexed_plane.h`) lleva la documentación completa de cada símbolo._

---

## vulkan_backend/vk_render_target.h

VkRenderTarget — the motor's offscreen color image (R3).

The HD frame is rendered here instead of straight to the swapchain, so each
frontend can decide what to do with it:
  - ayther_play blits it to its swapchain (present to window);
  - ayther_lab samples it in an ImGui viewport panel.

Usage flags cover every consumer in one image:
  COLOR_ATTACHMENT  — the VkSprite / VkPostProcess graphics passes
  TRANSFER_DST      — the emu-frame + HD-tile vkCmdBlitImage blits
  TRANSFER_SRC      — the frontend blit to the swapchain
  SAMPLED           — the Lab viewport (ImGui) / shader sampling

Owned by AytherRenderer and recreated on resize. Destruction is automatic;
shutdown() remains available for deterministic early release.

**Declara:** `VkContext`, `VkRenderTarget`, `VmaAllocation_T`

_El header instalado (`include/ayther/vulkan_backend/vk_render_target.h`) lleva la documentación completa de cada símbolo._

---

## vulkan_backend/vk_sprite.h

vk_sprite.h — Alpha-blended HD sprite overlay pipeline.  Ayther v0.8.0

Renders HD sprite textures (loaded from a .ay pack) over the emulator
framebuffer with standard src-alpha blending.

## Integration into the frame loop

  // swap must be in TRANSFER_DST_OPTIMAL on entry.
  sprite_pipeline.draw(ctx, swap, subs, n_subs, pack, emu_w, emu_h);
  // swap is back in TRANSFER_DST_OPTIMAL on return.
  VkPresent::finalize(ctx, swap);     // unchanged

## Lifecycle

  sprite_pipeline.init(ctx, swap)          // once after swapchain creation
  sprite_pipeline.rebuild(ctx, swap)       // on SDL_EVENT_WINDOW_RESIZED
  sprite_pipeline.shutdown(ctx)            // before vkDeviceWaitIdle / shutdown

## Render-pass layout path (one barrier pair per draw() call)

  TRANSFER_DST  →  [explicit barrier]  →  COLOR_ATTACHMENT
               →  [render pass, loadOp=LOAD, alpha-blend subpass]
               →  [finalLayout auto-transition]  →  TRANSFER_DST

  VkPresent::finalize() then transitions TRANSFER_DST → PRESENT_SRC_KHR
  as usual — no callers need to be changed.

**Declara:** `AyArchive`, `ayther`, `AytherSpriteSub`, `VkContext`, `VkSprite`, `VkTexture`

_El header instalado (`include/ayther/vulkan_backend/vk_sprite.h`) lleva la documentación completa de cada símbolo._

---

## vulkan_backend/vk_texture.h

VkTexture — a single GPU image + staging buffer for CPU→GPU upload.

Used to upload the emulator's software framebuffer (RGB565 / XRGB8888)
every frame.  A persistent staging buffer avoids per-frame allocation.

Usage:
  init(ctx, width, height)        — once
  upload(ctx, cmd, pixels, ...)   — inside a command buffer (begin..end)
  shutdown(ctx)

The image layout at the end of upload() is
  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (suitable for blit src).

**Declara:** `init_cost_`, `TexImageFormat`, `TexPixelFormat`, `VkContext`, `VkTexture`, `VmaAllocation_T`

_El header instalado (`include/ayther/vulkan_backend/vk_texture.h`) lleva la documentación completa de cada símbolo._

---

## widescreen.h

widescreen.h — qué celdas de nivel llenan el área extendida ( EM-8.1).

El ancho de más NO se dibuja con lo que hay en la nametable viva. Eso ya se
midió y no cierra: del lado hacia el que vas, el juego streamea 1-2 celdas por
delante, y 16:9 sobre 224 px pide 5 por lado (7 si se preserva la relación del
4:3 mostrado). El plano A —el gameplay— directamente no tiene arte lateral.

  fuente del lateral    celdas rancias    sin arte
  nametable viva              182            185
  lámina de nivel               0            185

«Rancia» es arte de OTRO tramo del nivel: la nametable envuelve cada 512 px, y
leerla de más devuelve una banda que no corresponde. La lámina del stitcher
—lo que cada posición mostró cuando estuvo en pantalla— pone el nivel real.

Este archivo es sólo el PLAN: qué posición de nivel va en cada celda del área
extendida. No dibuja, no lee VRAM y no toca Vulkan, para que se pueda medir
sin GPU y sin ROM — que es como se encontró el bug de bandas de EM-8.0.

Y la fila importa: en el plano B cada banda de parallax resuelve su propia
columna (ver `parallax_bands.h`). Un área extendida que pidiera una sola
columna por lado dejaría huecos justo donde el parallax separa las bandas.

**Declara:** `ayther`, `Coverage`, `ExtCell`, `widescreen_cols_per_side`, `widescreen_coverage`, `widescreen_plan`, `widescreen_target_width`

_El header instalado (`include/ayther/widescreen.h`) lleva la documentación completa de cada símbolo._

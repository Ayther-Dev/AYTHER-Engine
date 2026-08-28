// ---------------------------------------------------------------------------
// audio_player.cpp — SDL3 audio device: emulator passthrough + HD WAV playback.
//
// Implements AudioPlayer (see audio_player.h).
// v0.9.1: Mute-on-substitution (set_mute_hashes / should_mute).
// v0.9.7: Deferred passthrough (buffer_emulator / flush_emulator) — mutes a hash
//         on its first appearance, removing the 1-tick bleed.
//
// SDL3 audio model used here:
//   • One logical device opened on the default playback device.
//   • One *continuous* SDL_AudioStream (emu_stream_) bound to that device for
//     the emulator's raw PCM.  Data is pushed via submit_emulator() on every
//     retro_audio_sample_batch call.
//   • Zero or more *one-shot* SDL_AudioStream objects (sfx_streams_) bound to
//     the same device for HD WAV substitutions.  Each is created, loaded with
//     WAV data, flushed (signals EOS to SDL), and then reaped in tick() once
//     SDL reports it has no more data to consume.
//
// Mixing is handled by SDL3's audio device: all bound streams are summed
// automatically, so emulator audio and HD substitutions play simultaneously.
// Hashes with an active substitution are suppressed (mute-on-substitution) so
// only the HD asset is heard.
// ---------------------------------------------------------------------------

#include "ayther_env.h"
#include "audio_player.h"
#include "audio_live_resume.h"
#include "ayther_core_ffi.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

// ---------------------------------------------------------------------------
// OGG decode — stb_vorbis  (v0.9.2)
// Include the full implementation in this TU.
// stb_vorbis.c is a C file; compiling it as C++ is supported and tested.
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#  pragma warning(push, 0)
#endif
#include <stb_vorbis.c>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// FLAC decode — dr_flac  (v0.9.2)
// ---------------------------------------------------------------------------
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO   // we feed raw bytes; no FILE* needed
#ifdef _MSC_VER
#  pragma warning(push, 0)
#endif
#include <dr_flac.h>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// Emulator audio spec: Genesis Plus GX outputs S16LE stereo at 44100 Hz.
// ---------------------------------------------------------------------------
static constexpr SDL_AudioSpec kEmuSpec = {
    SDL_AUDIO_S16,  // format  (signed 16-bit little-endian)
    2,              // channels (stereo)
    44100           // freq     (Hz)
};

// ---------------------------------------------------------------------------
// AudioPlayer::init
// ---------------------------------------------------------------------------

bool AudioPlayer::init() {
    // El subsistema de audio, si el host no lo levantó. El Lab lo hace en su
    // SDL_Init, pero un probe que crea una AytherSession no tiene por qué saber
    // que hay un SDL debajo — y sin esto el device no abre y la sesión sigue
    // «muted», que es un oráculo de audio que pasa sin medir nada. SDL cuenta
    // referencias, así que llamarlo de más no molesta a quien ya lo inicializó.
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[AudioPlayer] SDL_InitSubSystem(AUDIO) failed: %s\n",
                     SDL_GetError());
        return false;
    }
    // Open the default playback device.
    // Passing nullptr for spec lets SDL choose the native format; streams
    // with different specs will be resampled/converted automatically.
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device_) {
        std::fprintf(stderr, "[AudioPlayer] SDL_OpenAudioDevice failed: %s\n",
                     SDL_GetError());
        return false;
    }

    // Query the actual device format so we can create matching streams.
    SDL_AudioSpec dev_spec = {};
    SDL_GetAudioDeviceFormat(device_, &dev_spec, nullptr);
    std::fprintf(stdout,
        "[AudioPlayer] device opened — fmt=%d  ch=%d  Hz=%d\n",
        dev_spec.format, dev_spec.channels, dev_spec.freq);

    // Create the continuous emulator stream: S16LE stereo → device native.
    emu_stream_ = SDL_CreateAudioStream(&kEmuSpec, &dev_spec);
    if (!emu_stream_) {
        std::fprintf(stderr,
            "[AudioPlayer] SDL_CreateAudioStream (emu) failed: %s\n",
            SDL_GetError());
        SDL_CloseAudioDevice(device_);
        device_ = 0;
        return false;
    }

    // : tee opcional del PCM del emulador a WAV (AYTHER_AUDIO_DUMP=<ruta>).
    // Header con tamanos placeholder; se parchea al cerrar (shutdown).
    if (const char* dump_path = ayther::env_get("AYTHER_AUDIO_DUMP")) {
        FILE* f = std::fopen(dump_path, "wb");
        if (f) {
            const uint8_t hdr[44] = {
                'R','I','F','F', 0,0,0,0, 'W','A','V','E',
                'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                0x44,0xAC,0,0,             // 44100 Hz
                0x10,0xB1,0x02,0,          // byte rate 44100*4
                4,0, 16,0,
                'd','a','t','a', 0,0,0,0 };
            std::fwrite(hdr, 1, sizeof(hdr), f);
            dump_ = f;
            dump_data_bytes_ = 0;
            std::fprintf(stdout, "[AudioPlayer] tee WAV activo: %s\n", dump_path);
        } else {
            std::fprintf(stderr, "[AudioPlayer] tee WAV: no pude abrir %s\n", dump_path);
        }
    }

    if (!SDL_BindAudioStream(device_, emu_stream_)) {
        std::fprintf(stderr,
            "[AudioPlayer] SDL_BindAudioStream (emu) failed: %s\n",
            SDL_GetError());
        SDL_DestroyAudioStream(emu_stream_);
        emu_stream_ = nullptr;
        SDL_CloseAudioDevice(device_);
        device_ = 0;
        return false;
    }

    // Stream del SINTETIZADOR SoundFont (). Va POR SEPARADO del emulador y
    // no mezclado en `pending_pcm_`, y el motivo es la mute: `flush_emulator`
    // saltea el LOTE ENTERO cuyo hash está muteado, así que un timbre mezclado
    // ahí se iría al silencio junto con la voz que viene a reemplazar — que es
    // exactamente lo contrario de lo que tiene que pasar. Con stream propio,
    // SDL los mezcla en el device y la mute del original no lo toca.
    //
    // F32 porque es lo que RustySynth entrega; 44100 para compartir la
    // aritmética de «un frame de juego = N muestras» con el emulador.
    static constexpr SDL_AudioSpec kSynthSpec = { SDL_AUDIO_F32, 2, 44100 };
    synth_stream_ = SDL_CreateAudioStream(&kSynthSpec, &dev_spec);
    if (synth_stream_ && !SDL_BindAudioStream(device_, synth_stream_)) {
        std::fprintf(stderr, "[AudioPlayer] SDL_BindAudioStream (synth) failed: %s\n",
                     SDL_GetError());
        SDL_DestroyAudioStream(synth_stream_);
        synth_stream_ = nullptr;
    }
    // Un fallo acá NO aborta el init: sin sintetizador el juego suena con su
    // chip, que es la degradación correcta. Abortar dejaría al Lab sin audio.

    // Start playback.
    SDL_ResumeAudioDevice(device_);
    return true;
}

void AudioPlayer::feed_synth(const float* interleaved, size_t frames) {
    if (!synth_stream_ || !interleaved || frames == 0) return;
    SDL_PutAudioStreamData(synth_stream_, interleaved,
                           static_cast<int>(frames * 2 * sizeof(float)));
}

size_t AudioPlayer::synth_queued_frames() const {
    if (!synth_stream_) return 0;
    const int b = SDL_GetAudioStreamQueued(synth_stream_);
    return b > 0 ? size_t(b) / (2 * sizeof(float)) : 0;
}

void AudioPlayer::prime_synth(size_t frames) {
    if (!synth_stream_ || !frames) return;
    std::vector<float> z(frames * 2, 0.0f);
    SDL_PutAudioStreamData(synth_stream_, z.data(),
                           static_cast<int>(z.size() * sizeof(float)));
}

void AudioPlayer::buffer_router(const float* in, size_t frames,
                                bool mix_over_chip) {
    if (!in || !frames) return;
    auto to_s16 = [](float v) {
        const int32_t x = int32_t(std::lrint(v * 32767.0f));
        return int16_t(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
    };
    if (mix_over_chip) {
        //  (Sega CD): lo staged NO es prescindible — lleva el chip PCM y el
        // CDDA, que el router no espeja. Se suma sobre el FRAME ACTUAL, que es
        // lo que el bloque del router cubre; en un catch-up, lo staged son
        // varios frames y el bloque va en el offset del último, no al principio.
        //
        // Sumar era imposible cuando `flush_emulator` empujaba lote por lote
        // salteando los muteados: el audio del router se iba con ellos. Desde
        // / el mute silencia EN SU LUGAR y el bloque sale entero, así
        // que la objeción que dejó escrita esta función ya no aplica.
        const size_t base = frame_mark_ * 2;
        const size_t need = base + frames * 2;
        if (pending_pcm_.size() < need) pending_pcm_.resize(need, 0);
        for (size_t i = 0; i < frames * 2; ++i) {
            const int32_t s = int32_t(pending_pcm_[base + i]) + int32_t(to_s16(in[i]));
            pending_pcm_[base + i] =
                int16_t(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
        }
        // UN lote con el hash reservado: la mezcla ya trae adentro el audio del
        // router, y un memset por hash del chip se lo llevaría puesto. El mute
        // que corresponde a este bloque ya lo aplicó el core, canal por canal.
        pending_batches_.clear();
        pending_batches_.push_back(
            PendingBatch{ kRouterHash, 0, pending_pcm_.size() / 2 });
        return;
    }
    // Cartucho: el bloque OCUPA EL LUGAR de lo staged. El router espeja los diez
    // canales que suenan, así que no se pierde nada al reemplazarlo — y el chip
    // sigue sonando sin muteo para todo lo que lo OBSERVA (el hasher).
    pending_pcm_.clear();
    pending_batches_.clear();
    pending_pcm_.reserve(frames * 2);
    for (size_t i = 0; i < frames * 2; ++i) pending_pcm_.push_back(to_s16(in[i]));
    // Hash reservado: nunca está en el set de mute (que sale de hashes de audio
    // del juego), así que este lote no se puede descartar por error.
    pending_batches_.push_back(PendingBatch{ kRouterHash, 0, frames });
    // : el bloque del router ES el frame actual — las voces HD que
    // dispararon en este frame se colocan en su comienzo.
    frame_mark_ = 0;
}

void AudioPlayer::clear_synth() {
    if (synth_stream_) SDL_ClearAudioStream(synth_stream_);
}

// ---------------------------------------------------------------------------
// AudioPlayer::shutdown
// ---------------------------------------------------------------------------

void AudioPlayer::shutdown() {
    // : cerrar el tee WAV (parchear tamanos RIFF/data).
    if (dump_) {
        FILE* f = static_cast<FILE*>(dump_);
        const uint32_t data_sz = static_cast<uint32_t>(dump_data_bytes_);
        const uint32_t riff_sz = 36u + data_sz;
        std::fseek(f, 4, SEEK_SET);  std::fwrite(&riff_sz, 4, 1, f);
        std::fseek(f, 40, SEEK_SET); std::fwrite(&data_sz, 4, 1, f);
        std::fclose(f);
        dump_ = nullptr;
        std::fprintf(stdout, "[AudioPlayer] tee WAV cerrado (%u bytes de PCM)\n", data_sz);
    }
    stop_all_sfx();
    stop_all_events();
    hd_mixer_.cut_all();     // 
    mix_cache_.clear();
    stop_oneshot();
    mute_hashes_.clear();
    pending_pcm_.clear();
    pending_batches_.clear();

    if (emu_stream_) {
        SDL_UnbindAudioStream(emu_stream_);
        SDL_DestroyAudioStream(emu_stream_);
        emu_stream_ = nullptr;
    }
    if (synth_stream_) {                        // 
        SDL_DestroyAudioStream(synth_stream_);
        synth_stream_ = nullptr;
    }

    if (device_) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }

    wav_cache_.clear();
    level_cache_.clear();   // 
}

// ---------------------------------------------------------------------------
// AudioPlayer::set_muted — gain del device a 0 (mute) / 1 (normal)
// ---------------------------------------------------------------------------

void AudioPlayer::set_muted(bool m) {
    muted_ = m;
    if (device_) SDL_SetAudioDeviceGain(device_, m ? 0.0f : 1.0f);
}

// ---------------------------------------------------------------------------
// AudioPlayer::should_mute / set_mute_hashes  (v0.9.1)
// ---------------------------------------------------------------------------

bool AudioPlayer::should_mute(uint64_t hash) const {
    if (hash == 0) return false;   // silent batch — never mute
    return mute_hashes_.count(hash) > 0 || user_mute_hashes_.count(hash) > 0;
}

void AudioPlayer::set_mute_hashes(const AytherAudioSub* subs, uint32_t count) {
    mute_hashes_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (subs[i].hash != 0)
            mute_hashes_.insert(subs[i].hash);
    }
}

// Mute persistente por hash, pedido por el autor desde la UI (filas de la capa
// AUDIO en el timeline de Editar). Independiente de las sustituciones HD: este
// set vive entre frames y se reaplica una vez al cambiar. El audio es pura
// salida (no afecta el estado del juego) → mutear es seguro y determinista.
void AudioPlayer::set_game_gain(float g) {
    if (g < 0.0f) g = 0.0f;
    if (g > 4.0f) g = 4.0f;
    // Idempotente: se llama por frame desde el tick de la Cinemática y sin este
    // guarda tocaría SDL 60 veces por segundo para nada.
    if (g == game_gain_) return;
    game_gain_ = g;
    if (emu_stream_) SDL_SetAudioStreamGain(emu_stream_, g);
}

void AudioPlayer::set_user_mute_hashes(const uint64_t* hashes, size_t n) {
    user_mute_hashes_.clear();
    for (size_t i = 0; i < n; ++i)
        if (hashes[i] != 0) user_mute_hashes_.insert(hashes[i]);
}

// ---------------------------------------------------------------------------
// AudioPlayer::buffer_emulator / flush_emulator  (v0.9.7)
//
// The audio callback fires during run_frame(), before the current frame's
// substitutions are resolved.  Staging the PCM here and flushing after the mute
// set is known lets us mute a hash on its very first appearance — no 1-tick
// bleed.  Both staging vectors retain capacity across frames, so the steady
// state allocates nothing.
// ---------------------------------------------------------------------------

void AudioPlayer::buffer_emulator(uint64_t hash, const int16_t* data, size_t frames) {
    if (!emu_stream_ || !data || frames == 0) return;

    // Stereo interleaved: each frame = 2 × int16_t.
    const size_t frame_offset = pending_pcm_.size() / 2;
    pending_pcm_.insert(pending_pcm_.end(), data, data + frames * 2);
    pending_batches_.push_back({ hash, frame_offset, frames });
}

void AudioPlayer::flush_emulator(bool suppress_original) {
    if (emu_stream_) {
        // : RE-CEBAR el colchón tras un STALL (seek frío troceado, pausa,
        // arranque). El caso medido: un seek de ~10 s dejó de generar PCM, el
        // device drenó el backlog a ~17 ms, y el dip normal de una escena densa
        // (~10 ms) lo hundió por debajo del consumo → degradación audible "la
        // primera vez"; el DRC (±0.5%) tarda ~30 s en rellenar. Cuando el audio
        // REAPARECE tras >250 ms sin flushes con datos, se antepone SILENCIO
        // hasta el target — el hueco ya existió durante el stall, así que el
        // silencio es inaudible, y la escena entra con el colchón lleno.
        if (!pending_batches_.empty()) {
            const uint64_t now = SDL_GetTicks();
            const bool stalled = last_flush_ms_ == 0 || now - last_flush_ms_ > 250;
            if (stalled) {
                const int queued_bytes = SDL_GetAudioStreamQueued(emu_stream_);
                const int queued = queued_bytes > 0 ? queued_bytes / 4 : 0;
                if (queued < static_cast<int>(kDrcTargetFrames)) {
                    const int prime = static_cast<int>(kDrcTargetFrames) - queued;
                    static std::vector<int16_t> zeros;
                    zeros.assign(static_cast<size_t>(prime) * 2, 0);
                    SDL_PutAudioStreamData(emu_stream_, zeros.data(),
                                           prime * 2 * static_cast<int>(sizeof(int16_t)));
                    drc_queue_avg_ = kDrcTargetFrames;   // no arrastrar el EMA viejo
                    std::fprintf(stderr,
                        "[AudioPlayer] stall %llums → prime %d frames de silencio\n",
                        static_cast<unsigned long long>(
                            last_flush_ms_ ? now - last_flush_ms_ : 0), prime);
                }
            }
            last_flush_ms_ = now;
        }
        {
            // : el bloque conserva su EJE DE TIEMPO — lo muteado se
            // silencia EN SU LUGAR (saltearlo acortaría el bloque y correría
            // la fase de todo lo que sigue), las voces HD se suman en su
            // sample exacto y sale UNA sola mezcla por el mismo DRC/backlog.
            // `suppress_original` = range-mute: el original entero calla, la
            // mezcla HD del bloque sobrevive (el discard viejo tiraba ambos).
            //
            // : hasta acá esto era la rama `unified_` de un if/else. El
            // camino viejo —encolar batch por batch salteando los muteados—
            // se retiró: acortaba el bloque y corría la fase de todo lo que
            // seguía, que es el defecto que  vino a arreglar.
            for (const PendingBatch& b : pending_batches_)
                if (suppress_original || should_mute(b.hash))
                    std::memset(pending_pcm_.data() + b.frame_offset * 2, 0,
                                b.frames * 2 * sizeof(int16_t));
            const size_t frames = pending_pcm_.size() / 2;
            if (frames > 0) {
                hd_mixer_.mix_into(pending_pcm_.data(), frames,
                                   timeline_samples_);
                const int byte_len =
                    static_cast<int>(frames * 2 * sizeof(int16_t));
                SDL_PutAudioStreamData(emu_stream_, pending_pcm_.data(),
                                       byte_len);
                if (dump_) {   // : tee — la MEZCLA final encolada
                    std::fwrite(pending_pcm_.data(), 1,
                                static_cast<size_t>(byte_len),
                                static_cast<FILE*>(dump_));
                    dump_data_bytes_ += static_cast<uint64_t>(byte_len);
                }
                timeline_samples_ += frames;
            }
        }

        // ---- Dynamic rate control (risk ) --------------------------------
        // Nudge the resample ratio so emu_stream_'s backlog stays near a target:
        // the emulator's effective clock then tracks the host device's, instead
        // of drifting into underrun (crackle) or overrun (latency). ±0.5% is
        // below the audible pitch-shift threshold (~1%).
        //
        //  ADAPTATIVO: la telemetría midió que el Lab puede reproducir una
        // toma ~4% más lento que el tiempo real sostenido (ticks de 17-19 ms) —
        // un DRC de ±0.5% nunca alcanza ese ritmo: el backlog vive clavado en
        // el fondo (starving crónico → crackle). El modelo correcto es el de la
        // CONSOLA REAL: si el juego va más lento, el audio SIGUE el ritmo
        // efectivo (pitch levemente más grave, como una Genesis arrastrándose),
        // continuo y sin raspar. Cerca del target el nudge sigue siendo ±0.5%
        // (inaudible); con el backlog por debajo del 50% se abre progresivo
        // hasta ±4% para engancharse a la velocidad real de generación.
        if (drc_enabled_) {
            constexpr float kMaxDeltaNear = 0.005f;   // ±0.5% en régimen normal
            constexpr float kMaxDeltaFar  = 0.04f;    // ±4% enganchando el ritmo real
            constexpr float kTarget   = kDrcTargetFrames;   // ver header ()
            constexpr float kEmaAlpha = 0.05f;     // backlog jitter filter

            const int queued_bytes = SDL_GetAudioStreamQueued(emu_stream_);
            if (queued_bytes >= 0) {
                const float queued = static_cast<float>(queued_bytes) / 4.0f;  // S16 stereo = 4 B/frame
                drc_queue_avg_ = (drc_queue_avg_ <= 0.0f)
                                   ? queued
                                   : drc_queue_avg_ * (1.0f - kEmaAlpha) + queued * kEmaAlpha;

                // dev > 0: backlog too full → ratio > 1 drains it (consume input
                // faster); dev < 0: starving → ratio < 1 lets it fill.
                float dev = (drc_queue_avg_ - kTarget) / kTarget;
                dev = std::clamp(dev, -1.0f, 1.0f);
                // Rampa del delta: normal hasta el 50% del target; de ahí al
                // fondo interpola hacia kMaxDeltaFar (déficit sostenido).
                float max_delta = kMaxDeltaNear;
                const float half = kTarget * 0.5f;
                if (drc_queue_avg_ < half) {
                    const float t = 1.0f - drc_queue_avg_ / half;   // 0 → 1 hacia el fondo
                    max_delta = kMaxDeltaNear + (kMaxDeltaFar - kMaxDeltaNear) * t;
                }
                drc_ratio_ = 1.0f + max_delta * dev;
                SDL_SetAudioStreamFrequencyRatio(emu_stream_, drc_ratio_);

                // : diagnostico de starvation — el DRC (±0.5%) no puede
                // compensar picos de frame-time (decode de texturas, stalls):
                // si el backlog INSTANTANEO cae bajo 1/4 del target, el device
                // esta por raspar el fondo → crackle. Log 1/s para correlacionar
                // con lo que el usuario oye ("[VkSprite] Loaded:" cerca = pico
                // de carga; nada cerca = presupuesto de frame excedido).
                if (queued < kTarget * 0.25f) {
                    ++starved_frames_;
                    const uint64_t now = SDL_GetTicks();
                    if (now - last_starve_log_ms_ > 1000) {
                        last_starve_log_ms_ = now;
                        std::fprintf(stderr,
                            "[AudioPlayer] backlog %.0f frames (<%.0f): starving x%llu\n",
                            queued, kTarget * 0.25f,
                            static_cast<unsigned long long>(starved_frames_));
                    }
                }
            }
        }
    }
    pending_pcm_.clear();       // retains capacity
    pending_batches_.clear();   // retains capacity
}

// ---------------------------------------------------------------------------
// AudioPlayer::discard_emulator  — drop staged PCM without playing it.
// ---------------------------------------------------------------------------

void AudioPlayer::discard_emulator() {
    pending_pcm_.clear();       // retains capacity
    pending_batches_.clear();   // retains capacity
    frame_mark_ = 0;            // : el próximo frame arranca el bloque
}

// ---------------------------------------------------------------------------
// AudioPlayer::play_substitutions
// ---------------------------------------------------------------------------

void AudioPlayer::play_substitutions(AyArchive*            pack,
                                      const AytherAudioSub* subs,
                                      uint32_t              count) {
    if (!device_ || !pack || count == 0) return;

    // Build a set of hashes already playing (from previous ticks that haven't
    // drained yet, plus any already started this tick).
    std::unordered_set<uint64_t> playing;
    for (const auto& s : sfx_streams_)
        playing.insert(s.hash);

    // Get device spec for stream creation.
    SDL_AudioSpec dev_spec = {};
    SDL_GetAudioDeviceFormat(device_, &dev_spec, nullptr);

    for (uint32_t i = 0; i < count; ++i) {
        const AytherAudioSub& sub = subs[i];
        if (sub.asset_path[0] == '\0')   continue;  // no asset assigned
        if (playing.count(sub.hash))     continue;  // already playing

        const std::string path(sub.asset_path);
        const WavEntry* wav = get_wav(pack, path);
        if (!wav || wav->pcm.empty()) continue;

        // Create a one-shot stream: WAV spec → device spec.
        SDL_AudioStream* stream = SDL_CreateAudioStream(&wav->spec, &dev_spec);
        if (!stream) {
            std::fprintf(stderr,
                "[AudioPlayer] CreateAudioStream SFX failed (%s): %s\n",
                path.c_str(), SDL_GetError());
            continue;
        }

        // Push all WAV data and flush to signal end-of-stream.
        SDL_PutAudioStreamData(stream,
                               wav->pcm.data(),
                               static_cast<int>(wav->pcm.size()));
        SDL_FlushAudioStream(stream);   // EOS — stream drains then reports 0

        if (!SDL_BindAudioStream(device_, stream)) {
            std::fprintf(stderr,
                "[AudioPlayer] BindAudioStream SFX failed (%s): %s\n",
                path.c_str(), SDL_GetError());
            SDL_DestroyAudioStream(stream);
            continue;
        }

        sfx_streams_.push_back({ stream, sub.hash });
        playing.insert(sub.hash);

        std::fprintf(stdout, "[AudioPlayer] SFX start: %s  hash=%016" PRIx64 "\n",
                     path.c_str(), sub.hash);
    }
}

// ---------------------------------------------------------------------------
// AudioPlayer::play_oneshot_asset_file — sustitución de audio por evento (C-A4)
// ---------------------------------------------------------------------------

bool AudioPlayer::play_oneshot_asset_file(const std::string& path, uint64_t key,
                                          double offset_seconds, float gain,
                                          bool preview) {
    if (!device_ || path.empty()) return false;
    //  UNIFICADO: los one-shot del GAMEPLAY van al mixer (fase exacta en
    // el bloque staged). Los previews EXPLÍCITOS de autoría siguen en streams
    // propios: no pertenecen al transporte y su corte es aparte () — por
    // eso `sfx_streams_` sigue vivo abajo aunque  haya retirado el camino
    // de streams del gameplay.
    if (!preview) {
        const WavEntry* wav = get_wav_disk(path);
        if (!wav || wav->pcm.empty()) return false;
        const HdMixPcm mix = get_mix_pcm(wav, path);
        if (!mix) return false;
        const uint64_t off = offset_seconds > 0.0
            ? static_cast<uint64_t>(offset_seconds * 44100.0) : 0u;
        // El corte por tail de estos one-shot lo barre la sesión en frames
        // (hd_oneshot_cut → stop_sfx_by_key), igual que en el camino viejo.
        return hd_mixer_.start(key, mix, timeline_samples_ + frame_mark_, off,
                               gain, /*looping=*/false, /*event=*/false,
                               UINT64_MAX, UINT64_MAX);
    }
    // Dos ocurrencias de la MISMA key (p.ej. dos ataques repetidos de una
    // Secuencia) pueden superponerse en el tiempo — en vez de ignorar el
    // disparo nuevo (silencio) o dejarlos sonar juntos sin control (choque),
    // el viejo arranca un fade-out rápido (tick() lo termina) y el nuevo
    // arranca ya mismo.
    const uint64_t now = SDL_GetTicks();
    for (auto& s : sfx_streams_)
        if (s.hash == key && s.fade_start_ms == 0) s.fade_start_ms = now;

    const WavEntry* wav = get_wav_disk(path);
    if (!wav || wav->pcm.empty()) return false;

    // Offset de arranque (el play comenzó DENTRO de la ventana): saltar los
    // primeros cuadros del PCM decodificado, alineado a cuadro completo.
    size_t off = 0;
    if (offset_seconds > 0.0) {
        const size_t bpf =
            static_cast<size_t>(SDL_AUDIO_BYTESIZE(wav->spec.format)) *
            static_cast<size_t>(wav->spec.channels);
        if (bpf > 0)
            off = static_cast<size_t>(offset_seconds * wav->spec.freq) * bpf;
        // Pasa el final: nada que sonar, pero NO es un fallo del asset — el
        // original de esa ventana ya pasó también (: true a propósito).
        if (off >= wav->pcm.size()) return true;
    }

    SDL_AudioSpec dev_spec = {};
    SDL_GetAudioDeviceFormat(device_, &dev_spec, nullptr);
    SDL_AudioStream* stream = SDL_CreateAudioStream(&wav->spec, &dev_spec);
    if (!stream) {
        ++hd_start_fails_;
        std::fprintf(stderr, "[AudioPlayer] CreateAudioStream (evento) falló (%s): %s\n",
                     path.c_str(), SDL_GetError());
        return false;
    }
    SDL_PutAudioStreamData(stream, wav->pcm.data() + off,
                           static_cast<int>(wav->pcm.size() - off));
    // Volumen del HD (slider de la Secuencia); 1 = original.
    if (gain != 1.0f) SDL_SetAudioStreamGain(stream, gain);
    SDL_FlushAudioStream(stream);   // EOS → drena y reporta 0 (tick lo reapea)
    if (!SDL_BindAudioStream(device_, stream)) {
        ++hd_start_fails_;
        std::fprintf(stderr, "[AudioPlayer] BindAudioStream (evento) falló (%s): %s\n",
                     path.c_str(), SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return false;
    }
    sfx_streams_.push_back({ stream, key, 0, preview });
    return true;
}

void AudioPlayer::prewarm_asset_file(const std::string& path) {
    if (!path.empty()) get_wav_disk(path);   // decodifica + cachea (no necesita device)
}

bool AudioPlayer::set_sfx_gain_by_key(uint64_t key, float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    bool any = false;
    for (auto& s : sfx_streams_)
        if (s.hash == key && s.fade_start_ms == 0 && s.stream) {
            SDL_SetAudioStreamGain(s.stream, gain);
            any = true;
        }
    if (hd_mixer_.set_gain(key, gain)) any = true;   // 
    return any;
}

bool AudioPlayer::stop_sfx_by_key(uint64_t key) {
    const uint64_t now = SDL_GetTicks();
    bool cut = false;
    for (auto& s : sfx_streams_)
        if (s.hash == key && s.fade_start_ms == 0) { s.fade_start_ms = now; cut = true; }
    if (hd_mixer_.stop(key)) cut = true;   // : la voz del mixer también
    return cut;
}

size_t AudioPlayer::decode_asset_pcm_s16_44k(const std::string& abs_path,
                                             std::vector<int16_t>& out) {
    out.clear();
    const WavEntry* wav = get_wav_disk(abs_path);   // decode + cache, sin device
    if (!wav || wav->pcm.empty()) return 0;
    SDL_AudioSpec dst{};
    dst.format   = SDL_AUDIO_S16;
    dst.channels = 2;
    dst.freq     = 44100;
    Uint8* conv = nullptr; int conv_len = 0;
    if (!SDL_ConvertAudioSamples(&wav->spec, wav->pcm.data(),
                                 static_cast<int>(wav->pcm.size()),
                                 &dst, &conv, &conv_len) || !conv)
        return 0;
    out.assign(reinterpret_cast<const int16_t*>(conv),
               reinterpret_cast<const int16_t*>(conv + conv_len));
    SDL_free(conv);
    return out.size() / 2;
}

HdMixPcm AudioPlayer::get_mix_pcm(const WavEntry* wav,
                                  const std::string& cache_key) {
    if (!wav || wav->pcm.empty()) return nullptr;
    if (const auto it = mix_cache_.find(cache_key); it != mix_cache_.end())
        return it->second;
    // Al formato del bloque staged (S16 estéreo 44100) UNA vez por asset —
    // la mezcla por bloque no puede pagar resample por voz, ése era el
    // trabajo que hacía SDL en cada stream suelto.
    SDL_AudioSpec dst{};
    dst.format   = SDL_AUDIO_S16;
    dst.channels = 2;
    dst.freq     = 44100;
    Uint8* conv = nullptr; int conv_len = 0;
    if (!SDL_ConvertAudioSamples(&wav->spec, wav->pcm.data(),
                                 static_cast<int>(wav->pcm.size()),
                                 &dst, &conv, &conv_len) || !conv)
        return nullptr;
    auto pcm = std::make_shared<std::vector<int16_t>>(
        reinterpret_cast<const int16_t*>(conv),
        reinterpret_cast<const int16_t*>(conv + conv_len));
    SDL_free(conv);
    mix_cache_[cache_key] = pcm;
    return pcm;
}

double AudioPlayer::asset_duration_seconds(const std::string& abs_path) {
    const WavEntry* wav = get_wav_disk(abs_path);   // decodifica (no necesita device)
    if (!wav || wav->pcm.empty()) return 0.0;
    const int bytes_per_sample = SDL_AUDIO_BYTESIZE(wav->spec.format);
    const int frame_size = bytes_per_sample * wav->spec.channels;
    if (frame_size <= 0 || wav->spec.freq <= 0) return 0.0;
    const double n_frames = static_cast<double>(wav->pcm.size()) / frame_size;
    return n_frames / static_cast<double>(wav->spec.freq);
}

// ---------------------------------------------------------------------------
// AudioPlayer::asset_level — : qué tan fuerte suena, y si clipea
// ---------------------------------------------------------------------------
//
// Se mide sobre el PCM convertido a S16 estéreo 44100 —el mismo que va a la
// mezcla— y no sobre los bytes crudos del archivo. Es lo correcto: lo que el
// autor quiere saber es cómo va a sonar ESO en el juego, no qué decía el
// contenedor. Un OGG de 22 kHz mono que sube a 44,1 estéreo puede cambiar de
// pico, y medir el archivo diría otra cosa que la que se oye.
const AudioPlayer::AssetLevel& AudioPlayer::asset_level(const std::string& abs_path) {
    if (const auto it = level_cache_.find(abs_path); it != level_cache_.end())
        return it->second;

    AssetLevel lv;
    // Formato del contenedor: sale del WavEntry, que es lo que el archivo dice
    // de sí mismo. Sirve para el informe aunque la medición sea sobre el PCM
    // convertido — el autor quiere ver «22 kHz mono» si eso es lo que exportó.
    if (const WavEntry* wav = get_wav_disk(abs_path); wav && !wav->pcm.empty()) {
        lv.sample_rate = wav->spec.freq;
        lv.channels    = wav->spec.channels;
    }

    std::vector<int16_t> pcm;
    const size_t frames = decode_asset_pcm_s16_44k(abs_path, pcm);
    if (frames == 0 || pcm.empty()) {
        return level_cache_.emplace(abs_path, lv).first->second;   // ok = false
    }

    lv.ok         = true;
    lv.duration_s = static_cast<double>(frames) / 44100.0;

    // Un solo paseo: pico, energía y rachas al tope. Son ~2,6 MB por segundo de
    // audio estéreo; recorrerlos dos veces por gusto se nota en un asset largo,
    // y este resultado se cachea justamente porque el panel lo pide por frame.
    constexpr int16_t kFull = 32767;
    double   acc  = 0.0;
    int16_t  peak = 0;
    uint64_t run  = 0;          // muestras consecutivas en el tope
    for (const int16_t s : pcm) {
        const int32_t a = s < 0 ? -static_cast<int32_t>(s) : s;
        if (a > peak) peak = static_cast<int16_t>(a > kFull ? kFull : a);
        acc += static_cast<double>(s) * static_cast<double>(s);
        // -32768 también es tope (asimetría del complemento a dos): usar sólo
        // +32767 dejaría pasar la mitad de las mesetas.
        if (a >= kFull) {
            if (++run == 3) ++lv.clipped_runs;   // se cuenta la racha UNA vez
        } else {
            run = 0;
        }
    }

    const double n = static_cast<double>(pcm.size());
    lv.peak = static_cast<float>(peak) / 32767.0f;
    lv.rms  = static_cast<float>(std::sqrt(acc / n) / 32767.0);

    auto db = [](float lin) -> float {
        return lin > 0.0000001f ? static_cast<float>(20.0 * std::log10(lin)) : -120.0f;
    };
    lv.peak_db = db(lv.peak);
    lv.rms_db  = db(lv.rms);
    lv.clipping = lv.clipped_runs > 0;

    // Los dos extremos que obligan a re-exportar:
    //
    //  · -0,1 dBFS de pico: por encima de eso, cualquier reproducción con
    //    resampleo o suma puede pasarse del tope aunque el archivo no clipee.
    //  · -30 dBFS de RMS: por debajo, el asset queda tapado por el juego y el
    //    autor lo va a subir con ganancia hasta que aparezca el ruido de fondo.
    //
    // Son umbrales de PRÁCTICA de mezcla, no del formato: se pueden discutir,
    // y por eso están acá y no repartidos por la UI.
    lv.too_hot   = lv.peak_db >= -0.1f || lv.clipping;
    lv.too_quiet = lv.rms_db  <  -30.0f;

    // La corrección NO DESTRUCTIVA: cuánto habría que mover el volumen para
    // dejar el pico en -1 dBFS. El archivo no se toca nunca — en AYTHER todas
    // las correcciones son ganancia de reproducción.
    lv.suggested_gain_db = -1.0f - lv.peak_db;

    return level_cache_.emplace(abs_path, lv).first->second;
}

// ---------------------------------------------------------------------------
// AudioPlayer::asset_waveform — : la envolvente, para dibujarla
// ---------------------------------------------------------------------------
//
// Min y max por columna, no un solo valor absoluto: una forma de onda dibujada
// sólo con el máximo no muestra la asimetría, y ahí es donde se ve el offset de
// DC y el recorte de un solo lado — que es la mitad de para qué se mira.
//
// Sobre el MISMO PCM que la mezcla (S16 44,1 estéreo), igual que asset_level:
// lo que el autor quiere ver es lo que va a sonar, no lo que decía el archivo.
const std::vector<float>& AudioPlayer::asset_waveform(const std::string& abs_path,
                                                      uint32_t bins) {
    if (bins == 0) bins = 1;
    const std::string key = abs_path + "#" + std::to_string(bins);
    if (const auto it = wave_cache_.find(key); it != wave_cache_.end())
        return it->second;

    std::vector<float> env;
    std::vector<int16_t> pcm;
    const size_t frames = decode_asset_pcm_s16_44k(abs_path, pcm);
    if (frames == 0 || pcm.empty())
        return wave_cache_.emplace(key, std::move(env)).first->second;   // vacía

    env.assign(static_cast<size_t>(bins) * 2, 0.0f);
    const size_t ch = 2;   // el PCM convertido siempre es estéreo
    for (uint32_t b = 0; b < bins; ++b) {
        const size_t f0 = static_cast<size_t>(frames * (double)b / bins);
        size_t f1 = static_cast<size_t>(frames * (double)(b + 1) / bins);
        // Ningún bin puede quedar vacío: con más columnas que muestras, los
        // huecos se dibujarían como silencio en medio de un sonido.
        if (f1 <= f0) f1 = f0 + 1;
        if (f1 > frames) f1 = frames;
        int16_t lo = 0, hi = 0;
        bool first = true;
        for (size_t f = f0; f < f1; ++f) {
            for (size_t c = 0; c < ch; ++c) {
                const int16_t v = pcm[f * ch + c];
                if (first) { lo = hi = v; first = false; }
                else { if (v < lo) lo = v; if (v > hi) hi = v; }
            }
        }
        env[b * 2 + 0] = static_cast<float>(lo) / 32768.0f;
        env[b * 2 + 1] = static_cast<float>(hi) / 32768.0f;
    }
    return wave_cache_.emplace(key, std::move(env)).first->second;
}

// ---------------------------------------------------------------------------
// AudioPlayer::tick
// ---------------------------------------------------------------------------

void AudioPlayer::tick() {
    // Reap el preview one-shot cuando terminó de drenar.
    if (preview_stream_ && SDL_GetAudioStreamAvailable(preview_stream_) <= 0)
        stop_oneshot();
    // Fade-out rápido (ver play_oneshot_asset_file): a los streams marcados
    // les baja la ganancia linealmente hasta silencio en kSfxFadeOutMs y los
    // destruye — no esperan a drenar su PCM completo, que sonaría entero
    // superpuesto con la instancia nueva de la misma key.
    constexpr uint64_t kSfxFadeOutMs = 60;
    const uint64_t now = SDL_GetTicks();
    // Reap streams whose output data has been fully consumed (available == 0),
    // o cuyo fade-out ya terminó.
    sfx_streams_.erase(
        std::remove_if(sfx_streams_.begin(), sfx_streams_.end(),
            [now](SfxStream& s) {
                if (s.fade_start_ms != 0) {
                    const uint64_t elapsed = now - s.fade_start_ms;
                    if (elapsed >= kSfxFadeOutMs) {
                        SDL_UnbindAudioStream(s.stream);
                        SDL_DestroyAudioStream(s.stream);
                        return true;
                    }
                    const float g = 1.0f - static_cast<float>(elapsed) /
                                           static_cast<float>(kSfxFadeOutMs);
                    SDL_SetAudioStreamGain(s.stream, g);
                    return false;
                }
                if (SDL_GetAudioStreamAvailable(s.stream) > 0)
                    return false;   // still draining — keep alive
                SDL_UnbindAudioStream(s.stream);
                SDL_DestroyAudioStream(s.stream);
                return true;
            }),
        sfx_streams_.end()
    );
}

// ---------------------------------------------------------------------------
// AudioPlayer::stop_all_sfx
// ---------------------------------------------------------------------------

void AudioPlayer::stop_preview_sfx() {
    sfx_streams_.erase(
        std::remove_if(sfx_streams_.begin(), sfx_streams_.end(),
            [](SfxStream& s) {
                if (!s.preview) return false;
                SDL_UnbindAudioStream(s.stream);
                SDL_DestroyAudioStream(s.stream);
                return true;
            }),
        sfx_streams_.end());
}

void AudioPlayer::stop_all_sfx() {
    for (auto& s : sfx_streams_) {
        SDL_UnbindAudioStream(s.stream);
        SDL_DestroyAudioStream(s.stream);
    }
    sfx_streams_.clear();
    hd_mixer_.cut_all_of(/*event=*/false);   // : los one-shot del mixer
}

void AudioPlayer::stop_all_events() {
    // : era el barrido de `event_streams_`; ahora las sustituciones por
    // evento son voces del mixer y se cortan ahí. El corte sigue siendo DURO
    // —es el camino de la pausa () y del cambio de proyecto—, así que no
    // pasa por tail ni por cut_frame.
    hd_mixer_.cut_all_of(/*event=*/true);
}

// ---------------------------------------------------------------------------
// AudioPlayer::cut_transport_audio — pausa = corte total del gameplay ()
// ---------------------------------------------------------------------------

uint64_t AudioPlayer::cut_transport_audio() {
    // SFX one-shot del GAMEPLAY — los previews explícitos de autoría no son
    // del transporte y siguen sonando (criterio de : pausar Capturar no
    // corta el Reproducir de Mezclar).
    sfx_streams_.erase(
        std::remove_if(sfx_streams_.begin(), sfx_streams_.end(),
            [](SfxStream& s) {
                if (s.preview) return false;
                SDL_UnbindAudioStream(s.stream);
                SDL_DestroyAudioStream(s.stream);
                return true;
            }),
        sfx_streams_.end());
    // Event-streams (HD del pack): todos pertenecen al gameplay.
    stop_all_events();
    // : las voces del mixer también son del gameplay, TODAS — el corte
    // duro no deja fade (el staging se descarta entero acá abajo y un fade
    // sin bloque que lo lleve no suena: sería una voz zombi).
    hd_mixer_.cut_all();

    // PCM en vuelo: staging del frame + lo ya encolado en los streams
    // continuos. El corte tiene que alcanzar el backlog (~70 ms de colchón
    // DRC): sin vaciarlo, la pausa deja ese resto sonando tras el botón.
    uint64_t frames = pending_pcm_.size() / 2;
    pending_pcm_.clear();
    pending_batches_.clear();
    frame_mark_ = 0;   // 
    if (emu_stream_) {
        const int q = SDL_GetAudioStreamQueued(emu_stream_);
        if (q > 0) frames += static_cast<uint64_t>(q) / 4;   // S16 estéreo
        SDL_ClearAudioStream(emu_stream_);
        SDL_SetAudioStreamFrequencyRatio(emu_stream_, 1.0f);
    }
    if (synth_stream_) {
        const int q = SDL_GetAudioStreamQueued(synth_stream_);
        if (q > 0) frames += static_cast<uint64_t>(q) / (2 * sizeof(float));
        SDL_ClearAudioStream(synth_stream_);
    }

    // DRC: nada del régimen pre-pausa debe cruzarla. last_flush_ms_ = 0 hace
    // que el primer flush con datos tras reanudar entre por el camino de
    // stall (flush_emulator) y re-cebe el colchón con silencio hasta el
    // target — el primer frame post-pausa queda con comportamiento definido.
    drc_ratio_     = 1.0f;
    drc_queue_avg_ = 0.0f;
    last_flush_ms_ = 0;

    if (frames > 0) {
        pause_cut_frames_ += frames;
        ++pause_cuts_;
    }
    return frames;
}

// ---------------------------------------------------------------------------
// AudioPlayer — HD por EVENTO (C-A2, Componentes)
// ---------------------------------------------------------------------------

bool AudioPlayer::play_event_hd(AyArchive* pack, const char* asset_path, bool looping,
                                uint64_t signature, uint64_t end_frame,
                                uint64_t cut_frame, double start_offset_seconds,
                                uint32_t fade_frames, float gain, size_t loop_begin, size_t loop_end)
{
    if (!device_ || !asset_path || asset_path[0] == '\0') return false;
    const WavEntry* wav = get_wav(pack, asset_path);
    if (!wav || wav->pcm.empty()) return false;

    //  UNIFICADO: voz del mixer en vez de stream SDL propio — colocada
    // en el sample donde empieza el frame actual dentro del bloque staged,
    // así la fase contra el original no depende del backlog ni del catch-up.
    // El contrato // se conserva: offset con módulo para loops,
    // end/cut por frame (tick_frame), pasado el final = éxito sin voz.
    //
    // : el camino de streams SDL por evento que vivía debajo se retiró.
    // Era la salida de emergencia mientras el mixer era nuevo; su fase contra
    // el original dependía del backlog, que es justo lo que  corrigió.
    const HdMixPcm mix = get_mix_pcm(wav, asset_path);
    if (!mix) return false;
    const uint64_t off = start_offset_seconds > 0.0
        ? static_cast<uint64_t>(start_offset_seconds * 44100.0) : 0u;
    return hd_mixer_.start(signature, mix,
                           timeline_samples_ + frame_mark_, off,
                           gain > 0.0f ? gain : 1.0f,
                           looping, /*event=*/true, end_frame, cut_frame,
                           fade_frames, loop_begin, loop_end);
}

void AudioPlayer::tick_events(uint64_t frame) {
    // : el contrato de vida —end_frame, tail, cut_frame— lo aplica el
    // mixer.  retiró el barrido de `event_streams_` que hacía lo mismo
    // para el camino de streams: destruía el stream SDL al pasar cut_frame,
    // re-alimentaba los loops y recogía los non-loop drenados.
    hd_mixer_.tick_frame(frame);
}

bool AudioPlayer::stop_event(uint64_t signature) {
    // : la voz del mixer con esta firma sale DURA — es el camino del
    // retrigger y del mute a mitad, mismo contrato que destruir el stream.
    // (: ya no hay stream que destruir; el mixer es el único que suena.)
    return hd_mixer_.stop_hard(signature);
}

// ---------------------------------------------------------------------------
// AudioPlayer::play_oneshot_pcm / stop_oneshot — vista previa de audio (Capas)
// ---------------------------------------------------------------------------

void AudioPlayer::play_oneshot_pcm(const int16_t* pcm, size_t frames) {
    if (!device_ || !pcm || frames == 0) return;
    stop_oneshot();   // reemplaza el preview anterior

    SDL_AudioSpec src = {};
    src.format = SDL_AUDIO_S16; src.channels = 2; src.freq = 44100;
    SDL_AudioSpec dev = {};
    SDL_GetAudioDeviceFormat(device_, &dev, nullptr);
    preview_stream_ = SDL_CreateAudioStream(&src, &dev);
    if (!preview_stream_) {
        std::fprintf(stderr, "[AudioPlayer] preview CreateAudioStream: %s\n", SDL_GetError());
        return;
    }
    SDL_PutAudioStreamData(preview_stream_, pcm,
                           static_cast<int>(frames * 2 * sizeof(int16_t)));
    SDL_FlushAudioStream(preview_stream_);   // EOS → drena y reporta 0
    if (!SDL_BindAudioStream(device_, preview_stream_)) {
        SDL_DestroyAudioStream(preview_stream_);
        preview_stream_ = nullptr;
    }
}

void AudioPlayer::stop_oneshot() {
    if (preview_stream_) {
        SDL_UnbindAudioStream(preview_stream_);
        SDL_DestroyAudioStream(preview_stream_);
        preview_stream_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// AudioPlayer::get_wav  (private) — WAV / OGG / FLAC  (v0.9.2)
// ---------------------------------------------------------------------------

namespace {

/// Lowercase file extension of asset_path, without the leading dot.
/// Returns "" if no extension is found.
std::string asset_ext(const std::string& path) {
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

} // anonymous namespace

// Decodifica WAV/OGG/FLAC desde bytes (compartido por get_wav y get_wav_disk).
bool AudioPlayer::decode_audio_bytes(WavEntry& entry,
                                     const std::vector<uint8_t>& raw,
                                     const std::string& ext) {
    // ---- WAV decode (SDL3) ----------------------------------------------
    if (ext == "wav") {
        SDL_IOStream* io = SDL_IOFromMem(const_cast<uint8_t*>(raw.data()), raw.size());
        if (!io) {
            std::fprintf(stderr, "[AudioPlayer] SDL_IOFromMem failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_AudioSpec spec    = {};
        uint8_t*      buf     = nullptr;
        uint32_t      buf_len = 0;
        if (!SDL_LoadWAV_IO(io, /*close_io=*/true, &spec, &buf, &buf_len)) {
            std::fprintf(stderr, "[AudioPlayer] SDL_LoadWAV_IO: %s\n", SDL_GetError());
            return false;
        }
        entry.spec = spec;
        entry.pcm.assign(buf, buf + buf_len);
        SDL_free(buf);
    }
    // ---- OGG/Vorbis decode (stb_vorbis) ---------------------------------
    else if (ext == "ogg") {
        int channels = 0, sample_rate = 0;
        short* decoded = nullptr;
        const int n_samples = stb_vorbis_decode_memory(
            raw.data(), static_cast<int>(raw.size()), &channels, &sample_rate, &decoded);
        if (n_samples < 0 || !decoded) {
            std::fprintf(stderr, "[AudioPlayer] stb_vorbis_decode_memory failed\n");
            return false;
        }
        entry.spec.format = SDL_AUDIO_S16; entry.spec.channels = channels; entry.spec.freq = sample_rate;
        const size_t total_bytes = static_cast<size_t>(n_samples) * channels * sizeof(short);
        entry.pcm.assign(reinterpret_cast<uint8_t*>(decoded),
                         reinterpret_cast<uint8_t*>(decoded) + total_bytes);
        free(decoded);   // stb_vorbis allocates with malloc
    }
    // ---- FLAC decode (dr_flac) ------------------------------------------
    else if (ext == "flac") {
        drflac_uint32 channels = 0, sample_rate = 0;
        drflac_uint64 total_frames = 0;
        drflac_int16* decoded = drflac_open_memory_and_read_pcm_frames_s16(
            raw.data(), raw.size(), &channels, &sample_rate, &total_frames, nullptr);
        if (!decoded) {
            std::fprintf(stderr, "[AudioPlayer] drflac decode failed\n");
            return false;
        }
        entry.spec.format = SDL_AUDIO_S16;
        entry.spec.channels = static_cast<int>(channels);
        entry.spec.freq     = static_cast<int>(sample_rate);
        const size_t total_bytes = static_cast<size_t>(total_frames) * channels * sizeof(drflac_int16);
        entry.pcm.assign(reinterpret_cast<uint8_t*>(decoded),
                         reinterpret_cast<uint8_t*>(decoded) + total_bytes);
        drflac_free(decoded, nullptr);
    }
    else {
        std::fprintf(stderr, "[AudioPlayer] formato no soportado: '%s'\n", ext.c_str());
        return false;
    }
    return !entry.pcm.empty();
}

namespace {

// : fingerprint barato de un archivo de disco (mtime + tamaño). false si
// no existe/no se puede statear. Es lo que invalida la cache negativa: un
// fallo se memoriza JUNTO con el archivo que falló, no para siempre.
bool stat_fingerprint(const std::string& path, int64_t* mtime, uint64_t* size) {
    std::error_code ec;
    const auto st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(st)) return false;
    const auto mt = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) return false;
    *mtime = static_cast<int64_t>(mt.time_since_epoch().count());
    *size  = static_cast<uint64_t>(sz);
    return true;
}

const char* asset_error_str(AudioPlayer::AssetError e) {
    switch (e) {
        case AudioPlayer::AssetError::Missing:     return "missing";
        case AudioPlayer::AssetError::Empty:       return "empty";
        case AudioPlayer::AssetError::Unsupported: return "unsupported";
        case AudioPlayer::AssetError::Corrupt:     return "corrupt";
        case AudioPlayer::AssetError::None:        break;
    }
    return nullptr;
}

}  // anonymous namespace

const AudioPlayer::WavEntry*
AudioPlayer::get_wav(AyArchive* pack, const std::string& asset_path) {
    auto it = wav_cache_.find(asset_path);
    if (it != wav_cache_.end())
        return it->second.err == AssetError::None ? &it->second : nullptr;
    // Cache negativa PERMANENTE (): el pack es inmutable por contenido —
    // lo que falló una vez falla siempre, y el log de abajo sale UNA vez.
    WavEntry& entry = wav_cache_[asset_path];
    entry.err = AssetError::Missing;
    if (!pack) return nullptr;

    const int64_t sz = ayther_pack_file_size(pack, asset_path.c_str());
    if (sz <= 0) {
        std::fprintf(stderr, "[AudioPlayer] asset not in pack: %s\n", asset_path.c_str());
        return nullptr;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    if (ayther_pack_read(pack, asset_path.c_str(), raw.data(), raw.size()) <= 0) {
        entry.err = AssetError::Corrupt;   // firmado y no se pudo leer/verificar
        return nullptr;
    }
    const std::string ext = asset_ext(asset_path);
    if (!decode_audio_bytes(entry, raw, ext)) {
        entry.err = (ext == "wav" || ext == "ogg" || ext == "flac")
                        ? AssetError::Corrupt : AssetError::Unsupported;
        return nullptr;
    }
    entry.err = AssetError::None;
    return &entry;
}

const AudioPlayer::WavEntry*
AudioPlayer::get_wav_disk(const std::string& abs_path) {
    auto it = wav_cache_.find(abs_path);
    if (it != wav_cache_.end()) {
        WavEntry& e = it->second;
        const uint64_t now = SDL_GetTicks();
        if (e.err == AssetError::None) {
            // Positiva: TAMBIÉN revalida por fingerprint, throttled (reporte
            // 2026-08-22: reemplazar el WAV de la Intro/Loop en disco servía
            // el PCM viejo hasta reiniciar el Lab — la positiva era «estable»
            // para siempre). Una voz que ya suena no se corta: el mixer
            // retiene su HdMixPcm compartido; el PRÓXIMO disparo decodifica
            // el archivo nuevo. Si el archivo desapareció, se sigue sirviendo
            // lo cargado (cortar una voz por un unlink transitorio sería
            // peor).
            if (now - e.last_check_ms < kAssetRecheckMs) return &e;
            e.last_check_ms = now;
            int64_t  mt = 0;
            uint64_t sz = 0;
            if (!stat_fingerprint(abs_path, &mt, &sz) ||
                (mt == e.fp_mtime && sz == e.fp_size))
                return &e;
            wav_cache_.erase(it);        // cambió → recarga por el camino frío
            level_cache_.erase(abs_path);   // : el nivel era del archivo viejo
            mix_cache_.erase(abs_path);     // el PCM de mezcla también
        } else {
            // Cache NEGATIVA (): reintentar SOLO si el archivo apareció o
            // cambió desde el intento fallido (fingerprint mtime+tamaño), y
            // statear como mucho cada kAssetRecheckMs — sin esto, una asignación
            // rota consultada por frame paga un stat por frame y el log escupe
            // 60 veces por segundo.
            if (now - e.last_check_ms < kAssetRecheckMs) return nullptr;
            e.last_check_ms = now;
            int64_t  mt = 0;
            uint64_t sz = 0;
            if (!stat_fingerprint(abs_path, &mt, &sz)) return nullptr;  // sigue sin existir
            if (mt == e.fp_mtime && sz == e.fp_size) return nullptr;    // sigue igual
            wav_cache_.erase(it);   // cambió → reintento completo por el camino frío
            // : el nivel medido era del archivo VIEJO. Dejarlo sería informar
            // el pico de un asset que ya no existe — el hot-reload es justo cuando
            // el autor re-exporta para arreglar el clipping que le marcamos.
            level_cache_.erase(abs_path);
        }
    }
    WavEntry& entry = wav_cache_[abs_path];
    entry.err = AssetError::Missing;
    entry.last_check_ms = SDL_GetTicks();
    stat_fingerprint(abs_path, &entry.fp_mtime, &entry.fp_size);
    std::ifstream f(abs_path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "[AudioPlayer] no se pudo abrir el asset: %s\n", abs_path.c_str());
        return nullptr;
    }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) {
        entry.err = AssetError::Empty;
        std::fprintf(stderr, "[AudioPlayer] asset vacío: %s\n", abs_path.c_str());
        return nullptr;
    }
    f.seekg(0);
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(raw.data()), sz)) {
        entry.err = AssetError::Corrupt;
        return nullptr;
    }
    const std::string ext = asset_ext(abs_path);
    if (!decode_audio_bytes(entry, raw, ext)) {
        entry.err = (ext == "wav" || ext == "ogg" || ext == "flac")
                        ? AssetError::Corrupt : AssetError::Unsupported;
        return nullptr;
    }
    entry.err = AssetError::None;
    return &entry;
}

// ---------------------------------------------------------------------------
// Disponibilidad de assets () — la pregunta previa a silenciar el original
// ---------------------------------------------------------------------------

bool AudioPlayer::asset_ready_disk(const std::string& abs_path) {
    if (abs_path.empty()) return false;
    const WavEntry* w = get_wav_disk(abs_path);
    return w && !w->pcm.empty();
}

bool AudioPlayer::asset_ready_pack(AyArchive* pack, const std::string& asset_path) {
    if (!pack || asset_path.empty()) return false;
    const WavEntry* w = get_wav(pack, asset_path);
    return w && !w->pcm.empty();
}

const char* AudioPlayer::asset_error_name(const std::string& path) const {
    const auto it = wav_cache_.find(path);
    if (it == wav_cache_.end()) return nullptr;   // nunca se intentó
    return asset_error_str(it->second.err);
}

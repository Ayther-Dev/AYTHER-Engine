// ---------------------------------------------------------------------------
// mute_replacement_probe — #329: SILENCIAR ES SILENCIAR.
//
// Hasta el 2026-07-28 el mute y la SUSTITUCIÓN eran dos caminos separados y
// sólo uno miraba el mute: silenciar un sonido callaba su voz del chip y dejaba
// sonando lo que la REEMPLAZA (el asset HD, el timbre de SoundFont). O sea que
// el altavoz apagaba el original y dejaba el reemplazo — al revés de lo que el
// artista pide.
//
// Este probe es el oráculo de que la decisión de mute es ÚNICA. Lo verifica
// sobre una toma REAL, sin oído:
//
//   1. Sin mute, el HD de la Secuencia se dispara (hd_muted = 0).
//   2. Silenciado el TIMBRE de sus miembros, el HD deja de dispararse.
//   3. Silenciadas sus OCURRENCIAS (el ojo de la Secuencia), idem.
//   4. Silenciado UN SOLO timbre de una Secuencia de varios, el HD SIGUE
//      sonando: un asset es una mezcla, no se puede silenciar a medias.
//   5. El camino viejo no se rompió: el ORIGINAL sigue muteado (audio_mute_mask).
//   6. Con el ROUTER puesto, el mute por ocurrencia y el mute por canal llegan
//      a la voz del chip. Con el chip mudo la máscara no dice nada (es 0x3FF
//      siempre), así que se mide por los key-ons NO copiados del router.
//   7. Con device (dummy) y transporte reproduciendo: silenciar A MITAD de un
//      asset lo CORTA (hd_cut > 0). Es el punto que muerde — si el mute sólo
//      evitara el próximo disparo, silenciar a mitad de un asset largo no se
//      oiría hasta la próxima repetición y desde afuera se ve igual que «el
//      mute no funciona».
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target mute_replacement_probe
//   Args:  <core.dll> <rom> <recording.arp> [asset.wav]
//          sin asset.wav: corren 1-6 (headless, sin device).
//          con asset.wav: corre además el 7 (device SDL "dummy", no se oye).
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

using ayther::AytherSession;

namespace {

int g_fails = 0;

void check(bool ok, const char* what, const char* detail = "") {
    std::printf("  %s %s%s%s\n", ok ? "[ok]  " : "[FAIL]", what,
                detail[0] ? " — " : "", detail);
    if (!ok) ++g_fails;
}

uint64_t occ_key(uint8_t chip, uint8_t channel, uint32_t start) {
    return (uint64_t(chip) << 56) | (uint64_t(channel) << 48) | start;
}

uint16_t chan_bit(uint8_t chip, uint8_t channel) {
    return chip == 0 ? uint16_t(1u << channel) : uint16_t(1u << (6 + channel));
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) { out.resize(size_t(n)); std::fread(out.data(), 1, out.size(), f); }
    std::fclose(f);
    return out;
}

/// Setea una variable en el entorno del CRT DE ESTE EXE. No sirve
/// SDL_setenv_unsafe: escribe en el entorno de SDL3.dll, que es otro CRT, y
/// `ayther::env_get` (que es std::getenv) no la ve. Vale para las AYTHER_*; las
/// SDL_* sí van por SDL, que es quien las lee.
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (value && value[0]) setenv(name, value, 1); else unsetenv(name);
#endif
}

std::string temp_path(const char* name) {
    const char* dir = std::getenv("TEMP");
    if (!dir) dir = std::getenv("TMPDIR");
    return dir ? std::string(dir) + "/" + name : std::string(name);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "mute_replacement_probe — #329: el mute alcanza al reemplazo\n"
            "uso: %s <core.dll> <rom> <recording.arp> [asset.wav]\n", argv[0]);
        return 2;
    }
    const std::string asset = argc > 4 ? argv[4] : std::string();
    const bool with_device = !asset.empty();
    // Sin buffer: si algo revienta a mitad, importa MÁS saber en qué chequeo
    // estaba que ahorrarse las escrituras.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Device "dummy": abre de verdad (los streams existen, que es lo que hace
    // falta para medir el CORTE) pero no suena — un probe no debe ponerse a
    // reproducir por los parlantes del que lo corre.
    if (with_device) {
        if (!SDL_getenv("SDL_AUDIODRIVER")) SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "[FAIL] SDL_Init(AUDIO): %s\n", SDL_GetError());
            return 1;
        }
    }

    AytherSession::Config cfg;
    cfg.core_path    = argv[1];
    cfg.rom_path     = argv[2];
    cfg.enable_audio = with_device;
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<AytherSession>& s = *r;

    auto rec_opt = ayther::AytherRecording::load(argv[3]);
    if (!rec_opt) { std::fprintf(stderr, "[FAIL] no pude cargar %s\n", argv[3]); return 1; }
    const ayther::AytherRecording& rec = *rec_opt;

    const uint32_t nev = s->analyze_audio_events(rec);
    const AytherAudioEvent* evs = s->audio_events();
    std::printf("toma: %s — %u frames, %u eventos%s\n\n", argv[3], rec.frame_count(),
                nev, with_device ? "  (con device dummy)" : "  (headless)");
    if (nev < 8) { std::fprintf(stderr, "[FAIL] pocos eventos (%u)\n", nev); return 1; }

    // -- La Secuencia sintética ---------------------------------------------
    // Disparador: la firma FM que MÁS se repite (así hay varias anclas que
    // recorrer y el re-disparo entra en juego). Miembros: esa firma más las
    // que caen dentro de su span — como agrupa la autoría real.
    uint64_t trig_sig = 0; uint32_t trig_reps = 0;
    for (uint32_t i = 0; i < nev; ++i) {
        if (evs[i].chip != 0) continue;
        uint32_t reps = 0;
        for (uint32_t j = 0; j < nev; ++j) if (evs[j].signature == evs[i].signature) ++reps;
        if (reps > trig_reps) { trig_reps = reps; trig_sig = evs[i].signature; }
    }
    if (trig_reps < 2) { std::fprintf(stderr, "[FAIL] sin firma FM repetida\n"); return 1; }

    uint32_t trig_start = 0, trig_next = 0;
    for (uint32_t i = 0; i < nev; ++i)
        if (evs[i].signature == trig_sig) {
            if (!trig_start && !trig_next) { trig_start = evs[i].start_frame; continue; }
            if (!trig_next) { trig_next = evs[i].start_frame; break; }
        }
    // Ventana = hasta la repetición siguiente (o 120 frames), que es el span
    // que la autoría le daría al grupo.
    const uint32_t span = trig_next > trig_start ? trig_next - trig_start : 120u;

    AytherSession::AudioSeqSub sub;
    sub.trigger_signature = trig_sig;
    sub.duration_frames   = span;
    sub.span_frames       = span;
    sub.key               = 0x329;
    sub.gain              = 1.0f;
    sub.asset             = with_device ? asset : "probe.wav";
    std::unordered_set<uint64_t> member_sigs;
    std::unordered_set<uint64_t> member_insts;
    for (uint32_t i = 0; i < nev; ++i) {
        if (evs[i].start_frame < trig_start || evs[i].start_frame >= trig_start + span)
            continue;
        member_sigs.insert(evs[i].signature);
        member_insts.insert(evs[i].instrument);
        sub.channel_mask |= chan_bit(evs[i].chip, evs[i].channel);
    }
    sub.signatures.assign(member_sigs.begin(), member_sigs.end());
    std::printf("Secuencia sintética: disparador %016llx ×%u, ventana %u frames,\n"
                "  %zu firmas miembro, %zu timbres, canales 0x%03x\n\n",
                (unsigned long long)trig_sig, trig_reps, span,
                member_sigs.size(), member_insts.size(), sub.channel_mask);

    s->set_audio_substitution_preview(true);
    s->set_audio_sequence_subs({ sub });
    if (with_device) s->set_transport_playing(true);

    // Recorre la ventana de la PRIMERA ancla y un poco más, y devuelve cuántos
    // disparos de HD quedaron silenciados / cortados en esa pasada, más si el
    // ORIGINAL siguió muteado (que es el camino viejo, que no debe romperse).
    const uint32_t f0 = trig_start;
    const uint32_t f1 = std::min(rec.frame_count(), trig_start + span * 2 + 30);
    struct Pass { uint64_t muted, cut; uint32_t orig_muted_frames; };
    auto pass = [&](bool router) -> Pass {
        s->set_voice_router(router);
        s->replay_invalidate();
        uint64_t m0 = 0, c0 = 0;
        s->audio_mute_stats(&m0, &c0);
        uint32_t orig = 0;
        for (uint32_t f = f0; f < f1; ++f) {
            const ayther::FrameView* fv = s->replay_seek(rec, f);
            if (fv && (fv->audio_mute_mask & sub.channel_mask)) ++orig;
        }
        uint64_t m1 = 0, c1 = 0;
        s->audio_mute_stats(&m1, &c1);
        return { m1 - m0, c1 - c0, orig };
    };
    auto set_inst_mute = [&](const std::vector<uint64_t>& v) {
        s->set_audio_instrument_mute(v.empty() ? nullptr : v.data(), uint32_t(v.size()));
    };
    auto set_occ_mute = [&](const std::vector<uint64_t>& v) {
        s->set_audio_occurrence_mute(v.empty() ? nullptr : v.data(), uint32_t(v.size()));
    };

    // -- 1. Sin mute: el HD suena -------------------------------------------
    std::printf("1. sin mute\n");
    const Pass base = pass(false);
    check(base.muted == 0, "el HD se dispara (nada suprimido)");

    // -- 2. Mute por TIMBRE de todos los miembros ---------------------------
    std::printf("2. silenciados los %zu timbres de la Secuencia (altavoz del Patrón)\n",
                member_insts.size());
    set_inst_mute(std::vector<uint64_t>(member_insts.begin(), member_insts.end()));
    const Pass by_inst = pass(false);
    check(by_inst.muted > 0, "el HD YA NO se dispara",
          by_inst.muted ? "" : "sigue sonando el reemplazo (#329)");
    check(by_inst.orig_muted_frames > 0, "y el ORIGINAL sigue muteado (camino viejo intacto)");
    set_inst_mute({});

    // -- 3. Mute por OCURRENCIA (el ojo de la Secuencia) --------------------
    std::vector<uint64_t> occs;
    for (uint32_t i = 0; i < nev; ++i)
        if (member_sigs.count(evs[i].signature))
            occs.push_back(occ_key(evs[i].chip, evs[i].channel, evs[i].start_frame));
    std::printf("3. silenciadas las %zu ocurrencias miembro (ojo de la Secuencia)\n", occs.size());
    set_occ_mute(occs);
    const Pass by_occ = pass(false);
    check(by_occ.muted > 0, "el HD YA NO se dispara");
    set_occ_mute({});

    // -- 4. Mute PARCIAL: un asset no se silencia a medias -------------------
    if (member_insts.size() > 1) {
        std::printf("4. silenciado UN SOLO timbre de %zu\n", member_insts.size());
        set_inst_mute({ *member_insts.begin() });
        const Pass partial = pass(false);
        check(partial.muted == 0, "el HD SIGUE sonando (la Secuencia conserva voces audibles)");
        set_inst_mute({});
    } else {
        std::printf("4. (omitido: la Secuencia tiene un solo timbre)\n");
    }

    // -- 5. El MIXDOWN exportado tampoco mezcla el HD silenciado -------------
    // El playback y el export son dos renders distintos: el mixdown arma la
    // mezcla por su cuenta (anclas + PCM decodificado), así que la fuga podía
    // sobrevivir al arreglo del playback y llegar igual al MP4.
    //
    // El oráculo es exacto, no estadístico: con la Secuencia silenciada, la
    // mezcla CON sub y SIN sub tienen que salir byte a byte iguales — y sin
    // silenciar, distintas (ahí el HD sí suma).
    if (with_device) {
        std::printf("5. mixdown exportado (el WAV del MP4)\n");
        const uint32_t mx_end = std::min(rec.frame_count(), f0 + span * 2);
        auto mixdown = [&](bool with_sub, const std::vector<uint64_t>& insts,
                           const char* name) -> std::vector<uint8_t> {
            s->set_audio_sequence_subs(with_sub ? std::vector<AytherSession::AudioSeqSub>{ sub }
                                                : std::vector<AytherSession::AudioSeqSub>{});
            set_inst_mute(insts);
            const std::string p = temp_path(name);
            const bool ok = s->export_mixdown_wav(rec, f0, mx_end, p.c_str(), /*hd=*/true);
            if (!ok) return {};
            std::vector<uint8_t> b = read_file(p);
            std::remove(p.c_str());
            return b;
        };
        const std::vector<uint64_t> all_insts(member_insts.begin(), member_insts.end());
        const auto a = mixdown(true,  {}, "ayther_329_a.wav");   // HD, sin mute
        const auto b = mixdown(false, {}, "ayther_329_b.wav");   // sin HD, sin mute
        const auto c = mixdown(true,  all_insts, "ayther_329_c.wav");   // HD, silenciado
        const auto d = mixdown(false, all_insts, "ayther_329_d.wav");   // sin HD, silenciado
        set_inst_mute({});
        check(!a.empty() && !b.empty() && !c.empty() && !d.empty(),
              "los cuatro mixdowns se exportaron");
        check(a != b, "sin mute el HD SUMA a la mezcla (control)");
        check(c == d, "silenciado, el HD NO llega al export");
        s->set_audio_sequence_subs({ sub });
    } else {
        std::printf("5. (omitido: el mixdown necesita el asset — pasar asset.wav)\n");
    }

    // -- 6. El router también escucha el mute --------------------------------
    // Sin subs ni asignaciones: lo ÚNICO que puede sacar una voz de la copia es
    // el mute. Antes de #329 el router sólo miraba el mute por instrumento, así
    // que el ojo de una Secuencia y el mute por canal se perdían con el chip
    // mudo — sin síntoma visible en la máscara, que ya es 0x3FF.
    if (with_device) {
        std::printf("6. router puesto, sin sustituciones\n");
        s->set_audio_sequence_subs({});
        auto router_substituted = [&](const std::vector<uint64_t>& insts,
                                      const std::vector<uint64_t>& oc,
                                      uint16_t manual) -> uint64_t {
            set_inst_mute(insts);
            set_occ_mute(oc);
            s->set_audio_manual_mute(manual);
            s->set_voice_router(true);
            s->replay_invalidate();
            uint64_t before = 0;
            s->voice_router_stats(nullptr, nullptr, nullptr, nullptr, &before);
            for (uint32_t f = f0; f < f1; ++f) s->replay_seek(rec, f);
            uint64_t after = 0;
            s->voice_router_stats(nullptr, nullptr, nullptr, nullptr, &after);
            set_inst_mute({}); set_occ_mute({}); s->set_audio_manual_mute(0);
            return after - before;
        };
        const uint64_t none = router_substituted({}, {}, 0);
        const uint64_t occ  = router_substituted({}, occs, 0);
        const uint64_t man  = router_substituted({}, {}, sub.channel_mask);
        std::printf("   key-ons no copiados: sin mute %llu · por ocurrencia %llu · por canal %llu\n",
                    (unsigned long long)none, (unsigned long long)occ,
                    (unsigned long long)man);
        check(none == 0, "sin mute el router copia todo");
        check(occ > 0,   "el mute por OCURRENCIA calla la voz del chip");
        check(man > 0,   "el mute por CANAL calla la voz del chip");
        s->set_voice_router(false);
        s->set_audio_sequence_subs({ sub });
    } else {
        std::printf("6. (omitido: el router necesita device — pasar asset.wav)\n");
    }

    // -- 7. Corte A MITAD del asset -----------------------------------------
    if (with_device) {
        std::printf("7. silenciar a mitad de un asset ya sonando\n");
        s->set_voice_router(false);
        s->replay_invalidate();
        uint64_t m0 = 0, c0 = 0;
        s->audio_mute_stats(&m0, &c0);
        // Arrancar la ventana sin mute (el HD se dispara)…
        const uint32_t mid = f0 + span / 2;
        for (uint32_t f = f0; f < mid; ++f) s->replay_seek(rec, f);
        // …y silenciar con el asset en el aire.
        set_inst_mute(std::vector<uint64_t>(member_insts.begin(), member_insts.end()));
        for (uint32_t f = mid; f < f1; ++f) s->replay_seek(rec, f);
        uint64_t m1 = 0, c1 = 0;
        s->audio_mute_stats(&m1, &c1);
        std::printf("   suprimidos %llu · CORTADOS %llu\n",
                    (unsigned long long)(m1 - m0), (unsigned long long)(c1 - c0));
        check(c1 - c0 > 0, "el asset en vuelo se CORTA",
              c1 - c0 ? "" : "sigue hasta el final: el mute no se oye hasta la próxima repetición");
        set_inst_mute({});
    } else {
        std::printf("7. (omitido: el corte necesita device — pasar asset.wav)\n");
    }

    // -- 8. El router no deja colar el chip ----------------------------------
    // Desde #326 el router es el DEFAULT, y calla al chip ocupándole el lugar en
    // el staging del player — ya no muteándolo en el core (eso dejaba al hasher
    // de audio viendo silencio y las tomas salían sin hashes). El riesgo del
    // cambio es exactamente el inverso: un frame en que el router no rinda
    // bloque y se empuje el PCM CRUDO del chip.
    //
    // Se mide sobre lo que se encola AL DEVICE (AYTHER_AUDIO_DUMP tee-a
    // justamente eso): con TODO silenciado, el device tiene que recibir
    // silencio. Si el chip se colara, ahí habría señal. La corrida sin
    // silenciar es el control — si diera silencio también, la prueba no
    // probaría nada.
    if (with_device) {
        std::printf("8. router puesto (default): lo que llega al device\n");
        s.reset();   // libera el device antes de abrir la sesión del tee
        auto device_rms = [&](uint16_t manual) -> double {
            const std::string wav = temp_path(manual ? "ayther_326_mute.wav"
                                                     : "ayther_326_libre.wav");
            set_env("AYTHER_AUDIO_DUMP", wav.c_str());
            double rms = -1.0;
            {
                auto r2 = AytherSession::create(cfg);
                if (!r2) return -1.0;
                std::unique_ptr<AytherSession>& s2 = *r2;
                // SIN analyze_audio_events a propósito: re-emula la toma
                // ENTERA y ese pase no va por el router, así que su PCM
                // ensuciaba el tee y tapaba lo que se quiere medir (fue el
                // primer resultado: 728 de «silencio» que era el análisis).
                // El router no necesita eventos — sin ellos toda voz cae en
                // copia, que para esta prueba es el caso PEOR.
                s2->set_transport_playing(true);
                s2->set_audio_manual_mute(manual);
                for (uint32_t f = f0; f < f1; ++f) s2->replay_seek(rec, f);
            }   // el destructor cierra el tee y parchea la cabecera RIFF
            set_env("AYTHER_AUDIO_DUMP", "");
            const std::vector<uint8_t> b = read_file(wav);
            if (b.size() > 44) {
                const int16_t* pcm = reinterpret_cast<const int16_t*>(b.data() + 44);
                const size_t n = (b.size() - 44) / 2;
                double acc = 0.0;
                for (size_t i = 0; i < n; ++i) acc += double(pcm[i]) * pcm[i];
                rms = n ? std::sqrt(acc / double(n)) : 0.0;
            }
            std::remove(wav.c_str());
            return rms;
        };
    const double unmuted = device_rms(0);
    const double muted  = device_rms(0x3FF);
        std::printf("   rms al device: sin silenciar %.1f · todo silenciado %.1f\n",
                unmuted, muted);
        check(unmuted > 100.0, "sin silenciar SÍ llega audio (control)");
    check(muted >= 0.0 && muted < 1.0,
              "con todo silenciado llega SILENCIO",
          muted >= 1.0 ? "el chip se está colando por debajo del router" : "");
    } else {
        std::printf("8. (omitido: necesita device — pasar asset.wav)\n");
    }

    std::printf("\n%s\n", g_fails == 0
        ? "[OK] silenciar calla también al reemplazo (#329)"
        : "[FAIL] el reemplazo sobrevive al mute — ver arriba");
    // La sesión ANTES que SDL_Quit: el AudioPlayer cierra device y streams en
    // su destructor, y hacerlo sobre un SDL ya terminado revienta al salir.
    s.reset();
    if (with_device) SDL_Quit();
    return g_fails == 0 ? 0 : 5;
}

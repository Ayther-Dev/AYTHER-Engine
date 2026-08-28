// ---------------------------------------------------------------------------
// segacd_pcm_probe — ¿el core nos cuenta lo que hace el chip PCM? (#409)
//
// POR QUÉ EXISTE. #409 propone sustituir audio en Sega CD consumiendo el chip
// PCM que el Audio Probe v2 instrumentó (`audio_probe_pcm_key/volume/pitch` en
// core/cd_hw/pcm.c). Antes de escribir ese consumidor hay que contestar dos
// cosas que hoy nadie verificó:
//
//   1. ¿LLEGAN los eventos? El Engine nunca llamó a `poll_audio_events`: todo
//      el audio va por `read_audio_writes_v1`, que transporta escrituras CRUDAS
//      de FM y PSG — el PCM de Sega CD no está ahí. Si el transporte de eventos
//      no funciona, la issue no tiene de dónde leer.
//   2. ¿Con qué TAMAÑO? `ayther_audio_event_v1` pasó sus campos a una unión y
//      cambió de tamaño. Este probe lee con el `event_size` que declara el
//      propio core y NO con el `sizeof` local — que es el gotcha anotado en la
//      cabecera de ayther_api.h y el que haría leer corrido a quien lo ignore.
//      También lo COMPARA, porque una diferencia silenciosa es justo lo que
//      hace que los campos salgan movidos sin que nada falle.
//
// Contestadas las dos (sí llegan; el tamaño coincide), el probe se quedó como
// el oráculo del consumidor que salió de ahí y mide dos cosas más:
//
//   3. ¿SIRVE la identidad? Cuenta los key-on distintos con y sin `st`/`ls`.
//      Si los dos números fueran iguales, agregar esos campos al fork (#25) no
//      habría servido de nada y habría que decirlo. Medido en Earthworm Jim SE:
//      83 identidades sin ellos, 89 con ellos — seis sonidos DISTINTOS que
//      llegaban como el mismo.
//   4. ¿Está CABLEADO? Los tres puntos anteriores miran el core. El último mira
//      la sesión: que `AytherSession` ingiera esos eventos y que el detector
//      abra canales de CHIP_PCM vivos, que es el camino que usa el Lab.
//
// Sin ROM de Sega CD se saltea: un cartucho no tiene este chip.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target segacd_pcm_probe
//   Env:   AYTHER_PROBE_ROM  = la ISO/CUE de Sega CD (obligatoria)
//          AYTHER_SYSTEM_DIR = carpeta con bios_CD_U/E/J.bin (obligatoria acá:
//                              sin BIOS el core no monta el disco)
//          AYTHER_ABI_CORE   = core del fork (default: el del repo)
//          AYTHER_PCM_FRAMES = frames a observar (default 3600)
// ---------------------------------------------------------------------------
#include "libretro_host/retro_runner.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_env.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

std::string env_or(const char* k, const std::string& d) {
    if (const char* v = ayther::env_get(k)) if (*v) return v;
    return d;
}
int env_int(const char* k, int d) {
    if (const char* v = ayther::env_get(k)) if (*v) return std::atoi(v);
    return d;
}

/// RMS de un WAV s16 estéreo escrito por el tee del player (AYTHER_AUDIO_DUMP).
/// Lee el PCM crudo desde el offset 44: los tamaños del header los parchea el
/// shutdown, y acá el archivo puede quedar sin parchear si algo falla antes.
/// Devuelve -1 si no hay archivo o no tiene muestras.
double wav_rms(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1.0;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    if (sz <= 44) { std::fclose(f); return -1.0; }
    std::fseek(f, 44, SEEK_SET);
    const size_t n = size_t(sz - 44) / 2;
    std::vector<int16_t> pcm(n);
    const size_t got = std::fread(pcm.data(), 2, n, f);
    std::fclose(f);
    if (!got) return -1.0;
    double acc = 0.0;
    for (size_t i = 0; i < got; ++i)
        acc += double(pcm[i]) * double(pcm[i]);
    return std::sqrt(acc / double(got));
}

const char* fuente(uint8_t s) {
    switch (s) {
        case AYTHER_AUDIO_SOURCE_FM:  return "FM";
        case AYTHER_AUDIO_SOURCE_PSG: return "PSG";
        case AYTHER_AUDIO_SOURCE_DAC: return "DAC";
        case AYTHER_AUDIO_SOURCE_PCM: return "PCM";
        default: return "?";
    }
}
const char* event_type_name(uint8_t t) {
    switch (t) {
        case AYTHER_AUDIO_EVENT_RAW_WRITE: return "raw";
        case AYTHER_AUDIO_EVENT_NOTE_ON:   return "note_on";
        case AYTHER_AUDIO_EVENT_NOTE_OFF:  return "note_off";
        case AYTHER_AUDIO_EVENT_DAC_START: return "dac_start";
        case AYTHER_AUDIO_EVENT_DAC_STOP:  return "dac_stop";
        case AYTHER_AUDIO_EVENT_PATCH:     return "patch";
        case AYTHER_AUDIO_EVENT_PITCH:     return "pitch";
        case AYTHER_AUDIO_EVENT_VOLUME:    return "volume";
        case AYTHER_AUDIO_EVENT_RESET:     return "reset";
        case AYTHER_AUDIO_EVENT_STATE_LOAD:return "state_load";
        case AYTHER_AUDIO_EVENT_FRAME:     return "frame";
        default: return "?";
    }
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string rom  = env_or("AYTHER_PROBE_ROM", "");
    const std::string core = env_or("AYTHER_ABI_CORE",
        root + "/third_party/cores/genesis_plus_gx_libretro_vram.dll");
    const int frames = env_int("AYTHER_PCM_FRAMES", 3600);

    std::printf("=== segacd_pcm_probe (#409) — ¿llegan los eventos del chip PCM? ===\n");
    if (rom.empty() || !fs::exists(rom) || !fs::exists(core)) {
        std::printf("[skip] falta ROM o core\n"); return 0;
    }
    // Un cartucho no tiene chip PCM: pedirle key-ons sería exigirle hardware que
    // no existe. Se salta ANTES de medir en vez de contarlo como fallo — pero se
    // salta por la ROM, no por el resultado: con una imagen de Sega CD que no
    // produzca eventos, esto tiene que seguir fallando.
    {
        std::string ext = fs::path(rom).extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
        if (ext != ".iso" && ext != ".cue" && ext != ".chd" && ext != ".bin") {
            std::printf("[skip] \"%s\" no es una imagen de Sega CD — este probe "
                        "mide un chip que sólo existe en ese hardware\n", ext.c_str());
            return 0;
        }
    }
    if (env_or("AYTHER_SYSTEM_DIR", "").empty())
        std::printf("[aviso] sin AYTHER_SYSTEM_DIR: el core busca el BIOS junto "
                    "a la ISO y sin BIOS no monta el disco\n");

    RetroRunner r;
    if (!r.init(core, rom)) {
        std::fprintf(stderr, "[FAIL] init — ¿falta bios_CD_U.bin en el system dir?\n");
        return 1;
    }
    if (!r.has_ayther_v1()) { std::printf("[skip] core sin ABI v1\n"); return 0; }
    const ayther_interface_v1* api = r.ayther_api();

    const bool cap = (api->capabilities & AYTHER_CAP_AUDIO_PROBE_V1) != 0;
    std::printf("  capability AUDIO_PROBE_V1=%d · poll_audio_events=%p\n",
                (int)cap, (const void*)api->poll_audio_events);
    check(cap && api->poll_audio_events != nullptr,
          "el core declara el probe de audio y expone poll_audio_events");
    if (!cap || !api->poll_audio_events) { std::printf("\n=== FAIL ===\n"); return 1; }

    ayther_subscription_state_v1 subs{};
    subs.struct_size = sizeof(subs);
    api->get_subscriptions(&subs, sizeof(subs));
    api->set_subscriptions(AYTHER_SUB_ALL & subs.supported_mask);
    check((subs.supported_mask & AYTHER_SUB_AUDIO_EVENTS) != 0,
          "el core soporta la suscripción AUDIO_EVENTS");

    // El TAMAÑO lo declara el core. Leer con el `sizeof` local es el error que
    // la cabecera de ayther_api.h advierte: el struct cambió a una unión.
    ayther_audio_transport_stats_v1 st{};
    st.struct_size = sizeof(st);
    const bool stats_ok = api->get_audio_transport_stats &&
        api->get_audio_transport_stats(&st, sizeof(st)) == AYTHER_STATUS_OK;
    check(stats_ok, "el core reporta las stats del transporte de eventos");
    const uint32_t ev_size = stats_ok && st.event_size ? st.event_size
                                                       : uint32_t(sizeof(ayther_audio_event_v1));
    std::printf("  event_size del core=%u · sizeof local=%zu · capacidad=%u\n",
                ev_size, sizeof(ayther_audio_event_v1), st.capacity);
    check(ev_size == sizeof(ayther_audio_event_v1),
          "el event_size del core == el struct que conoce este Engine");

    // Buffer en BYTES y stride del core: si algún día difieren, esto sigue
    // leyendo alineado en vez de correrse campo a campo.
    constexpr uint32_t kMax = 4096;
    std::vector<uint8_t> buf(size_t(kMax) * ev_size, 0);

    uint64_t por_fuente[8] = {0};
    uint64_t pcm_by_type[16] = {0};
    uint64_t total = 0, frames_with_events = 0, dropped = 0;
    int sample_count = 0;

    // La MEDICIÓN que decide si el cambio del fork valía la pena. Se cuentan
    // las identidades distintas de key-on de dos maneras:
    //   viejo  = (canal, env, fd, pan)  — todo lo que daba el schema 1
    //   nuevo  = (canal, env, fd, pan, st, ls)
    // Si los dos números son iguales, `st`/`ls` no agregan nada en este juego y
    // hay que decirlo en vez de dar por buena la premisa.
    std::set<uint64_t> legacy_ids, new_ids, samples;
    uint64_t foreign_schema_count = 0;

    for (int f = 0; f < frames; ++f) {
        r.run_frame();
        uint32_t n = 0;
        if (api->poll_audio_events(
                reinterpret_cast<ayther_audio_event_v1*>(buf.data()), kMax, &n)
            != AYTHER_STATUS_OK) continue;
        if (n) ++frames_with_events;
        for (uint32_t i = 0; i < n; ++i) {
            ayther_audio_event_v1 e{};
            std::memcpy(&e, buf.data() + size_t(i) * ev_size,
                        (std::min)(size_t(ev_size), sizeof(e)));
            ++total;
            if (e.source < 8) ++por_fuente[e.source];
            if (e.source == AYTHER_AUDIO_SOURCE_PCM) {
            if (e.type < 16) ++pcm_by_type[e.type];
                if (e.type == AYTHER_AUDIO_EVENT_NOTE_ON) {
                    // schema 2: reg = st | ls<<8 · data = fd | env<<16 | pan<<24
                    // Se mira el schema del EVENTO y no se infiere del binario:
                    // con un core schema 1 `reg` viene en cero y todo lo de abajo
                    // saldría plausible pero mal.
            if (e.schema != AYTHER_LAYOUT_AUDIO_EVENT_V1) ++foreign_schema_count;
                    const uint32_t st = e.reg & 0xFF, ls = (e.reg >> 8) & 0xFFFF;
            legacy_ids.insert((uint64_t(e.channel) << 32) | e.data);
            new_ids.insert(((uint64_t(e.channel) << 56) ^
                                     (uint64_t(e.reg) << 24)) | e.data);
                    samples.insert((uint64_t(st) << 16) | ls);
                }
            if (sample_count < 6 && e.type != AYTHER_AUDIO_EVENT_RAW_WRITE) {
                    std::printf("       PCM %-9s ch=%u  t_frame=%u  st=%u ls=0x%04X  "
                                "fd=%u env=%u pan=%u\n",
                        event_type_name(e.type), e.channel, e.t_frame,
                                e.reg & 0xFF, (e.reg >> 8) & 0xFFFF,
                                e.data & 0xFFFF, (e.data >> 16) & 0xFF,
                                (e.data >> 24) & 0xFF);
                ++sample_count;
                }
            }
        }
    }
    if (api->get_audio_transport_stats) {
        ayther_audio_transport_stats_v1 s2{};
        s2.struct_size = sizeof(s2);
        if (api->get_audio_transport_stats(&s2, sizeof(s2)) == AYTHER_STATUS_OK)
            dropped = s2.dropped_events;
    }

    std::printf("\n  %d frames · %llu eventos · %llu frames con eventos · descartados=%llu\n",
                frames, (unsigned long long)total,
                (unsigned long long)frames_with_events, (unsigned long long)dropped);
    for (int s = 0; s < 4; ++s)
        std::printf("     %-3s : %llu\n", fuente(uint8_t(s)),
                    (unsigned long long)por_fuente[s]);

    check(total > 0, "NO-VACUIDAD: el transporte de eventos entrega algo");
    check(por_fuente[AYTHER_AUDIO_SOURCE_PCM] > 0,
          "HALLAZGO CENTRAL DE #409: llegan eventos del chip PCM de Sega CD");
    if (por_fuente[AYTHER_AUDIO_SOURCE_PCM]) {
        std::printf("  PCM por tipo:");
        for (int t = 0; t < 16; ++t)
        if (pcm_by_type[t]) std::printf(" %s=%llu", event_type_name(uint8_t(t)),
                                        (unsigned long long)pcm_by_type[t]);
        std::printf("\n");
    check(pcm_by_type[AYTHER_AUDIO_EVENT_NOTE_ON] > 0,
              "…y entre ellos hay key-ons (lo que una sustitución necesita anclar)");

        // ¿Sirvió agregar st/ls al key-on (fork #25)?
        std::printf("\n  IDENTIDAD de los key-on PCM:\n"
                    "     con env/fd/pan solamente (schema 1) : %zu distintas\n"
                    "     agregando st/ls        (schema 2) : %zu distintas\n"
                    "     samples distintos (st,ls)          : %zu\n",
                legacy_ids.size(), new_ids.size(), samples.size());
    check(!new_ids.empty(), "NO-VACUIDAD: hubo key-ons que identificar");
    check(foreign_schema_count == 0,
              "todos los key-on PCM declaran el schema que este Engine sabe leer");
    check(new_ids.size() >= legacy_ids.size(),
              "agregar st/ls no puede FUSIONAR identidades (sólo separar)");
        // Que HAYA colisiones que separar depende de cuánto material se oyó: en
        // un tramo corto el juego puede tocar un solo sample y entonces no hay
        // nada que st/ls pueda distinguir. Eso no es un fallo — pero tampoco un
        // éxito, así que se dice «sin datos» en vez de contar un OK vacuo.
        if (frames < 3000) {
            std::printf("  [ -- ] con %d frames no hay corpus para la premisa de "
                        "#409 (necesita >= 3000; con 3600 da 83 -> 89)\n", frames);
        } else {
    check(new_ids.size() > legacy_ids.size(),
                  "PREMISA DE #409: sin st/ls, sonidos DISTINTOS llegaban como el mismo");
            check(samples.size() > 1,
                  "el juego toca más de un sample por el chip PCM");
        }
    }
    check(dropped == 0, "el transporte no descartó eventos (si descarta, falta polleo)");

    r.shutdown();

    // -----------------------------------------------------------------------
    // ¿EL MUTE CALLA DE VERDAD? Que la máscara tenga bits para el PCM y que el
    // core los lea no prueba que el sonido baje: hay que medir el PCM de salida.
    //
    // Tres corridas del MISMO tramo, siempre desde cero:
    //   A1  sin mute            -> energía de referencia
    //   A2  sin mute (control)  -> tiene que dar IDÉNTICO a A1, o la medición no
    //                              distingue un mute de la varianza del emulador
    //   B   con los 8 canales de PCM muteados -> tiene que BAJAR
    //
    // El control A-vs-A no es ceremonia: sin él, un run que difiera por deriva
    // del audio pasaría por "el mute funciona".
    // -----------------------------------------------------------------------
    {
        std::printf("\n  --- ¿el mute del chip PCM baja el sonido? ---\n");
    auto energy = [&](uint32_t mute) -> double {
            RetroRunner rr;
            if (!rr.init(core, rom)) return -1.0;
            // SIN ESTO EL MUTE NO EXISTE: el core lo gatea con
            // AYTHER_SUB_RENDER_CONTROLS y un runner recién creado no está
            // suscripto a nada. Es el mismo gate que protege la supresión de
            // capas — un frontend que no pidió los controles no los recibe.
            if (const ayther_interface_v1* a = rr.ayther_api()) {
                ayther_subscription_state_v1 s2{};
                s2.struct_size = sizeof(s2);
                a->get_subscriptions(&s2, sizeof(s2));
                a->set_subscriptions(AYTHER_SUB_ALL & s2.supported_mask);
            }
        double sum = 0.0; uint64_t n = 0;
            rr.set_audio_callback([&](const int16_t* s, size_t frames) -> size_t {
            for (size_t i = 0; i < frames * 2; ++i) sum += double(s[i]) * s[i];
                n += frames * 2;
                return frames;
            });
            for (int f = 0; f < frames; ++f) {
                // El mute se re-pide cada frame: es una escritura de control con
                // generación, y un reset del core la descartaría en silencio.
                if (mute) rr.set_audio_mute_v1(mute);
                rr.run_frame();
            }
            rr.shutdown();
        return n ? std::sqrt(sum / double(n)) : 0.0;
        };
    uint32_t all_pcm_channels = 0;
    for (int ch = 0; ch < 8; ++ch) all_pcm_channels |= RetroRunner::audio_mute_pcm(ch);
    std::printf("     máscara de los 8 canales PCM = 0x%05X\n", all_pcm_channels);
    check(all_pcm_channels == 0x3FC00u,
              "los 8 canales del PCM caen en los bits 10-17 y en ninguno más");

    const double a1 = energy(0);
    const double a2 = energy(0);
    const double b  = energy(all_pcm_channels);
        std::printf("     rms  A1=%.1f  A2=%.1f (control)  B=%.1f (PCM mudo)\n", a1, a2, b);
        check(a1 > 0.0, "NO-VACUIDAD: sin mute hay sonido que medir");
        check(a1 == a2, "CONTROL A-vs-A: dos corridas iguales dan lo MISMO");
        check(b < a1, "mutear los canales de PCM BAJA la energía de salida");
        if (a1 > 0.0)
            std::printf("     el PCM aportaba el %.1f%% de la energía del tramo\n",
                        100.0 * (a1 - b) / a1);

        // Y que no se lleve puesto lo que no le toca: con los bits del PCM
        // puestos, el FM y el PSG tienen que seguir sonando igual que siempre.
        check(b > 0.0, "…pero el FM y el PSG siguen sonando (no es un mute global)");
    }


    // -----------------------------------------------------------------------
    // El CABLEADO, no sólo el transporte. Todo lo de arriba mide lo que el core
    // entrega; esto mide que la SESIÓN lo consuma y que el detector produzca
    // canales de CHIP_PCM vivos — que es el camino que usa el Lab. Verificarlo
    // por el runner y no por la sesión dejaría sin cubrir justo donde se rompe.
    // -----------------------------------------------------------------------
    {
        std::printf("\n  --- por AytherSession (el camino del Lab) ---\n");
        ayther::AytherSession::Config c;
        c.core_path = core;
        c.rom_path  = rom;
        c.enable_audio = false;
        auto sr = ayther::AytherSession::create(c);
        if (!sr) {
            std::fprintf(stderr, "[FAIL] no se pudo crear la sesión: %s\n",
                         sr.error.message.c_str());
            ++g_checks; ++g_fails;
        } else {
            std::unique_ptr<ayther::AytherSession>& s = *sr;
            uint32_t frames_with_pcm = 0, peak_active = 0;
            std::set<uint64_t> sigs, insts;
            for (int f = 0; f < frames; ++f) {
                s->step();
                AytherAudioActive act[64];
                const uint32_t n = s->audio_live_active(act, 64);
                uint32_t active_count = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    if (act[i].chip != 3) continue;   // CHIP_PCM
                    ++active_count;
                    sigs.insert(act[i].signature);
                    insts.insert(act[i].instrument);
                }
                if (active_count) {
                    ++frames_with_pcm;
                    if (active_count > peak_active) peak_active = active_count;
                }
            }
            std::printf("     frames con PCM vivo=%u · pico simultáneo=%u · "
                        "firmas=%zu · instrumentos=%zu\n",
                        frames_with_pcm, peak_active, sigs.size(), insts.size());
            check(frames_with_pcm > 0,
                  "CABLEADO: la sesión ingiere los eventos y el detector abre canales PCM");
            check(!insts.empty() && *insts.begin() != 0,
                  "…con identidad de instrumento calculada (no cero)");
            check(sigs.size() >= insts.size(),
                  "hay al menos tantas firmas como instrumentos (el mismo sample a otra "
                  "velocidad comparte instrumento)");
            check(peak_active <= 8, "no aparecen más canales de los que el chip tiene");

            // -------------------------------------------------------------
            // EL CAMINO DE AUTORÍA, que es distinto del de vivo. El usuario no
            // asigna sobre lo que suena ahora: graba una toma, la ANALIZA y
            // asigna sobre los eventos cerrados que salen de ahí. Ese análisis
            // re-emula la toma y hasta #409 le pasaba al detector sólo las
            // escrituras de FM/PSG, así que el chip PCM se podía sustituir en
            // vivo pero no autorar — que es como se trabaja.
            // -------------------------------------------------------------
            std::printf("\n  --- por el ANÁLISIS de una toma (el camino de autoría) ---\n");
            s->record_start();
            for (int f = 0; f < 600; ++f) s->step();
            s->record_stop();
            ayther::AytherRecording rec = s->take_recording();
            const uint32_t nev2 = s->analyze_audio_events(rec);
            const AytherAudioEvent* evs = s->audio_events();
            uint32_t pcm_events = 0, with_pitch = 0, with_velocity = 0;
            std::set<uint64_t> analysis_instruments;
            for (uint32_t i = 0; i < nev2 && evs; ++i) {
                if (evs[i].chip != 3) continue;
                ++pcm_events;
                analysis_instruments.insert(evs[i].instrument);
                if (evs[i].pitch <= 127) ++with_pitch;
                if (evs[i].velocity > 0) ++with_velocity;
            }
            std::printf("     toma de %u frames · %u eventos · %u de PCM · "
                        "%zu instrumentos · %u con nota · %u con velocity\n",
                        (unsigned)rec.frame_count(), nev2, pcm_events,
                        analysis_instruments.size(), with_pitch, with_velocity);
            check(nev2 > 0, "NO-VACUIDAD: el análisis produce eventos");
            check(pcm_events > 0,
                  "EL ANÁLISIS VE EL CHIP PCM (sin esto no se puede autorar)");
            check(!analysis_instruments.empty(),
                  "…con identidad de instrumento, que es lo que se asigna");
            check(with_pitch > 0,
                  "…y con NOTA derivada de la velocidad del sample");

            // La misma toma, a disco, para poder MIRARLA en el Lab: no hay ROM
            // de Sega CD en el corpus ni proyecto con tomas, y grabar una a mano
            // pide manejar la interfaz. Con esto, `load_take` del MCP abre esta
            // toma en Mezclar y las lanes del chip se ven de verdad.
            if (const std::string out = env_or("AYTHER_PROBE_SAVE_TAKE", ""); !out.empty()) {
                const bool ok = rec.save(out);
                std::printf("     toma guardada en %s: %s\n", out.c_str(), ok ? "sí" : "NO");
            }
        }
    }

    // -----------------------------------------------------------------------
    // LA REPRODUCCIÓN. Todo lo anterior mira el DETECTOR; esto mira lo que sale
    // por el device, que es lo único que el usuario oye.
    //
    // Hasta 2026-08-13, con el router de voces puesto —el default— su bloque
    // OCUPABA EL LUGAR del PCM del core en el staging del player. En cartucho no
    // se pierde nada (el router espeja los diez canales), pero en Sega CD ese
    // buffer es el ÚNICO portador del chip PCM y del CDDA: el sistema salía
    // MUDO. Medido en Earthworm Jim SE: 127 s de silencio absoluto contra
    // -25,0 dBFS con el router apagado. Ningún check lo veía porque todos
    // miraban el core, que producía el audio perfectamente bien.
    //
    // El device es el dummy de SDL —no hay que oír nada, hay que MEDIR— y lo
    // que se mide es el tee del player, que escribe la mezcla final encolada.
    // -----------------------------------------------------------------------
    {
        std::printf("\n  --- ¿SUENA? (la mezcla que se le encola al device) ---\n");
        const auto tmp = std::filesystem::temp_directory_path();
        const std::string wav_on  = (tmp / "segacd_probe_router_on.wav").string();
        const std::string wav_off = (tmp / "segacd_probe_router_off.wav").string();
        auto setenv_ = [](const char* k, const char* v) {
#ifdef _WIN32
            _putenv_s(k, v);
#else
            setenv(k, v, 1);
#endif
        };
        setenv_("SDL_AUDIODRIVER", "dummy");
        auto medir = [&](const char* dump, bool router) -> double {
            std::filesystem::remove(dump);
            setenv_("AYTHER_AUDIO_DUMP", dump);
            ayther::AytherSession::Config c;
            c.core_path = core;
            c.rom_path  = rom;
            c.enable_audio = true;
            auto sr = ayther::AytherSession::create(c);
            if (!sr) return -1.0;
            {
                std::unique_ptr<ayther::AytherSession>& s = *sr;
                // Explícito en las dos ramas: el env AYTHER_VOICE_ROUTER del
                // entorno no debe decidir qué mide este control.
                s->set_voice_router(router);
                for (int f = 0; f < frames; ++f) s->step();
            }   // el destructor cierra el tee (parchea los tamaños del WAV)
            setenv_("AYTHER_AUDIO_DUMP", "");
            return wav_rms(dump);
        };
        const double r_off = medir(wav_off.c_str(), false);
        const double r_on  = medir(wav_on.c_str(),  true);
        std::printf("     rms de la salida:  router APAGADO=%.1f   router PUESTO=%.1f\n",
                    r_off, r_on);
        check(r_off > 0.0,
              "NO-VACUIDAD: con el router apagado el juego suena (hay algo que perder)");
        check(r_on > 0.0,
              "CON EL ROUTER PUESTO —el default— SEGA CD SUENA (#409: salía mudo)");
        // El router aporta su propia síntesis de FM/PSG, así que los dos números
        // no tienen por qué coincidir; lo que no puede pasar es que el camino
        // por defecto pierda el grueso del audio del hardware.
        check(r_off <= 0.0 || r_on >= r_off * 0.5,
              "…y con el nivel del hardware, no un resto (≥ la mitad del control)");
    }

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

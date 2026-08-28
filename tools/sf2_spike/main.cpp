// ---------------------------------------------------------------------------
// sf2_spike — ¿se puede recuperar la PARTITURA que el juego ejecuta, para
// re-sintetizarla con un SoundFont?
//
// La idea: la sustitución de audio que ya existe reemplaza una Secuencia entera
// por una grabación — hay que rearreglar y regrabar afuera, y sólo calza si el
// timing coincide. Un SoundFont es OTRO eje, complementario: el juego SIGUE
// tocando (su tempo, sus jingles, sus cortes) y sólo se cambia el TIMBRE de una
// voz. No se puede desincronizar, porque la fuente de tiempo es el juego.
//
// Este spike NO integra ningún sintetizador. Responde la única pregunta que
// decide si vale la pena hacerlo: **¿las notas salen bien?**
//
// Decodifica las escrituras crudas al bus de los chips —que el fork ya expone
// y que son REPLAY-ESTABLES como secuencia de comandos— a eventos de nota:
//   · YM2612: key on/off (reg 0x28), F-Number + Block (0xA0-0xA6) → Hz → nota
//     MIDI. La «velocidad» sale del Total Level del operador portador, porque
//     FM no tiene velocity.
//   · SN76489: latch/data de tono y atenuación → nota y volumen.
//   · Canal 6 en modo DAC: se CUENTA aparte. Ahí no hay notas — son samples
//     PCM (batería, voces) y el SoundFont no aplica; eso sigue por el camino de
//     sustitución de samples que ya existe. Medir cuánto pesa es parte de la
//     respuesta.
//
// Oráculos, en orden de importancia:
//   1. DETERMINISMO — decodificar la misma toma dos veces da la MISMA lista de
//      eventos. Es lo que hace posible que un render con SF2 sea reproducible,
//      y no es gratis: el motor advierte que el `cycle` de la escritura NO es
//      replay-estable, así que la decodificación no puede depender de él.
//   2. MUSICALIDAD — las notas caen en rango audible, los note-on cierran, y la
//      densidad es de música y no de ruido de registros.
//   3. IDENTIDAD DEL INSTRUMENTO — la firma del patch FM de un canal (los
//      registros de operador + algoritmo) se mantiene mientras suena una nota y
//      cambia entre instrumentos. Es lo que permitiría mapear patch → preset
//      del SoundFont, con la misma filosofía de identidad por contenido que el
//      hash de sprite.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target sf2_spike
//   Run:    bin/sf2_spike [core.dll rom]
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;

namespace {

constexpr uint8_t kChipFM  = 0;
constexpr uint8_t kChipPSG = 1;

// Relojes NTSC del Mega Drive.
constexpr double kFmClock  = 7670453.0 / 144.0;   // ~53267 Hz (fase del OPN2)
constexpr double kPsgClock = 3579545.0;

/// Un evento de nota recuperado del bus. `frame` es la unidad temporal: el
/// `cycle` de la escritura NO es replay-estable, y además los drivers de sonido
/// del Mega Drive corren su secuenciador en el V-blank, así que el frame ES la
/// resolución nativa de esta música.
struct NoteEvent {
    uint32_t frame;
    uint8_t  chip;      ///< 0 = FM · 1 = PSG
    uint8_t  channel;
    uint8_t  on;        ///< 1 = note on · 0 = note off
    uint8_t  note;      ///< nota MIDI (0 en un off)
    uint8_t  velocity;  ///< derivada del Total Level / atenuación
    uint64_t patch;     ///< firma del timbre del canal al disparar (0 en un off)
    uint64_t patch_legacy; ///< la firma VIEJA (con TL de portador) — sólo para la medición de #261
};

/// Convierte Hz a nota MIDI. Fuera de rango audible devuelve 0.
uint8_t midi_of(double hz) {
    if (hz < 16.0 || hz > 12600.0) return 0;
    const double n = 69.0 + 12.0 * std::log2(hz / 440.0);
    if (n < 1.0 || n > 127.0) return 0;
    return static_cast<uint8_t>(n + 0.5);
}

/// Estado sombra de los chips: replica el latcheo del bus igual que el
/// detector de eventos del core (core/src/audio_event.rs, apply_fm).
struct ChipState {
    uint8_t  fm[0x200] = {};
    uint16_t fm_addr   = 0;
    bool     dac_on    = false;
    uint32_t dac_writes = 0;
    uint8_t  key[6]    = {};      ///< máscara de operadores activa por canal

    // PSG: 4 canales (3 de tono + ruido).
    uint16_t psg_tone[4] = {};
    uint8_t  psg_att[4]  = { 15, 15, 15, 15 };   // 15 = silencio
    uint8_t  psg_latch   = 0;

    /// La firma ANTERIOR, que mezclaba el TL de los cuatro operadores. Se
    /// conserva SÓLO para la medición que justificó cambiarla (ver la sección
    /// «IDENTIDAD vs VOLUMEN»): sobre 60 s daba 43 timbres contra 30, y en FM0
    /// partía un solo instrumento en dieciséis. No usar para nada más.
    uint64_t fm_patch_legacy_tl(int ch) const {
        const int bank = ch < 3 ? 0 : 0x100;
        const int idx  = ch % 3;
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint8_t v) { h ^= v; h *= 1099511628211ull; };
        for (int op = 0; op < 4; ++op) {
            const int base = bank + 0x30 + op * 4 + idx;
            for (int r = 0; r < 7; ++r) mix(fm[(base + r * 0x10) & 0x1FF]);
        }
        mix(fm[bank + 0xB0 + idx]);      // algoritmo + feedback
        return h ? h : 1;
    }

    /// Firma del TIMBRE del canal FM: los cuatro operadores (DT/MUL, TL, KS/AR,
    /// AM/DR, SR, SL/RR) más algoritmo y feedback, PERO sin el Total Level de
    /// los operadores PORTADORES. Es la identidad del «Instrumento» — misma
    /// filosofía que el hash de un sprite: el contenido, no la presentación.
    ///
    /// EL TL DEL PORTADOR ES EL VOLUMEN, NO EL TIMBRE. (El de un modulador sí
    /// es timbre: es el índice de modulación, y se queda.) Medido sobre 60 s de
    /// Sonic: incluirlo daba 43 identidades donde hay 30, y en FM0 fragmentaba
    /// UN instrumento en 16 — o sea que el artista habría tenido que re-asignar
    /// el mismo timbre cada vez que el juego baja el volumen.
    ///
    /// Mismo defecto que tuvo el hash de sprite antes de hacerse invariante al
    /// flip, y por eso se corrige ANTES de que exista autoría: cambiar la forma
    /// de la firma después obliga a re-autorar todo lo asignado.
    ///
    /// El TL del portador no se pierde: sale por `fm_velocity`, que es donde
    /// corresponde que esté.
    uint64_t fm_patch(int ch) const {
        const int bank = ch < 3 ? 0 : 0x100;
        const int idx  = ch % 3;
        const uint8_t alg = fm[bank + 0xB0 + idx] & 7;
        static const uint8_t kCarriers[8] = { 0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF };
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint8_t v) { h ^= v; h *= 1099511628211ull; };
        for (int op = 0; op < 4; ++op) {
            const bool carrier = (kCarriers[alg] & (1u << op)) != 0;
            const int base = bank + 0x30 + op * 4 + idx;
            for (int r = 0; r < 7; ++r) {
                // r == 1 es el registro 0x40 = Total Level.
                if (r == 1 && carrier) continue;
                mix(fm[(base + r * 0x10) & 0x1FF]);
            }
        }
        mix(fm[bank + 0xB0 + idx]);
        return h ? h : 1;
    }

    /// Nivel del canal: el Total Level del operador PORTADOR según el
    /// algoritmo. En FM no hay velocity — esto es lo más parecido.
    uint8_t fm_velocity(int ch) const {
        const int bank = ch < 3 ? 0 : 0x100;
        const int idx  = ch % 3;
        const uint8_t alg = fm[bank + 0xB0 + idx] & 7;
        // Operadores portadores por algoritmo (bitmask sobre op 0..3).
        static const uint8_t kCarriers[8] = { 0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF };
        uint8_t best = 127;
        for (int op = 0; op < 4; ++op) {
            if (!(kCarriers[alg] & (1u << op))) continue;
            const uint8_t tl = fm[(bank + 0x40 + op * 4 + idx) & 0x1FF] & 0x7F;
            if (tl < best) best = tl;      // menor TL = más fuerte
        }
        // TL 0 = máximo, 127 = silencio → invertir a velocity MIDI.
        const int v = 127 - static_cast<int>(best);
        return static_cast<uint8_t>(v < 1 ? 1 : v);
    }

    double fm_hz(int ch) const {
        const int bank = ch < 3 ? 0 : 0x100;
        const int idx  = ch % 3;
        const uint8_t hi = fm[bank + 0xA4 + idx];
        const uint8_t lo = fm[bank + 0xA0 + idx];
        const uint32_t fnum  = ((hi & 0x07u) << 8) | lo;
        const uint32_t block = (hi >> 3) & 0x07u;
        if (!fnum) return 0.0;
        // f = fnum * clock / 2^(20 - block)   (fórmula estándar del OPN2)
        return fnum * kFmClock * std::pow(2.0, static_cast<double>(block)) /
               (2.0 * 1048576.0);
    }
};

}  // namespace

static std::string cfg_quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve_path(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

/// Decodifica una toma entera a eventos de nota. Sin estado previo: se arranca
/// de cero para que dos corridas sean comparables byte a byte.
/// Frames en los que `replay_seek` no devolvió vista. Se cuenta en vez de
/// tragarse el `continue`: si TODOS fallan, el spike mide cero y sin esto
/// parecería que el core no loguea (ya me pasó una vez).
static uint32_t g_null_views = 0;

static std::vector<NoteEvent> decode(ayther::AytherSession& s,
                                     const ayther::AytherRecording& take,
                                     uint32_t* out_writes, uint32_t* out_dac) {
    std::vector<NoteEvent> ev;
    ChipState st;
    uint32_t writes = 0;
    const uint32_t N = take.frame_count();
    for (uint32_t f = 0; f < N; ++f) {
        const FrameView* fv = s.replay_seek(take, f);
        if (!fv) { ++g_null_views; continue; }
        for (uint32_t i = 0; i < fv->chip_write_count; ++i) {
            const auto& w = fv->chip_writes[i];
            ++writes;
            if (w.chip == kChipFM) {
                // Latcheo del bus: addr&3 → 0/2 puerto de dirección (banco 0/1),
                // 1/3 puerto de dato. Igual que el detector del core.
                switch (w.addr & 3u) {
                    case 0: st.fm_addr = w.data; break;
                    case 2: st.fm_addr = static_cast<uint16_t>(w.data) | 0x100u; break;
                    default: {
                        const uint16_t reg = st.fm_addr & 0x1FFu;
                        st.fm[reg] = w.data;
                        if (reg == 0x2B) st.dac_on = (w.data & 0x80u) != 0;
                        else if (reg == 0x2A) ++st.dac_writes;
                        else if (reg == 0x28) {
                            // data: bits 0-1 canal del banco, bit 2 banco,
                            // bits 4-7 máscara de operadores (≠0 = key on).
                            const uint8_t sel = w.data & 0x07u;
                            if ((sel & 3u) == 3u) break;     // combinación inválida
                            const int ch = (sel & 3u) + ((sel & 4u) ? 3 : 0);
                            const uint8_t ops = (w.data >> 4) & 0x0Fu;
                            const bool was = st.key[ch] != 0;
                            st.key[ch] = ops;
                            if (ops && !was) {
                                // El canal 6 en modo DAC no toca notas: es PCM.
                                if (ch == 5 && st.dac_on) break;
                                const uint8_t note = midi_of(st.fm_hz(ch));
                                if (note)
                                    ev.push_back({ f, kChipFM, (uint8_t)ch, 1, note,
                                                   st.fm_velocity(ch), st.fm_patch(ch),
                                                   st.fm_patch_legacy_tl(ch) });
                            } else if (!ops && was) {
                                ev.push_back({ f, kChipFM, (uint8_t)ch, 0, 0, 0, 0, 0 });
                            }
                        }
                        break;
                    }
                }
            } else if (w.chip == kChipPSG) {
                const uint8_t d = w.data;
                if (d & 0x80u) {                       // byte de LATCH
                    st.psg_latch = (d >> 4) & 0x07u;   // canal<<1 | tipo
                    const int ch  = (st.psg_latch >> 1) & 3;
                    const bool att = (st.psg_latch & 1) != 0;
                    if (att) {
                        const uint8_t a = d & 0x0Fu;
                        const bool was = st.psg_att[ch] < 15;
                        st.psg_att[ch] = a;
                        if (a < 15 && !was) {
                            const double hz = (ch < 3 && st.psg_tone[ch])
                                ? kPsgClock / (32.0 * st.psg_tone[ch]) : 0.0;
                            const uint8_t note = midi_of(hz);
                            if (note)
                                ev.push_back({ f, kChipPSG, (uint8_t)ch, 1, note,
                                               (uint8_t)(127 - a * 8), 0, 0 });
                        } else if (a >= 15 && was) {
                            ev.push_back({ f, kChipPSG, (uint8_t)ch, 0, 0, 0, 0, 0 });
                        }
                    } else if (ch < 3) {
                        st.psg_tone[ch] = (st.psg_tone[ch] & 0x3F0u) | (d & 0x0Fu);
                    }
                } else {                                // byte de DATO (6 bits altos)
                    const int ch = (st.psg_latch >> 1) & 3;
                    if (!(st.psg_latch & 1) && ch < 3)
                        st.psg_tone[ch] = (uint16_t)((st.psg_tone[ch] & 0x0Fu)
                                                     | ((d & 0x3Fu) << 4));
                }
            }
        }
    }
    if (out_writes) *out_writes = writes;
    if (out_dac)    *out_dac    = st.dac_writes;
    return ev;
}

int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    if (!cfg) { std::fprintf(stderr, "[skip] sin tests/test_config.toml\n"); return 0; }
    std::string core, line; std::vector<std::string> roms; bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) in_rom = true;
        else if (line.find("core") != std::string::npos
                 && line.find('=') != std::string::npos && core.empty())
            core = cfg_quoted(line);
        else if (in_rom && line.find("path") != std::string::npos)
            roms.push_back(cfg_quoted(line));
    }
    core = resolve_path(core, root);
    for (auto& p : roms) p = resolve_path(p, root);
    if (argc >= 3) { core = argv[1]; roms.assign(1, argv[2]); }
    if (core.empty() || roms.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }
    std::printf("=== sf2_spike — partitura desde el bus de los chips ===\ncore: %s\n\n",
                core.c_str());

    // Buscar una ROM que TOQUE, en vez de asumir la primera del config. El
    // warm-up a ciegas depende del juego: en Aladdin no llega a la música ni en
    // 5000 frames de START (el bus queda en el goteo del driver en reposo), y
    // medir cero ahí es indistinguible de un core sin instrumentar si no se
    // mira. El spike necesita música sonando; que la busque él.
    constexpr uint16_t START = 1u << 3;
    std::unique_ptr<ayther::AytherSession> s;
    std::string rom;
    for (const std::string& cand : roms) {
        ayther::AytherSession::Config c;
        c.core_path = core; c.rom_path = cand; c.enable_audio = false;
        auto r = ayther::AytherSession::create(c);
        if (!r) { std::fprintf(stderr, "[..] %s: create falló\n", cand.c_str()); continue; }
        std::unique_ptr<ayther::AytherSession> cs = std::move(*r);
        // Medir por BLOQUE, no por racha de frames consecutivos: el driver
        // escribe a ráfagas (Sonic promedia ~280 escrituras/frame pero deja
        // frames en cero), así que exigir una racha rechaza música real.
        constexpr int kBlock = 120;               // 2 s
        constexpr uint32_t kBusy = kBlock * 20;   // densidad de música, no de goteo
        int warm = 0; bool music = false;
        while (warm < 3000 && !music) {
            uint32_t sum = 0;
            for (int i = 0; i < kBlock; ++i, ++warm) {
                cs->set_input(0, (warm % 32 == 0) ? START : 0);
                sum += cs->step().chip_write_count;
            }
            music = sum >= kBusy;
        }
        if (music) {
            std::printf("[ok] %s: música tras %d frames\n", cand.c_str(), warm);
            s = std::move(cs); rom = cand; break;
        }
        std::printf("[..] %s: sin música tras %d frames — siguiente\n", cand.c_str(), warm);
    }
    if (!s) {
        std::fprintf(stderr,
            "\n[skip] ninguna ROM del config llegó a tocar música con un warm-up a\n"
            "       ciegas. Pasá una ROM y un estado con música: sf2_spike <core> <rom>.\n");
        return 0;
    }
    // Toma larga a propósito (#261): con 600 frames (10 s) la música no llega a
    // tener dinámica, y la pregunta de si el VOLUMEN contamina la identidad del
    // timbre sólo se puede responder sobre un tramo donde el volumen cambie.
    // Se puede acortar con el 3er argumento cuando sólo interesan los oráculos
    // de partitura, que con 600 ya pasan.
    const int rec_frames = (argc > 3) ? std::atoi(argv[3]) : 3600;   // 60 s
    s->record_start();
    for (int i = 0; i < rec_frames; ++i) { s->set_input(0, 0); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[ok] toma: %u frames\n", take.frame_count());

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail;
    };

    uint32_t writes = 0, dac = 0;
    std::vector<NoteEvent> ev = decode(*s, take, &writes, &dac);
    std::printf("[..] %u escrituras al bus · %u al DAC · %zu eventos de nota\n",
                writes, dac, ev.size());

    // Degradación limpia, como el resto de los tools: sin el log de escrituras
    // no hay NADA que decodificar y el spike no probaría nada. Se salta con un
    // mensaje fuerte en vez de fallar — el canal 0x109 está expuesto por el
    // core (el puntero no es nulo) pero el contador no se mueve, así que la
    // instrumentación C-A1 no está viva en este build del fork.
    if (writes == 0 && g_null_views > 0) {
        std::fprintf(stderr,
            "\n[FAIL] replay_seek no devolvió vista en %u de %u frames — el spike\n"
            "       no llegó a medir nada. No es el core: audio_chip_spike sí ve\n"
            "       escrituras con este mismo DLL.\n", g_null_views, take.frame_count());
        return 1;
    }
    if (writes == 0) {
        // NO es «el core no loguea» — con un core stock el puntero de 0x109 sale
        // nulo y eso se detecta antes. Cero escrituras con el fork significa que
        // el warm-up dejó al juego en una pantalla donde el driver de sonido no
        // está tocando (me pasó con Aladdin: 900 frames de START y quedó mudo).
        std::fprintf(stderr,
            "\n[skip] la toma no tiene NINGUNA escritura a los chips: el warm-up\n"
            "       dejó al juego en una pantalla muda. Probá otra ROM u otro\n"
            "       warm-up — el spike necesita música sonando para decodificarla.\n"
            "       (Con un core stock el fallo sería otro y sale más arriba.)\n");
        return 0;
    }
    check(!ev.empty(), "se recupera una PARTITURA del bus");
    if (ev.empty()) { std::printf("\nFALLÓ\n"); return 1; }

    // ── 1. DETERMINISMO ────────────────────────────────────────────────────
    // Es el chequeo que decide si un render con SF2 puede ser reproducible. No
    // es gratis: el motor advierte que el `cycle` de la escritura NO es
    // replay-estable, así que la decodificación no lo usa — el tiempo va en
    // frames, que además es la resolución nativa de esta música (los drivers
    // del Mega Drive corren su secuenciador en el V-blank).
    {
        std::vector<NoteEvent> again = decode(*s, take, nullptr, nullptr);
        bool same = again.size() == ev.size();
        for (size_t i = 0; same && i < ev.size(); ++i)
            same = std::memcmp(&ev[i], &again[i], sizeof(NoteEvent)) == 0;
        check(same, "decodificar dos veces da la MISMA partitura (replay-estable)");
    }

    // ── 2. MUSICALIDAD ─────────────────────────────────────────────────────
    {
        std::map<int, int> per_ch;
        int ons = 0, offs = 0, lo = 127, hi = 0;
        for (const NoteEvent& e : ev) {
            if (e.on) {
                ++ons; ++per_ch[e.chip * 16 + e.channel];
                if (e.note < lo) lo = e.note;
                if (e.note > hi) hi = e.note;
            } else ++offs;
        }
        std::printf("[..] %d note-on · %d note-off · rango MIDI %d-%d · %zu canales activos\n",
                    ons, offs, lo, hi, per_ch.size());
        std::printf("[..] por canal:");
        for (const auto& [k, n] : per_ch)
            std::printf(" %s%d(%d)", (k / 16) ? "PSG" : "FM", k % 16, n);
        std::printf("\n");

        check(ons > 20, "hay una cantidad musical de notas");
        check(per_ch.size() >= 2, "suena más de un canal (no es un pitido suelto)");
        check(lo >= 21 && hi <= 108,
              "las notas caen en el rango de un piano (la fórmula de fnum/block es correcta)");
        // Los off HUÉRFANOS son normales y no prueban nada: un driver de FM manda
        // key-off incondicional antes de cada nota para re-disparar, y la toma
        // empieza con notas ya sonando. Lo que sí decide si esto sirve para
        // resintetizar es la DURACIÓN: el SoundFont necesita saber cuánto dura
        // cada nota para correr su envolvente. Una duración absurda (una nota
        // abierta toda la toma) sería un error de decodificación disfrazado.
        std::map<int, uint32_t> open;        // canal → frame del note-on abierto
        std::vector<uint32_t> dur;
        for (const NoteEvent& e : ev) {
            const int k = e.chip * 16 + e.channel;
            if (e.on) open[k] = e.frame;
            else if (auto it = open.find(k); it != open.end()) {
                dur.push_back(e.frame - it->second);
                open.erase(it);
            }
        }
        std::printf("[..] %d off huérfanos (re-disparo del driver + notas ya sonando: normal)\n",
                    offs - static_cast<int>(dur.size()));
        if (!dur.empty()) {
            std::sort(dur.begin(), dur.end());
            const uint32_t med = dur[dur.size() / 2], mx = dur.back();
            std::printf("[..] duración de nota: mediana %u frames · máxima %u\n", med, mx);
            check(med >= 1 && med <= 240,
                  "las notas duran lo que dura una nota (mediana < 4 s)");
            check(mx < take.frame_count(),
                  "ninguna nota queda abierta toda la toma");
        }
        check(!dur.empty(), "hay notas que abren y cierran (pares on/off emparejados)");
        const double per_sec = double(ons) / (take.frame_count() / 60.0);
        std::printf("[..] densidad: %.1f notas por segundo\n", per_sec);
        check(per_sec > 0.5 && per_sec < 60.0,
              "la densidad es de música, no de ruido de registros");
    }

    // ── 3. IDENTIDAD DEL INSTRUMENTO ───────────────────────────────────────
    // La firma del patch es lo que permitiría mapear timbre → preset del SF2.
    // Tiene que ser ESTABLE (un canal no cambia de instrumento en cada nota) y
    // DISCRIMINANTE (canales distintos usan patches distintos).
    {
        std::map<int, std::map<uint64_t, int>> patches;   // canal → patch → notas
        for (const NoteEvent& e : ev)
            if (e.on && e.chip == kChipFM && e.patch) ++patches[e.channel][e.patch];

        // Lo que hay que probar es que la firma es ESTABLE: que identifica un
        // timbre y no cambia con cada nota. Antes se medía como «un patch
        // domina el canal» (≥50% de las notas), pero eso asumía una ventana de
        // 10 s; en 60 s un canal cambia de instrumento entre secciones y la
        // afirmación deja de ser cierta sin que nada esté mal.
        //
        // La medida honesta es la RAZÓN notas/timbre: si la firma fuera ruido,
        // daría ~1. El umbral de 4 es holgado a propósito — separa «identifica
        // algo» de «cambia todo el tiempo», que es lo único que este oráculo
        // tiene que decidir. Cuál firma es MEJOR lo decide la sección de abajo.
        size_t distinct = 0;
        double worst_ratio = 1e9;
        int worst_ch = -1;
        for (const auto& [ch, m] : patches) {
            distinct += m.size();
            int tot = 0;
            for (const auto& [p, n] : m) tot += n;
            const double ratio = m.empty() ? 0.0 : double(tot) / double(m.size());
            if (ratio < worst_ratio) { worst_ratio = ratio; worst_ch = ch; }
            std::printf("[..] FM%d: %zu patch(es) en %d notas (%.1f notas por timbre)\n",
                        ch, m.size(), tot, ratio);
        }
        check(!patches.empty(), "los canales FM tienen firma de patch");
        check(distinct >= 2, "hay MÁS DE UN timbre (la firma discrimina instrumentos)");
        std::printf("[..] peor canal: FM%d con %.1f notas por timbre\n",
                    worst_ch, worst_ratio);
        check(worst_ratio >= 4.0,
              "la firma IDENTIFICA un timbre (no cambia nota a nota)");

        // ¿El VOLUMEN está contaminando la identidad? (#261)
        //
        // En FM el Total Level de un operador PORTADOR es el volumen del canal,
        // no su timbre. Si entra en la firma, un fundido o un acento la cambian
        // y el mismo instrumento se fragmenta en varias identidades — con lo
        // cual el artista tendría que re-asignarlo cada vez que el juego baja
        // el volumen. Es el defecto que tuvo el hash de sprite antes de hacerse
        // invariante al flip, y conviene descubrirlo AHORA: cambiar la forma de
        // la firma después obliga a re-autorar todo lo asignado.
        //
        // Se compara la MISMA toma con las dos firmas. Menos identidades con la
        // firma sin TL de portador = el volumen estaba fragmentando.
        std::map<int, std::map<uint64_t, int>> nc;
        for (const NoteEvent& e : ev)
            if (e.on && e.chip == kChipFM && e.patch_legacy) ++nc[e.channel][e.patch_legacy];
        size_t distinct_nc = 0;
        for (const auto& [ch, m] : nc) distinct_nc += m.size();

        // `distinct` es la firma VIGENTE (sin TL de portador); `distinct_nc` la
        // legacy. La medición se conserva porque es la evidencia de por qué la
        // firma tiene la forma que tiene — si alguien la "simplifica" volviendo
        // a incluir el TL, este número se lo dice.
        std::printf("\n[..] IDENTIDAD vs VOLUMEN (#261)\n");
        std::printf("[..]   firma VIGENTE (sin TL de portador): %zu timbres\n", distinct);
        std::printf("[..]   firma legacy (con TL de portador):  %zu timbres\n", distinct_nc);
        for (const auto& [ch, m] : patches) {
            const size_t viva = m.size();
            const size_t leg  = nc.count(ch) ? nc.at(ch).size() : 0;
            if (viva != leg)
                std::printf("[..]   FM%d: legacy %zu -> vigente %zu%s\n", ch, leg, viva,
                            viva < leg ? "  (el volumen fragmentaba)" : "  (!! sube: revisar)");
        }
        if (distinct < distinct_nc)
            std::printf("[ok] la firma vigente evita %zu identidades espurias — el\n"
                        "     volumen del portador NO fragmenta el timbre.\n",
                        distinct_nc - distinct);
        else
            std::printf("[..] sin diferencia EN ESTA TOMA: el volumen no cambió acá.\n"
                        "     No invalida la decisión, sólo no la ejercita.\n");
        // NO es un check: la magnitud depende de la toma, y hacerla aserción
        // rompería el spike en una toma sin dinámica.
    }

    // ── 4. ALCANCE: cuánto NO cubre un SoundFont ────────────────────────────
    // El canal 6 en modo DAC son samples PCM (batería, voces digitalizadas).
    // Ahí el SF2 no aplica y sigue el camino de sustitución de samples que ya
    // existe — la mecánica de Secuencias NO se reemplaza, se complementa.
    {
        const double dac_share = writes ? 100.0 * dac / writes : 0.0;
        std::printf("[..] escrituras al DAC: %.1f%% del bus (PCM: fuera del alcance del SF2)\n",
                    dac_share);
        check(dac_share < 99.0, "no TODO el audio es PCM (queda FM que resintetizar)");
    }

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}

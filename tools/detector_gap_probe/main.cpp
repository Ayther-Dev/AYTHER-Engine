// ---------------------------------------------------------------------------
// detector_gap_probe — ¿qué parte del audio NO está en los eventos? (#282)
//
// mute_silence_probe midió el RESIDUO (con todo silenciado queda -33,7 dBFS de
// pico) y demostró que no es cola de release: son frames que ninguna ventana
// cubre. Este probe responde la pregunta siguiente, la que separa las causas:
// ¿hubo un key-on ahí que el detector no convirtió en evento?
//
// Método: un TRACKER DE KEY SOMBRA alimentado directo del bus crudo
// (FrameView.chip_writes, ids 0x109/0x10A del fork), independiente del
// detector Rust. Decodifica lo mínimo para saber cuándo un canal está keyed:
//
//   FM  — pares latch/data por banco; reg 0x28 = key on/off (canal en bits
//         0-2, operadores en 4-7), reg 0x2B = modo DAC (FM6).
//   PSG — byte latch 1cctdddd: canal cc, tipo t (1=volumen), atenuación 0xF
//         = canal callado.
//
// Con eso se cruza, frame por frame y por canal:
//
//   ON del bus  ∩  ventana de evento   → cubierto (lo esperable)
//   ON del bus  ∖  toda ventana        → HUECO del detector: el driver tiene
//                                        la voz keyed y ningún evento lo sabe
//   ventana     ∖  ON del bus          → ventana de más (debería ser ~0)
//
// y el censo de transiciones: cada key-on del bus, ¿tiene un evento que
// arranca en ese frame? Un key-on sin evento es pérdida directa; un frame ON
// sin key-on perdido cerca es una nota que venía de antes (residual).
//
// Clasificaciones que el detector excluye A PROPÓSITO y acá se cuentan aparte
// (no son huecos): key-ons de FM6 en modo DAC (los maneja el stream DAC) y
// re-attacks con el canal ya keyed (la ventana continúa, no se pierde nada).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target detector_gap_probe
//   Args:  <core.dll> <rom> <recording.arp>
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using ayther::AytherSession;

namespace {

constexpr int kFm  = 6;    // FM0-5 → bits 0-5
constexpr int kPsg = 4;    // PSG0-3 → bits 6-9
constexpr int kCh  = kFm + kPsg;

struct ChannelStats {
    uint32_t frames_bus_on      = 0;  // frames con la voz keyed según el bus
    uint32_t frames_window      = 0;  // frames cubiertos por ventanas de eventos
    uint32_t frames_on_covered  = 0;  // ON del bus ∩ ventana
    uint32_t frames_on_orphan   = 0;  // ON del bus sin ventana → hueco
    uint32_t frames_win_no_on   = 0;  // ventana sin ON del bus
    uint32_t keyons             = 0;  // transiciones off→on del bus
    uint32_t keyons_matched     = 0;  // con evento que arranca en f (±1)
    uint32_t keyons_orphan      = 0;  // sin evento correspondiente
    uint32_t keyons_dac_mode    = 0;  // FM6 keyed con DAC activo (excluido a propósito)
    uint32_t reattacks          = 0;  // key-on con el canal ya keyed
    uint32_t events             = 0;  // eventos del detector en este canal
    // Frames donde el canal NO está keyed y aun así el driver le escribe
    // registros (patch/TL/frecuencia). Candidato a «golpecito»: tocar una voz
    // que ringea su release produce un transitorio que ningún evento cubre.
    uint32_t frames_unkeyed_poked = 0;
};

const char* ch_name(int c) {
    static const char* n[kCh] = { "FM0", "FM1", "FM2", "FM3", "FM4", "FM5",
                                  "PSG0", "PSG1", "PSG2", "PSG3" };
    return n[c];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "detector_gap_probe — cruza las ventanas del detector contra el bus crudo (#282)\n"
            "uso: %s <core.dll> <rom> <recording.arp>\n", argv[0]);
        return 2;
    }
    AytherSession::Config cfg;
    cfg.core_path    = argv[1];
    cfg.rom_path     = argv[2];
    cfg.enable_audio = false;
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create\n"); return 1; }
    std::unique_ptr<AytherSession>& s = *r;

    auto rec_opt = ayther::AytherRecording::load(argv[3]);
    if (!rec_opt) { std::fprintf(stderr, "[FAIL] rec\n"); return 1; }
    const ayther::AytherRecording& rec = *rec_opt;
    const uint32_t nframes = rec.frame_count();

    const uint32_t nev = s->analyze_audio_events(rec);
    const AytherAudioEvent* evs = s->audio_events();
    if (!nev) { std::fprintf(stderr, "[FAIL] sin eventos\n"); return 1; }

    // ---- Ventanas del detector, por canal, como bitmap de frames ------------
    // (bitmap y no lista: la comparación de abajo es por frame y O(1) acá vale
    // más que la memoria — 10 canales × nframes bytes).
    std::vector<std::vector<uint8_t>> window(kCh, std::vector<uint8_t>(nframes, 0));
    // Arranques de evento por canal (para casar key-ons): frame → cuenta.
    std::vector<std::vector<uint8_t>> ev_start(kCh, std::vector<uint8_t>(nframes, 0));
    ChannelStats st[kCh];
    for (uint32_t i = 0; i < nev; ++i) {
        const AytherAudioEvent& e = evs[i];
        const int c = e.chip == 0 ? e.channel : kFm + e.channel;
        if (c < 0 || c >= kCh) continue;
        ++st[c].events;
        const uint32_t e0 = std::min(e.start_frame, nframes ? nframes - 1 : 0);
        const uint32_t e1 = std::min(e.end_frame,   nframes ? nframes - 1 : 0);
        for (uint32_t f = e0; f <= e1; ++f) window[c][f] = 1;
        if (e.start_frame < nframes) ++ev_start[c][e.start_frame];
    }

    // ---- Tracker sombra sobre el bus crudo ----------------------------------
    // Estado FM: registro latcheado por banco + modo DAC + keyed por canal.
    bool    dac_enabled = false;
    bool    bus_on[kCh] = { false };
    uint8_t psg_latch_ch = 0; bool psg_latch_vol = false;

    struct Orphan { uint32_t frame; int ch; };
    std::vector<Orphan> orphan_list;
    bool poked_this_frame[kCh] = { false };   // regs del canal escritos este frame

    s->replay_invalidate();
    for (uint32_t f = 0; f < nframes; ++f) {
        const ayther::FrameView* fv = s->replay_seek(rec, f);
        if (!fv) continue;
        for (uint32_t w = 0; w < fv->chip_write_count; ++w) {
            const AytherAudioWrite& aw = fv->chip_writes[w];
            if (aw.chip == 0) {
                // FM — `addr` es el REGISTRO ya latcheado y su bit 0x100 el
                // banco (ver ayther_core_ffi.h). Reconstruir acá el protocolo
                // de puertos, como se hacía hasta el fork 3fc6ee89, dejaba a
                // este probe sin ver un solo key-on del bus: comparaba dos
                // censos vacíos y no reportaba ninguna discrepancia.
                const int bank = (aw.addr & 0x100) ? 1 : 0;
                const uint8_t reg = uint8_t(aw.addr & 0xFF);
                if (bank == 0 && reg == 0x2B) dac_enabled = (aw.data & 0x80) != 0;
                // Escritura a registros POR-CANAL (0x30-0xB6: patch, TL,
                // frecuencia, algoritmo/feedback, panning) — para el censo de
                // «tocado sin key» de abajo.
                if (reg >= 0x30 && reg <= 0xB6 && (reg & 3) != 3) {
                    const int pc = bank * 3 + (reg & 3);
                    if (pc < kFm) poked_this_frame[pc] = true;
                }
                if (bank != 0 || reg != 0x28) continue;
                const uint8_t lo = aw.data & 0x03;
                if (lo == 3) continue;
                const int ch = lo + ((aw.data & 0x04) ? 3 : 0);
                const bool on = (aw.data & 0xF0) != 0;
                if (on) {
                    if (ch == 5 && dac_enabled) { ++st[ch].keyons_dac_mode; continue; }
                    if (bus_on[ch]) { ++st[ch].reattacks; continue; }
                    bus_on[ch] = true;
                    ++st[ch].keyons;
                    // ¿El detector abrió un evento acá? (±1 frame de tolerancia)
                    bool matched = false;
                    for (int d = -1; d <= 1 && !matched; ++d) {
                        const int64_t g = int64_t(f) + d;
                        if (g >= 0 && g < nframes && ev_start[ch][size_t(g)]) matched = true;
                    }
                    if (matched) ++st[ch].keyons_matched;
                    else { ++st[ch].keyons_orphan; orphan_list.push_back({ f, ch }); }
                } else {
                    bus_on[ch] = false;
                }
            } else {
                // PSG — sólo el volumen decide keyed (atenuación 0xF = callado).
                if (aw.data & 0x80) {
                    psg_latch_ch  = (aw.data >> 5) & 3;
                    psg_latch_vol = (aw.data & 0x10) != 0;
                    if (psg_latch_vol)
                        bus_on[kFm + psg_latch_ch] = (aw.data & 0x0F) != 0x0F;
                } else if (psg_latch_vol) {
                    bus_on[kFm + psg_latch_ch] = (aw.data & 0x0F) != 0x0F;
                }
            }
        }
        // Censo del frame: keyed vs ventana, por canal. El key-on de este mismo
        // frame ya está aplicado — el evento del detector también arranca acá
        // ([key-on, key-off-1]), así que las dos vistas quedan en fase.
        for (int c = 0; c < kCh; ++c) {
            const bool on  = bus_on[c];
            const bool win = window[c][f] != 0;
            if (on)         ++st[c].frames_bus_on;
            if (win)        ++st[c].frames_window;
            if (on && win)  ++st[c].frames_on_covered;
            if (on && !win) ++st[c].frames_on_orphan;
            if (!on && win) ++st[c].frames_win_no_on;
            if (!on && !win && poked_this_frame[c]) ++st[c].frames_unkeyed_poked;
            poked_this_frame[c] = false;
        }
    }

    // ---- Reporte -------------------------------------------------------------
    std::printf("toma: %u frames, %u eventos del detector\n\n", nframes, nev);
    std::printf("%-5s %7s %7s %8s %8s %8s %9s | %7s %8s %8s %6s %5s\n",
                "canal", "ON bus", "ventana", "cubierto", "HUECO", "vent s/ON",
                "poke s/key", "key-ons", "casados", "HUERFANO", "reatk", "ev");
    uint32_t tot_on = 0, tot_orphan_frames = 0, tot_keyons = 0, tot_orphan_keyons = 0;
    uint32_t tot_poked = 0;
    for (int c = 0; c < kCh; ++c) {
        const ChannelStats& x = st[c];
        if (!x.frames_bus_on && !x.frames_window && !x.keyons) continue;
        std::printf("%-5s %7u %7u %8u %8u %8u %9u | %7u %8u %8u %6u %5u\n",
                    ch_name(c), x.frames_bus_on, x.frames_window,
                    x.frames_on_covered, x.frames_on_orphan, x.frames_win_no_on,
                    x.frames_unkeyed_poked,
                    x.keyons, x.keyons_matched, x.keyons_orphan,
                    x.reattacks, x.events);
        tot_on            += x.frames_bus_on;
        tot_orphan_frames += x.frames_on_orphan;
        tot_keyons        += x.keyons;
        tot_orphan_keyons += x.keyons_orphan;
        tot_poked         += x.frames_unkeyed_poked;
    }
    uint32_t dac_keyons = 0;
    for (int c = 0; c < kCh; ++c) dac_keyons += st[c].keyons_dac_mode;

    std::printf("\nkey-ons del bus: %u — casados con evento: %u — HUERFANOS: %u"
                " — FM6 en modo DAC (excluidos a proposito): %u\n",
                tot_keyons, tot_keyons - tot_orphan_keyons - 0, tot_orphan_keyons,
                dac_keyons);
    std::printf("frames con voz keyed segun el bus: %u — sin ventana que los cubra: %u (%.1f%%)\n",
                tot_on, tot_orphan_frames,
                tot_on ? 100.0 * tot_orphan_frames / tot_on : 0.0);
    std::printf("frames con canal NO keyed y registros escritos (candidato a golpecito): %u\n",
                tot_poked);

    if (!orphan_list.empty()) {
        std::printf("\nprimeros key-ons huerfanos (frame/canal):");
        const size_t n = std::min<size_t>(orphan_list.size(), 20);
        for (size_t i = 0; i < n; ++i)
            std::printf(" %u/%s", orphan_list[i].frame, ch_name(orphan_list[i].ch));
        std::printf("%s\n", orphan_list.size() > n ? " ..." : "");
    }

    // Lectura: HUERFANO > 0 = el detector pierde key-ons (pérdida directa).
    // HUECO alto con HUERFANO 0 = el ON del bus viene de ANTES de la toma o de
    // un cierre temprano — no hay key-on que perder, la causa es otra.
    std::printf("\n[%s] censo completo — este probe MIDE, la interpretacion va en #282\n",
                tot_keyons ? "OK" : "FAIL");
    return tot_keyons ? 0 : 5;
}

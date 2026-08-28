// ---------------------------------------------------------------------------
// psg_identity_probe — ¿hay algo que separe dos tonos del PSG? (E-8, #407)
//
// POR QUÉ EXISTE. #407 propone consumir los eventos semánticos del Audio Probe
// v2 para que «la regla `instrument` sobre PSG separe lo que hoy no separa». El
// diagnóstico de partida es correcto y está medido en #389 F3: hoy
// `psg_instrument()` devuelve la MISMA constante para los tres canales de tono,
// así que una regla por instrumento sobre PSG alcanza a cualquier tono.
//
// Pero antes de cablear el probe v2 hay que contestar si eso es una limitación
// del método o una descripción del hardware. El SN76489 no tiene timbre
// configurable: un canal de tono es una onda cuadrada pura y lo único que el
// juego escribe es frecuencia (10 bits) y atenuación (4 bits). No hay patch, no
// hay envolvente, no hay forma de onda. Si es así, no existe «instrumento» que
// descubrir y ningún evento semántico lo va a inventar — porque el probe v2
// deriva PITCH/VOLUME/NOTE_ON de los MISMOS registros que este probe decodifica.
//
// Este probe mide, sobre música real del juego:
//
//   1. Cuántos key-ons hay por canal y qué rango de tono usa cada uno.
//   2. Cuánto se SOLAPAN los canales en frecuencia — si dos canales tocan las
//      mismas notas, ni siquiera el rango los separa.
//   3. Cuántas notas distintas comparten canales distintos, que es la pregunta
//      exacta de la regla `instrument`: dada una nota, ¿de qué canal salió?
//
// Lo que NO mide: si el ROL musical (melodía / bajo / arpegio) es estable por
// canal. Eso es una hipótesis distinta —identidad por comportamiento y no por
// timbre— y merece su propia medición.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target psg_identity_probe
//   Env:   AYTHER_PROBE_ROM (obligatoria) · AYTHER_ABI_CORE (default: el del repo)
//          AYTHER_PSG_FRAMES (default 3600 = 1 minuto de música)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_env.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using ayther::FrameView;

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

constexpr uint8_t kChipPsg = 1;

/// Lo que el juego escribe de un canal de tono del PSG. Es TODO lo que el chip
/// ofrece: no hay un campo más que consultar.
struct ChannelStats {
    uint64_t key_ons = 0;
    uint16_t tone_min = 0x3FF, tone_max = 0;
    std::set<uint16_t> tones;      ///< notas distintas tocadas
};

}  // namespace

int main() {
    namespace fs = std::filesystem;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string rom  = env_or("AYTHER_PROBE_ROM", "");
    const std::string core = env_or("AYTHER_ABI_CORE",
        root + "/third_party/cores/genesis_plus_gx_libretro_vram.dll");
    const int frames = env_int("AYTHER_PSG_FRAMES", 3600);

    std::printf("=== psg_identity_probe (E-8 #407) — ¿que separa dos tonos del PSG? ===\n");
    if (rom.empty() || !fs::exists(rom) || !fs::exists(core)) {
        std::printf("[skip] falta ROM o core\n"); return 0;
    }

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // Boot. `start` entra al juego (el de Sonic 2 y compañía); `none` deja
    // correr el attract, que es donde la mayoría de los juegos tocan música
    // sin que haya que saber su menú. Sin esto el probe medía un solo juego:
    // con START, dos de tres ROMs quedaban en una pantalla muda.
    const std::string boot = env_or("AYTHER_PSG_BOOT", "start");
    constexpr uint16_t START = 1u << 3;
    for (int i = 0; i < 1400; ++i) {
        s->set_input(0, (boot == "start" && i % 32 == 0) ? START : 0);
        s->step();
    }
    s->set_input(0, 0);

    // Decodificación del bus del PSG, idéntica a la del chip: byte con bit 7 =
    // latch (canal en bits 6-5, tipo en bit 4), sin bit 7 = 6 bits altos del
    // tono del último canal latcheado. Es la misma que hace `audio_probe_psg_raw`
    // en el core — de ahí que sus eventos PITCH/VOLUME no traigan nada que no
    // esté acá.
    ChannelStats channel[3];
    uint8_t  latch_ch = 0; bool latch_vol = false;
    uint16_t tone[4] = {0, 0, 0, 0};
    bool     active_channel[4] = {false, false, false, false};
    uint64_t escrituras_psg = 0, key_ons_ruido = 0, escrituras_bus = 0;

    for (int f = 0; f < frames; ++f) {
        const FrameView& fv = s->step();
        escrituras_bus += fv.chip_write_count;
        for (uint32_t w = 0; w < fv.chip_write_count; ++w) {
            const auto& aw = fv.chip_writes[w];
            if (aw.chip != kChipPsg) continue;
            ++escrituras_psg;
            const uint8_t d = aw.data;
            uint8_t ch; bool is_volume;
            if (d & 0x80) {
                latch_ch  = (d >> 5) & 3;
                latch_vol = (d & 0x10) != 0;
                ch = latch_ch; is_volume = latch_vol;
                if (is_volume) { /* atenuación en los 4 bits bajos */ }
                else tone[ch] = (uint16_t)((tone[ch] & 0x3F0) | (d & 0x0F));
            } else {
                ch = latch_ch; is_volume = latch_vol;
                if (!is_volume) tone[ch] = (uint16_t)((tone[ch] & 0x00F) | ((d & 0x3F) << 4));
            }
            if (is_volume) {
                const bool suena = (d & 0x0F) != 0x0F;
                if (suena && !active_channel[ch]) {
                    active_channel[ch] = true;
                    if (ch < 3) {
            ChannelStats& k = channel[ch];
                        ++k.key_ons;
                k.tones.insert(tone[ch]);
                k.tone_min = (std::min)(k.tone_min, tone[ch]);
                k.tone_max = (std::max)(k.tone_max, tone[ch]);
                    } else ++key_ons_ruido;
                } else if (!suena) active_channel[ch] = false;
            }
        }
    }

    // El total del bus separa «este juego no usa PSG» de «no estoy midiendo
    // nada»: sin ese número, un cero de PSG parece un hallazgo y puede ser un
    // boot que dejó al juego en una pantalla muda. Pasó con dos de las tres
    // ROMs del corpus.
    std::printf("\n  %d frames · bus: %llu escrituras · PSG: %llu · ruido: %llu key-ons\n",
                frames, (unsigned long long)escrituras_bus,
                (unsigned long long)escrituras_psg,
                (unsigned long long)key_ons_ruido);
    std::printf("  canal   key-ons   notas distintas   rango de tono\n");
    for (int ch = 0; ch < 3; ++ch)
        std::printf("    %d     %7llu   %15zu   %u..%u\n", ch,
                    (unsigned long long)channel[ch].key_ons, channel[ch].tones.size(),
                    channel[ch].tone_min > channel[ch].tone_max ? 0 : channel[ch].tone_min,
                    channel[ch].tone_max);

    // ---- ¿Alguna nota es exclusiva de un canal? ----------------------------
    // Si una nota suena en dos canales distintos, entonces ni el canal ni la
    // frecuencia alcanzan para decir «esto es tal instrumento»: son la MISMA
    // onda cuadrada a la misma altura, emitida por otro oscilador.
    // ¿Los canales tienen ROLES estables (melodía / contramelodía / bajo)?
    // Es la hipótesis de #410, y se mide por el rango de tono que ocupa cada
    // uno: en el SN76489 el valor es el DIVISOR, así que más alto = más grave.
    for (int ch = 0; ch < 3; ++ch) {
        if (channel[ch].tones.empty()) continue;
        std::vector<uint16_t> t(channel[ch].tones.begin(), channel[ch].tones.end());
        std::sort(t.begin(), t.end());
        std::printf("    canal %d · mediana de tono %u  (p10 %u · p90 %u)\n", ch,
                    t[t.size() / 2], t[t.size() / 10], t[(t.size() * 9) / 10]);
    }

    std::map<uint16_t, int> channels_by_tone;   // nota → bitmask de canales
    for (int ch = 0; ch < 3; ++ch)
        for (uint16_t t : channel[ch].tones) channels_by_tone[t] |= (1 << ch);
    size_t shared_tones = 0;
    for (const auto& [t, m] : channels_by_tone)
        if (m & (m - 1)) ++shared_tones;   // más de un bit ⇒ compartida
    std::printf("\n  notas distintas en total: %zu · tocadas por MAS de un canal: %zu (%.0f%%)\n",
                channels_by_tone.size(), shared_tones,
                channels_by_tone.empty() ? 0.0
                    : 100.0 * double(shared_tones) / double(channels_by_tone.size()));

    const uint64_t total_tone_events = channel[0].key_ons + channel[1].key_ons + channel[2].key_ons;
    check(total_tone_events > 0, "NO-VACUIDAD: hubo notas de tono del PSG que analizar");
    check(escrituras_bus > 0,
          "NO-VACUIDAD: el bus de audio llega (si esto falla, no se midio NADA)");
    check(escrituras_psg > 0, "el juego escribio al PSG");

    // El veredicto de #407: el chip no ofrece nada más que canal, frecuencia y
    // atenuación. Si además las notas se comparten entre canales, ni siquiera
    // el rango sirve como identidad de «instrumento».
    check(shared_tones > 0,
          "HALLAZGO: hay notas que suenan en MAS de un canal — el rango no identifica");

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

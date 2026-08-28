// ---------------------------------------------------------------------------
// mute_silence_probe — «silenciar todo» debería dar SILENCIO. Mide si lo da.
//
// POR QUÉ NO ALCANZA instrument_mute_probe: aquél compara la máscara aplicada
// contra las ventanas de los eventos, o sea verifica que el motor hace lo que
// dice hacer. Pero el reporte del usuario es que con TODO silenciado se siguen
// oyendo clics suaves y chisporroteo — y eso puede ser perfectamente
// consistente con una máscara «correcta»: si la ventana del evento termina en
// el key-off, la COLA DE RELEASE del canal FM sigue sonando después, y al
// levantarse el mute reaparece de golpe desde una amplitud no nula. Un salto
// de 0 a A en una muestra es exactamente lo que el oído llama «clic».
//
// Así que este probe no mira máscaras: mira el PCM. Dos corridas:
//
//   CONTROL — mute manual 0x3FF (los 10 canales, TODO el tiempo). Esto tiene
//             que ser silencio digital. Si acá ya hay señal, el leak está en
//             el mecanismo de mute (core) y no en la cobertura, y todo lo
//             demás que midamos sería sobre arena.
//
//   REAL    — mute por INSTRUMENTO con todos los instrumentos detectados, que
//             es literalmente lo que hace «Silenciar todos» del panel Sonidos.
//             El residuo de acá es lo que el usuario está escuchando.
//
// Y para el residuo, la pregunta que decide entre las dos causas posibles:
// ¿DÓNDE cae? Si se concentra en los frames INMEDIATAMENTE POSTERIORES al
// cierre de una ventana, es la cola de release destapada (falta `tail`, como
// el que occurrence_mute_at ya tiene). Si se reparte por cualquier lado, es
// audio que el detector nunca capturó como evento y el mute no puede cubrir.
//
// El detector de clics no es el pico: es el SALTO entre muestras contiguas
// (|x[n]-x[n-1]|). Una cola de release que reaparece tiene pico chico pero
// salto grande — por eso se oye como clic y no como nota.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target mute_silence_probe
//   Args:  <core.dll> <rom> <recording.arp> [dir_salida]
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using ayther::AytherSession;

namespace {

// --- WAV S16 estéreo: sólo lo justo para recuperar lo que escribimos --------
bool read_wav_s16(const std::string& path, std::vector<int16_t>& out,
                  uint32_t* rate, uint16_t* channels) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> all;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 44) { std::fclose(f); return false; }
    all.resize(static_cast<size_t>(n));
    const bool ok = std::fread(all.data(), 1, all.size(), f) == all.size();
    std::fclose(f);
    if (!ok) return false;
    if (std::memcmp(all.data(), "RIFF", 4) != 0 ||
        std::memcmp(all.data() + 8, "WAVE", 4) != 0) return false;

    size_t p = 12;
    while (p + 8 <= all.size()) {
        char id[5] = {0};
        std::memcpy(id, all.data() + p, 4);
        uint32_t sz = 0;
        std::memcpy(&sz, all.data() + p + 4, 4);
        const size_t body = p + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= all.size()) {
            uint16_t ch = 0; uint32_t sr = 0;
            std::memcpy(&ch, all.data() + body + 2, 2);
            std::memcpy(&sr, all.data() + body + 4, 4);
            if (channels) *channels = ch;
            if (rate) *rate = sr;
        } else if (std::memcmp(id, "data", 4) == 0) {
            const size_t avail = std::min<size_t>(sz, all.size() - body);
            out.resize(avail / 2);
            std::memcpy(out.data(), all.data() + body, out.size() * 2);
            return true;
        }
        p = body + sz + (sz & 1);
    }
    return false;
}

struct Residual {
    double   peak_dbfs   = -999.0;
    double   rms_dbfs    = -999.0;
    uint64_t nonzero     = 0;
    uint64_t total       = 0;
    std::vector<int32_t> frame_peak;   // por frame de video
    std::vector<int32_t> frame_step;   // salto máx entre muestras contiguas
};

double to_dbfs(double lin) {
    if (lin <= 0.0) return -999.0;
    return 20.0 * std::log10(lin / 32768.0);
}

Residual analyze(const std::vector<int16_t>& pcm, uint16_t channels,
                 uint32_t frames) {
    Residual r;
    r.total = pcm.size();
    if (pcm.empty() || frames == 0) return r;

    int32_t peak = 0;
    double  sumsq = 0.0;
    for (int16_t v : pcm) {
        const int32_t a = v < 0 ? -int32_t(v) : int32_t(v);
        if (a > peak) peak = a;
        if (v != 0) ++r.nonzero;
        sumsq += double(v) * double(v);
    }
    r.peak_dbfs = to_dbfs(peak);
    r.rms_dbfs  = to_dbfs(std::sqrt(sumsq / double(pcm.size())));

    // Las muestras del WAV son intercaladas; el paso por frame de video sale de
    // la duración real, no de 44100/fps — el mixdown puede no dar exactamente
    // ese número y desalinearía toda la clasificación de abajo.
    //
    // Y el corte va con acumulador FRACCIONARIO, no con división entera: a
    // 59,9227 fps tocan 735,95 cuadros por frame, así que truncar pierde ~0,9
    // muestras por frame y al final de una toma de 3913 frames la ventana está
    // corrida casi 5 frames. Con esa deriva, clasificar frames como «tapado» o
    // «destapado» mide el vecino.
    if (pcm.size() / frames == 0) return r;
    const double per_frame = double(pcm.size()) / double(frames);
    r.frame_peak.assign(frames, 0);
    r.frame_step.assign(frames, 0);
    for (uint32_t f = 0; f < frames; ++f) {
        size_t a = size_t(per_frame * f + 0.5);
        size_t b = std::min(size_t(per_frame * (f + 1) + 0.5), pcm.size());
        a -= a % channels;   // no arrancar a mitad de un cuadro estéreo
        b -= b % channels;
        if (a >= b) continue;
        int32_t fp = 0, fs = 0;
        for (size_t i = a; i < b; ++i) {
            const int32_t v = pcm[i];
            const int32_t av = v < 0 ? -v : v;
            if (av > fp) fp = av;
            // Salto entre muestras del MISMO canal (de ahí el paso de
            // `channels`): comparar L contra R sería ruido inventado.
            if (i >= a + channels) {
                int32_t d = v - int32_t(pcm[i - channels]);
                if (d < 0) d = -d;
                if (d > fs) fs = d;
            }
        }
        r.frame_peak[f] = fp;
        r.frame_step[f] = fs;
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "mute_silence_probe — «silenciar todo» debería dar silencio\n"
            "uso: %s <core.dll> <rom> <recording.arp> [dir_salida]\n", argv[0]);
        return 2;
    }
    const std::string core_path = argv[1], rom_path = argv[2], rec_path = argv[3];
    const std::string out_dir = argc > 4 ? argv[4] : ".";

    AytherSession::Config cfg;
    cfg.core_path = core_path.c_str();
    cfg.rom_path  = rom_path.c_str();
    cfg.enable_audio = false;          // el mixdown no usa el device
    auto r = AytherSession::create(cfg);
    if (!r) {
        std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str());
        return 1;
    }
    std::unique_ptr<AytherSession>& s = *r;

    auto rec_opt = ayther::AytherRecording::load(rec_path);
    if (!rec_opt) {
        std::fprintf(stderr, "[FAIL] no pude cargar %s\n", rec_path.c_str());
        return 1;
    }
    const ayther::AytherRecording& rec = *rec_opt;

    const uint32_t nev = s->analyze_audio_events(rec);
    const AytherAudioEvent* evs = s->audio_events();
    const uint32_t frames = rec.frame_count();
    std::printf("toma: %s — %u frames, %u eventos\n",
                rec_path.c_str(), frames, nev);
    if (nev == 0) {
        std::fprintf(stderr, "[FAIL] sin eventos — ¿core sin log de chip?\n");
        return 1;
    }

    std::vector<uint64_t> insts;
    {
        std::unordered_set<uint64_t> seen;
        for (uint32_t i = 0; i < nev; ++i)
            if (seen.insert(evs[i].instrument).second)
                insts.push_back(evs[i].instrument);
    }
    std::printf("instrumentos distintos: %zu\n", insts.size());
    {
        uint32_t by_chip[2] = {0, 0};
        for (uint32_t i = 0; i < nev; ++i)
            if (evs[i].chip < 2) ++by_chip[evs[i].chip];
        std::printf("eventos por chip: FM=%u  PSG=%u\n", by_chip[0], by_chip[1]);
    }

    // ---- ¿El juego USA el PSG en esta toma? --------------------------------
    // Si el detector no emitió eventos PSG puede ser por dos motivos opuestos:
    // el juego no lo toca, o el detector lo pierde. La diferencia se ve en el
    // BUS, no en los eventos — así que se recorre la toma contando escrituras
    // crudas. En el SN76489 un byte con bit7=1 es LATCH: bits 6-5 = canal,
    // bit 4 = 1 → atenuación (0xF = mudo). Una atenuación distinta de 0xF es
    // el juego pidiendo que ese canal SUENE.
    {
        uint64_t w_fm = 0, w_psg = 0;
        uint32_t psg_audible[4] = {0}, psg_silence[4] = {0};
        uint32_t fm_keyon = 0;
        for (uint32_t f = 0; f < frames; ++f) {
            const ayther::FrameView* fv = s->replay_seek(rec, f);
            if (!fv) break;
            for (uint32_t i = 0; i < fv->chip_write_count; ++i) {
                const AytherAudioWrite& w = fv->chip_writes[i];
                if (w.chip == 0) {
                    ++w_fm;
                    if (w.addr == 0 && w.data == 0x28) ++fm_keyon;
                    continue;
                }
                ++w_psg;
                if (!(w.data & 0x80)) continue;            // byte de datos
                if (!(w.data & 0x10)) continue;            // latch de TONO
                const int ch = (w.data >> 5) & 3;
                if ((w.data & 0x0F) == 0x0F) ++psg_silence[ch];
                else                          ++psg_audible[ch];
            }
        }
        std::printf("escrituras al bus: FM=%llu  PSG=%llu\n",
                    (unsigned long long)w_fm, (unsigned long long)w_psg);
        std::printf("PSG — atenuaciones pedidas por canal (audible / mudo):\n");
        for (int c = 0; c < 4; ++c)
            std::printf("   PSG%d  audible=%-7u mudo=%u\n",
                        c, psg_audible[c], psg_silence[c]);
    }

    // ---- CONTROL: los 10 canales muteados SIEMPRE -------------------------
    const std::string wav_ctl = out_dir + "/mute_control.wav";
    s->set_audio_manual_mute(0x3FF);
    if (!s->export_mixdown_wav(rec, 0, frames, wav_ctl.c_str(), true)) {
        std::fprintf(stderr, "[FAIL] export_mixdown_wav (control)\n");
        return 1;
    }
    s->set_audio_manual_mute(0);

    // ---- REAL: «Silenciar todos» del panel Sonidos ------------------------
    const std::string wav_all = out_dir + "/mute_all_instruments.wav";
    s->set_audio_instrument_mute(insts.data(), uint32_t(insts.size()));
    if (!s->export_mixdown_wav(rec, 0, frames, wav_all.c_str(), true)) {
        std::fprintf(stderr, "[FAIL] export_mixdown_wav (instrumentos)\n");
        return 1;
    }
    s->set_audio_instrument_mute(nullptr, 0);

    uint32_t rate = 0; uint16_t ch = 0;
    std::vector<int16_t> pcm_ctl, pcm_all;
    if (!read_wav_s16(wav_ctl, pcm_ctl, &rate, &ch) ||
        !read_wav_s16(wav_all, pcm_all, &rate, &ch)) {
        std::fprintf(stderr, "[FAIL] no pude releer los WAV\n");
        return 1;
    }
    std::printf("wav: %u Hz, %u canales, %zu muestras\n\n",
                rate, unsigned(ch), pcm_all.size());

    const Residual ctl = analyze(pcm_ctl, ch, frames);
    const Residual all = analyze(pcm_all, ch, frames);

    std::printf("=== CONTROL (mute manual 0x3FF, todo el tiempo) ===\n");
    std::printf("  pico %.1f dBFS · rms %.1f dBFS · muestras no-nulas %llu/%llu\n",
                ctl.peak_dbfs, ctl.rms_dbfs,
                (unsigned long long)ctl.nonzero, (unsigned long long)ctl.total);
    const bool control_silent = ctl.nonzero == 0;
    std::printf("  %s\n\n", control_silent
        ? "[ok] silencio digital — el mecanismo de mute NO tiene leak"
        : "[!!] HAY SEÑAL con todo muteado — el leak es del mecanismo, no de la cobertura");

    std::printf("=== REAL (mute por instrumento, TODOS) ===\n");
    std::printf("  pico %.1f dBFS · rms %.1f dBFS · muestras no-nulas %llu/%llu\n",
                all.peak_dbfs, all.rms_dbfs,
                (unsigned long long)all.nonzero, (unsigned long long)all.total);

    if (all.frame_step.empty()) return 0;

    // ---- COBERTURA por canal ----------------------------------------------
    // La pregunta de fondo: ¿las ventanas de los eventos cubren el tiempo en
    // que cada canal SUENA? El mute sólo puede callar lo que alguna ventana
    // tapa; lo que el detector no vio, sigue sonando por definición.
    std::vector<uint16_t> mask_at(frames, 0);
    for (uint32_t i = 0; i < nev; ++i) {
        const AytherAudioEvent& e = evs[i];
        const uint16_t bit = e.chip == 0 ? uint16_t(1u << e.channel)
                                         : uint16_t(1u << (6 + e.channel));
        for (uint32_t f = e.start_frame; f <= e.end_frame && f < frames; ++f)
            mask_at[f] |= bit;
    }
    std::printf("\n  cobertura de las ventanas por canal (%% de la toma)\n");
    for (int c = 0; c < 10; ++c) {
        uint32_t cov = 0;
        for (uint32_t f = 0; f < frames; ++f)
            if (mask_at[f] & (1u << c)) ++cov;
        char name[8];
        std::snprintf(name, sizeof name, c < 6 ? "FM%d" : "PSG%d",
                      c < 6 ? c : c - 6);
        std::printf("   %-7s %6.1f%%  (%u/%u frames)\n",
                    name, 100.0 * cov / frames, cov, frames);
    }

    // El test que decide: en los frames donde la máscara tapa los SEIS canales
    // FM no puede quedar nada del chip (el PSG se mide aparte arriba; si está
    // parqueado en mudo, no participa). Si ahí hay silencio y el residuo vive
    // en los frames con algún FM destapado, la causa es la COBERTURA y no el
    // mute. Es gratis — no hace falta re-emular aislando canales.
    //
    // OJO con la constante: pedir 0x3FF (los 10 canales) vuelve el test VACÍO
    // en cualquier juego que no toque el PSG — la condición no se cumple nunca
    // y el resultado parece «no hay evidencia» cuando en realidad no se midió.
    constexpr uint16_t kFmAll = 0x3F;
    {
        uint64_t n_full = 0, n_part = 0;
        int32_t peak_full = 0, peak_part = 0;
        for (uint32_t f = 0; f < frames; ++f) {
            if ((mask_at[f] & kFmAll) == kFmAll) {
                ++n_full;
                if (all.frame_peak[f] > peak_full) peak_full = all.frame_peak[f];
            } else {
                ++n_part;
                if (all.frame_peak[f] > peak_part) peak_part = all.frame_peak[f];
            }
        }
        std::printf("\n  residuo según cuánto tapa la máscara ese frame\n");
        if (n_full)
            std::printf("   los 6 FM tapados       : n=%-6llu  pico=%-7d (%.1f dBFS)\n",
                        (unsigned long long)n_full, peak_full, to_dbfs(peak_full));
        else
            std::printf("   los 6 FM tapados       : NUNCA ocurre\n");
        if (n_part)
            std::printf("   con algún FM destapado : n=%-6llu  pico=%-7d (%.1f dBFS)\n",
                        (unsigned long long)n_part, peak_part, to_dbfs(peak_part));
    }

    // ---- ¿Alcanzaría con una COLA de release? -----------------------------
    // occurrence_mute_at ya extiende su ventana 15 frames por esta misma razón;
    // el mute por instrumento y el del sintetizador cierran en el key-off seco.
    // Si los huecos son cortos, una cola los tapa y el arreglo es de una línea.
    // Si son largos, el hueco es silencio real del canal y una cola sólo
    // taparía audio que SÍ debería sonar.
    std::printf("\n  cobertura FM según la cola de release que se agregue\n");
    for (uint32_t tail : {0u, 5u, 10u, 15u, 30u, 60u, 120u}) {
        std::vector<uint16_t> m(frames, 0);
        for (uint32_t i = 0; i < nev; ++i) {
            const AytherAudioEvent& e = evs[i];
            if (e.chip != 0) continue;
            const uint16_t bit = uint16_t(1u << e.channel);
            for (uint32_t f = e.start_frame; f <= e.end_frame + tail && f < frames; ++f)
                m[f] |= bit;
        }
        // Lo que importa no es la cobertura sino la ENERGÍA que queda suelta:
        // un hueco donde el canal ya está callado no molesta a nadie. Se mide
        // el residuo que sobrevive en los frames que ESTA cola no llega a
        // tapar — es la cota de cuánto arreglaría subir la cola.
        uint32_t full = 0;
        int32_t  peak_loose = 0;
        double   sum_loose = 0;
        for (uint32_t f = 0; f < frames; ++f) {
            if ((m[f] & kFmAll) == kFmAll) { ++full; continue; }
            sum_loose += double(all.frame_peak[f]) * double(all.frame_peak[f]);
            if (all.frame_peak[f] > peak_loose) peak_loose = all.frame_peak[f];
        }
        const uint32_t loose = frames - full;
        std::printf("   cola %-4u → 6 FM tapados %5.1f%%  ·  residuo suelto: "
                    "pico %6.1f dBFS  rms %6.1f dBFS\n",
                    tail, 100.0 * full / frames, to_dbfs(peak_loose),
                    loose ? to_dbfs(std::sqrt(sum_loose / loose)) : -999.0);
    }

    // ---- ¿DÓNDE cae el residuo? -------------------------------------------
    // Para cada frame: ¿está DENTRO de alguna ventana, o a cuántos frames del
    // cierre más cercano? Ahí se separan las dos causas.
    std::vector<int32_t> dist(frames, -2);   // -1 = dentro de una ventana
    for (uint32_t f = 0; f < frames; ++f) {
        int32_t best = -2;
        bool inside = false;
        for (uint32_t i = 0; i < nev && !inside; ++i) {
            const AytherAudioEvent& e = evs[i];
            if (f >= e.start_frame && f <= e.end_frame) { inside = true; break; }
            if (f > e.end_frame) {
                const int32_t d = int32_t(f - e.end_frame);
                if (best < 0 || d < best) best = d;
            }
        }
        dist[f] = inside ? -1 : best;
    }

    // Energía del residuo por distancia al cierre. Si la cola de release es la
    // causa, esto se desploma después de ~15 frames (el release de un FM).
    constexpr int kBins = 21;
    double  bin_sum[kBins] = {0};
    int32_t bin_max[kBins] = {0};
    uint32_t bin_n[kBins] = {0};
    double inside_sum = 0, far_sum = 0;
    uint32_t inside_n = 0, far_n = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        const double e = double(all.frame_step[f]);
        if (dist[f] == -1) { inside_sum += e; ++inside_n; continue; }
        if (dist[f] < 0)   { continue; }
        if (dist[f] < kBins) {
            const int b = dist[f];
            bin_sum[b] += e; ++bin_n[b];
            if (all.frame_step[f] > bin_max[b]) bin_max[b] = all.frame_step[f];
        } else { far_sum += e; ++far_n; }
    }
    std::printf("\n  salto medio por distancia al CIERRE de la ventana más cercana\n");
    std::printf("  (el salto entre muestras es lo que se oye como clic)\n");
    for (int b = 1; b < kBins; ++b) {
        if (!bin_n[b]) continue;
        std::printf("   +%-3d frames  n=%-5u  medio=%-8.1f  máx=%d\n",
                    b, bin_n[b], bin_sum[b] / bin_n[b], bin_max[b]);
    }
    if (far_n)
        std::printf("   >%-3d frames  n=%-5u  medio=%-8.1f\n",
                    kBins - 1, far_n, far_sum / far_n);
    if (inside_n)
        std::printf("   dentro       n=%-5u  medio=%-8.1f\n",
                    inside_n, inside_sum / inside_n);

    // ---- Los peores frames, para poder ir a escucharlos --------------------
    std::vector<uint32_t> worst(frames);
    for (uint32_t f = 0; f < frames; ++f) worst[f] = f;
    std::sort(worst.begin(), worst.end(), [&](uint32_t a, uint32_t b) {
        return all.frame_step[a] > all.frame_step[b];
    });
    std::printf("\n  15 frames con el salto más grande:\n");
    for (int i = 0; i < 15 && i < int(frames); ++i) {
        const uint32_t f = worst[i];
        char where[32];
        if (dist[f] == -1)      std::snprintf(where, sizeof where, "dentro de ventana");
        else if (dist[f] < 0)   std::snprintf(where, sizeof where, "antes de todo evento");
        else                    std::snprintf(where, sizeof where, "+%d del cierre", dist[f]);
        std::printf("   f=%-6u salto=%-7d pico=%-7d  %s\n",
                    f, all.frame_step[f], all.frame_peak[f], where);
    }

    // ---- TODOS MENOS UNO --------------------------------------------------
    // El caso del reporte 2026-07-27: «noto ruidos aleatorios cuando dejo
    // activo sólo un sonido». Con 44 timbres callados, lo que se cuela deja de
    // estar enmascarado y se vuelve audible. La pregunta medible es si ese
    // residuo cae DENTRO de las ventanas del timbre que quedó sonando —donde es
    // el timbre y está bien— o AFUERA, donde no lo pidió nadie.
    //
    // Al timbre audible se le concede la misma cola que al muteado: su release
    // también suena legítimamente después del key-off.
    {
        uint64_t solo = 0;
        if (argc > 5) {
            const char* h = argv[5];
            solo = std::strtoull(h[0] == '0' && (h[1] == 'x' || h[1] == 'X') ? h + 2 : h,
                                 nullptr, 16);
        }
        if (!solo) {   // por defecto, el que más suena
            std::unordered_map<uint64_t, uint32_t> cnt;
            for (uint32_t i = 0; i < nev; ++i) ++cnt[evs[i].instrument];
            uint32_t best = 0;
            for (const auto& [k, c] : cnt) if (c > best) { best = c; solo = k; }
        }
        // ---- Distribución temporal de sus notas -------------------------
        // Para el A/B de la Biblioteca de SoundFonts: «Original» reproduce la
        // ventana entera del CANAL (música continua) y «Con este preset» sólo
        // las notas de ESTE timbre. Si el timbre es esparcido, el segundo suena
        // casi vacío sin que haya ningún defecto — hay que saber cuál de los dos
        // casos es antes de tocar el sintetizador.
        {
            std::vector<uint32_t> starts;
            for (uint32_t i = 0; i < nev; ++i)
                if (evs[i].instrument == solo && evs[i].pitch != 255)
                    starts.push_back(evs[i].start_frame);
            std::sort(starts.begin(), starts.end());
            std::printf("\n=== distribución de 0x%016llx ===\n",
                        (unsigned long long)solo);
            std::printf("  notas con altura: %zu\n", starts.size());
            if (!starts.empty()) {
                const uint32_t f0 = starts.front();
                uint32_t gap_max = 0;
                for (size_t i = 1; i < starts.size(); ++i)
                    gap_max = std::max(gap_max, starts[i] - starts[i - 1]);
                std::printf("  primera en el frame %u, última en %u\n",
                            f0, starts.back());
                std::printf("  hueco más grande entre notas: %u frames (%.1f s)\n",
                            gap_max, gap_max / 59.9227);
                for (uint32_t w : {120u, 300u, 600u, 1200u}) {
                    size_t n_in = 0;
                    for (uint32_t s : starts) if (s >= f0 && s < f0 + w) ++n_in;
                    std::printf("  ventana de %-5u frames desde la 1ª: %zu notas\n",
                                w, n_in);
                }
            }
        }

        std::vector<uint64_t> rest;
        for (uint64_t i : insts) if (i != solo) rest.push_back(i);

        const std::string wav_solo = out_dir + "/mute_all_but_one.wav";
        s->set_audio_instrument_mute(rest.data(), uint32_t(rest.size()));
        const bool ok = s->export_mixdown_wav(rec, 0, frames, wav_solo.c_str(), true);
        s->set_audio_instrument_mute(nullptr, 0);

        std::vector<int16_t> pcm_solo;
        if (ok && read_wav_s16(wav_solo, pcm_solo, nullptr, nullptr)) {
            const Residual so = analyze(pcm_solo, ch, frames);
            uint32_t n_solo = 0;
            for (uint32_t i = 0; i < nev; ++i) if (evs[i].instrument == solo) ++n_solo;
            std::printf("\n=== TODOS MENOS UNO (0x%016llx, %u eventos) ===\n",
                        (unsigned long long)solo, n_solo);
            std::printf("  pico %.1f dBFS · rms %.1f dBFS\n",
                        so.peak_dbfs, so.rms_dbfs);

            std::vector<uint8_t> legit(frames, 0);
            for (uint32_t i = 0; i < nev; ++i) {
                if (evs[i].instrument != solo) continue;
                for (uint32_t f = evs[i].start_frame;
                     f <= evs[i].end_frame + 15 && f < frames; ++f) legit[f] = 1;
            }
            double in_sum = 0, out_sum = 0;
            uint32_t in_n = 0, out_n = 0;
            int32_t in_pk = 0, out_pk = 0;
            for (uint32_t f = 0; f < frames; ++f) {
                const double e = double(so.frame_peak[f]) * double(so.frame_peak[f]);
                if (legit[f]) {
                    in_sum += e; ++in_n;
                    if (so.frame_peak[f] > in_pk) in_pk = so.frame_peak[f];
                } else {
                    out_sum += e; ++out_n;
                    if (so.frame_peak[f] > out_pk) out_pk = so.frame_peak[f];
                }
            }
            std::printf("  dentro de sus ventanas (+cola): n=%-5u pico %6.1f dBFS  rms %6.1f dBFS\n",
                        in_n, to_dbfs(in_pk),
                        in_n ? to_dbfs(std::sqrt(in_sum / in_n)) : -999.0);
            std::printf("  FUERA  (nadie lo pidió)       : n=%-5u pico %6.1f dBFS  rms %6.1f dBFS\n",
                        out_n, to_dbfs(out_pk),
                        out_n ? to_dbfs(std::sqrt(out_sum / out_n)) : -999.0);
            std::printf("  wav: %s\n", wav_solo.c_str());
        }
    }

    std::printf("\nwav: %s\n     %s\n", wav_ctl.c_str(), wav_all.c_str());
    return 0;
}

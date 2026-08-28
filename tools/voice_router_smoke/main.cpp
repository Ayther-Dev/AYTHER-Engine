// ---------------------------------------------------------------------------
// voice_router_smoke — el oráculo del router de canales por voz (#327, Fase 2).
//
// LA AFIRMACIÓN A PROBAR: silenciar una voz la saca ENTERA y NO TOCA a las
// demás, con el corte exactamente en el key-on del chip.
//
// Eso es lo que el modelo viejo no podía dar. Ahí el mute salía de VENTANAS de
// eventos, así que entre nota y nota se colaba el chip (#280) y en la juntura
// entre Secuencias se cortaba de más (#326). Acá una voz se toma el canal desde
// el key-on hasta el fin de su cola: no hay ventana en la que equivocarse, y
// esta prueba lo verifica con IGUALDAD EXACTA, no con un umbral en dBFS.
//
// Es SINTÉTICO a propósito: un log de escrituras armado a mano, sin ROM ni
// core, así corre en el ctest de todos y no depende de los juegos del usuario.
// ---------------------------------------------------------------------------

#include "voice_router.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::fprintf(stdout, "  %s %s\n", ok ? "[ok]  " : "[FALLA]", what);
    if (!ok) ++g_fail;
}

// Una escritura al YM2612: el REGISTRO ya latcheado y su dato. El protocolo de
// puertos lo resuelve el core desde el fork 3fc6ee89 (2026-08-11); emitirlo acá
// hacía que el espejo no tocara una nota y el smoke medía silencio contra
// silencio — pasaba por «el mute funciona».
void fm(std::vector<AytherAudioWrite>& v, uint32_t cyc, uint8_t reg, uint8_t val) {
    v.push_back({ cyc, reg, val, 0 });
}

// Un patch audible y simple: algoritmo 7 (los cuatro operadores son portadores,
// así que cualquiera suena), ataque instantáneo y sin decay — una nota que se
// sostiene, que es lo que hace falta para medir bordes.
void patch(std::vector<AytherAudioWrite>& v, uint32_t cyc, int ch) {
    for (int op = 0; op < 4; ++op) {
        const uint8_t o = uint8_t(op * 4 + ch);
        fm(v, cyc, uint8_t(0x30 + o), 0x01);   // DT=0 MUL=1
        fm(v, cyc, uint8_t(0x40 + o), 0x00);   // TL=0 → nivel máximo
        fm(v, cyc, uint8_t(0x50 + o), 0x1F);   // AR=31 → ataque instantáneo
        fm(v, cyc, uint8_t(0x60 + o), 0x00);   // D1R=0 → sin decay
        fm(v, cyc, uint8_t(0x70 + o), 0x00);   // D2R=0
        fm(v, cyc, uint8_t(0x80 + o), 0x0F);   // D1L=0 RR=15
        fm(v, cyc, uint8_t(0x90 + o), 0x00);   // SSG-EG apagado
    }
    fm(v, cyc, uint8_t(0xB0 + ch), 0x07);      // FB=0 ALG=7
    fm(v, cyc, uint8_t(0xB4 + ch), 0xC0);      // sale por L y por R
    fm(v, cyc, uint8_t(0xA4 + ch), 0x22);      // block=4, fnum alto
    fm(v, cyc, uint8_t(0xA0 + ch), 0x69);      // fnum bajo
}

void key(std::vector<AytherAudioWrite>& v, uint32_t cyc, int ch, bool on) {
    fm(v, cyc, 0x28, uint8_t((on ? 0xF0 : 0x00) | ch));
}

/// Silencia UN canal de FM y deja el resto en copia.
class SilenceOne final : public ayther::VoicePolicy {
public:
    explicit SilenceOne(int ch) : ch_(ch) {}
    ayther::IVoiceSource* choose(const ayther::VoiceContext& c) override {
        if (c.chip == 0 && c.channel == ch_) return &silent_;
        return nullptr;                        // nullptr = copia
    }
private:
    int                  ch_;
    ayther::SilentSource silent_;
};

double rms(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double a = 0.0;
    for (float x : v) a += double(x) * x;
    return std::sqrt(a / double(v.size()));
}

constexpr int      kFrames      = 8;
constexpr int      kSilenced    = 0;    // el canal que se saca
constexpr int      kKeeper      = 1;    // el que TIENE que sobrevivir
constexpr uint32_t kKeyOnFrame  = 2;
constexpr uint32_t kKeyOnCycle  = 400000;   // a mitad de frame (de 896040)
constexpr uint32_t kMCyclesFrame = 3420 * 262;

}  // namespace

int main() {
    std::fprintf(stdout,
        "voice_router_smoke — silenciar una voz la saca entera, en su borde,\n"
        "                     y no toca a las de al lado (#327 Fase 2)\n\n");

    // -- El log: dos canales sonando, el segundo entra tarde ----------------
    std::vector<std::vector<AytherAudioWrite>> log(kFrames);
    patch(log[0], 100, kSilenced);
    patch(log[0], 200, kKeeper);
    key  (log[0], 1000, kKeeper, true);              // el que sobrevive arranca ya
    key  (log[kKeyOnFrame], kKeyOnCycle, kSilenced, true);

    // -- Corrida A: todo copia (sin política) --------------------------------
    ayther::ChannelRouter ra;
    std::vector<std::vector<float>> outA(kFrames), chA(kFrames);
    for (int f = 0; f < kFrames; ++f) {
        std::vector<float> o;
        ra.tick(log[f].data(), uint32_t(log[f].size()), uint32_t(f), o);
        outA[f] = o;
        const float* c = ra.mirror().channel(kSilenced);
        if (c) chA[f].assign(c, c + ra.mirror().frame_samples() * 2);
    }

    // -- Corrida B: el canal kSilenced, mudo ---------------------------------
    SilenceOne pol(kSilenced);
    ayther::ChannelRouter rb;
    rb.set_policy(&pol);
    std::vector<std::vector<float>> outB(kFrames);
    for (int f = 0; f < kFrames; ++f) {
        std::vector<float> o;
        rb.tick(log[f].data(), uint32_t(log[f].size()), uint32_t(f), o);
        outB[f] = o;
    }

    // -- 1. La síntesis suena ------------------------------------------------
    std::vector<float> allA;
    for (auto& v : outA) allA.insert(allA.end(), v.begin(), v.end());
    check(rms(allA) > 1e-3, "la corrida de copia SUENA (si no, no hay nada que medir)");

    // -- 2. Antes del key-on, las dos corridas son IDÉNTICAS -----------------
    // No «parecidas»: idénticas bit a bit. Un canal que todavía no fue tomado
    // no puede diferir en nada.
    bool same_before = true;
    for (uint32_t f = 0; f < kKeyOnFrame; ++f) {
        if (outA[f].size() != outB[f].size()) { same_before = false; break; }
        for (size_t i = 0; i < outA[f].size(); ++i)
            if (outA[f][i] != outB[f][i]) { same_before = false; break; }
    }
    check(same_before, "antes del key-on, copia y silencio son bit-idénticos");

    // -- 3. El canal de al lado SOBREVIVE ------------------------------------
    // Ésta es la propiedad que hizo descartar «mutear el canal entero» en #280:
    // «Espadazo de Tyris» comparte canal con Melodía y no se puede perder.
    std::vector<float> allB;
    for (auto& v : outB) allB.insert(allB.end(), v.begin(), v.end());
    check(rms(allB) > 1e-3, "con una voz muda, la otra SIGUE SONANDO");

    // -- 4. Lo que se sacó es EXACTAMENTE ese canal, ni más ni menos ---------
    double max_err = 0.0;
    for (int f = 0; f < kFrames; ++f) {
        if (outA[f].size() != outB[f].size() || chA[f].size() != outA[f].size()) {
            max_err = 1.0; break;
        }
        for (size_t i = 0; i < outA[f].size(); ++i) {
            // Antes del key-on el canal está en copia en las DOS corridas, así
            // que ahí la diferencia tiene que ser 0, no chA.
            const bool taken = (f > int(kKeyOnFrame)) ||
                               (f == int(kKeyOnFrame) &&
                                i >= size_t(uint64_t(kKeyOnCycle) * outA[f].size()
                                            / kMCyclesFrame));
            const double want = taken ? double(chA[f][i]) : 0.0;
            max_err = std::max(max_err, std::fabs(double(outA[f][i] - outB[f][i]) - want));
        }
    }
    // El umbral es épsilon de float, no una tolerancia de audio: la suma no es
    // asociativa, así que (ch0+ch1)−ch1 no da ch0 al último bit. 1e-6 son unos
    // −120 dBFS — dos órdenes de magnitud por debajo del residuo de −66 dBFS
    // que dejaba el modelo viejo con TODO silenciado.
    check(max_err < 1e-6,
          "la diferencia es EXACTAMENTE el canal silenciado (a épsilon de float)");
    std::fprintf(stdout, "         error máximo: %.3e  (épsilon de float ≈ 1,2e-7)\n", max_err);

    // -- 5. El corte cae en el key-on, no en el borde del frame --------------
    // Es la diferencia con el modelo viejo: ahí el mute llegaba con la
    // granularidad del frame (16,7 ms) y el ataque de la nota se colaba.
    size_t first_diff = SIZE_MAX;
    for (size_t i = 0; i < outA[kKeyOnFrame].size(); ++i)
        if (outA[kKeyOnFrame][i] != outB[kKeyOnFrame][i]) { first_diff = i; break; }
    const size_t expect = size_t(uint64_t(kKeyOnCycle) * outA[kKeyOnFrame].size()
                                 / kMCyclesFrame);
    // El key-on cae a mitad de frame: si el corte fuese por frame, first_diff
    // sería 0. Se tolera un par de muestras por el redondeo del prorrateo.
    const bool at_keyon = first_diff != SIZE_MAX &&
                          first_diff + 4 >= expect && first_diff <= expect + 4;
    check(at_keyon, "el corte cae en el KEY-ON, no en el borde del frame");
    std::fprintf(stdout, "         primera diferencia en la muestra %zu (esperada ~%zu)\n",
                 first_diff == SIZE_MAX ? 0 : first_diff / 2, expect / 2);

    // -- 6. Un canal que no suena da CERO EXACTO -----------------------------
    // El bloqueador de continua tiene que arrancar cebado: si no, el escalón de
    // dac_discontinuity(0) entra como un transitorio decayente en cada canal
    // mudo — un clic en el arranque. Cero exacto es la prueba de que no queda.
    ayther::ChannelRouter quiet;
    std::vector<float> qo;
    std::vector<AytherAudioWrite> none;
    double qmax = 0.0;
    for (int f = 0; f < 4; ++f) {
        quiet.tick(none.data(), 0, uint32_t(f), qo);
        for (float x : qo) qmax = std::max(qmax, std::fabs(double(x)));
    }
    check(qmax == 0.0, "sin una sola nota, la salida es SILENCIO DIGITAL (cero exacto)");
    if (qmax != 0.0) std::fprintf(stdout, "         residuo: %.9f\n", qmax);

    const ayther::ChannelRouter::Stats& st = rb.stats();
    std::fprintf(stdout,
        "\n  telemetría: ticks=%llu key_on=%llu key_off=%llu sustituidas=%llu\n",
        (unsigned long long)st.ticks, (unsigned long long)st.key_ons,
        (unsigned long long)st.key_offs, (unsigned long long)st.substituted);
    check(st.key_ons == 2, "se detectaron los DOS key-on del log");
    check(st.substituted == 1, "exactamente UNA voz fue sustituida");

    std::fprintf(stdout, "\n%s\n", g_fail ? "=== FALLÓ ===" : "=== OK ===");
    return g_fail ? 1 : 0;
}

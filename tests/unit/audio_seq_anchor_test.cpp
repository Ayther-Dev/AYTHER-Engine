// ---------------------------------------------------------------------------
// audio_seq_anchor_test — anclas de Secuencia con RECLAMO entre Secuencias
// (#511).
//
// POR QUÉ EXISTE. Dos Secuencias que comparten firmas se disparaban las dos a
// la vez: «The Battle - Intro» (Golden Axe) abre con un hi-hat que reaparece
// cada 63 frames dentro de «The Battle - Loop», y el bajo que abre el Loop
// aparece dentro de la Intro. Cada sub anclaba sola sobre su disparador, ciega
// a las demás. La regla nueva: una ocurrencia del disparador de S que cae
// dentro de la ventana (con HD) de otra T que la tiene como miembro es de T —
// «la que se estaba escuchando gana»; en un empate de frame gana la más
// específica. Fija también que la segmentación greedy de siempre (reporte
// 2026-07-23) no cambió.
// ---------------------------------------------------------------------------
#include "audio_seq_anchor.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

struct Ev { uint64_t sig; uint32_t start; };

std::unordered_map<uint64_t, std::vector<uint32_t>>
table(const std::vector<Ev>& ev, const std::vector<ayther::SeqAnchorSub>& subs) {
    return ayther::seq_anchor_table(
        ev.size(), [&](size_t i) { return ev[i].sig; },
        [&](size_t i) { return ev[i].start; }, subs);
}

std::string fmt(const std::vector<uint32_t>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s + "]";
}

}  // namespace

int main() {
    std::printf("=== audio_seq_anchor_test — reclamo entre Secuencias (#511) ===\n");
    using ayther::SeqAnchorSub;
    constexpr uint64_t kHat = 0x9303, kBass = 0x6946, kOther = 0xaaaa;

    // ---- Segmentación greedy de siempre: una sola Secuencia ---------------
    {
        SeqAnchorSub m; m.key = 1; m.trigger_signature = kHat;
        m.duration_frames = 100; m.span_frames = 60; m.signatures = {kHat, kOther};
        // ocurrencias del disparador en 0, 30 (interna), 60 (repetición real), 200
        const auto t = table({{kHat, 0}, {kHat, 30}, {kOther, 40}, {kHat, 60}, {kHat, 200}}, {m});
        check(t.at(1) == std::vector<uint32_t>{0, 60, 200},
              "greedy: la ocurrencia interna (30) no re-ancla; la repetición tras el paso sí → " + fmt(t.at(1)));
        SeqAnchorSub off = m; off.enabled = false;
        check(table({{kHat, 0}}, {off}).at(1).empty(), "sub sin asset: no ancla");
    }

    // ---- El caso Intro/Loop, reducido ---------------------------------------
    // Intro: abre con kHat en 1, dura 100 (HD); tiene kBass como miembro.
    // Loop: abre con kBass en 101, dura 50, paso 50; tiene kHat como miembro.
    // La toma: Intro 1..100 con kBass en 40 (miembro de la Intro); después el
    // Loop repite cada 50 frames y trae kHat en 101, 131, 151...
    {
        SeqAnchorSub intro; intro.key = 10; intro.trigger_signature = kHat;
        intro.duration_frames = 100; intro.span_frames = 100;
        intro.signatures = {kHat, kBass, kOther, 0xb, 0xc};   // 5 firmas: menos específica
        SeqAnchorSub loop; loop.key = 11; loop.trigger_signature = kBass;
        loop.duration_frames = 50; loop.span_frames = 50;
        loop.signatures = {kBass, kHat, kOther};               // 3 firmas: más específica
        const std::vector<Ev> ev = {
            {kHat, 1}, {kBass, 40}, {kHat, 64},
            {kBass, 101}, {kHat, 101}, {kHat, 131},
            {kBass, 151}, {kHat, 151},
            {kBass, 201}, {kHat, 201},
        };
        const auto t = table(ev, {intro, loop});
        check(t.at(10) == std::vector<uint32_t>{1},
              "Intro ancla en 1 y NUNCA más: sus hi-hats de 101/131/151/201 son del Loop → " + fmt(t.at(10)));
        check(t.at(11) == std::vector<uint32_t>{101, 151, 201},
              "Loop: el bajo de 40 es de la Intro (reclamado); ancla en 101 y en cada pasada → " + fmt(t.at(11)));
        // Mismo resultado con las subs en el otro orden (determinismo).
        const auto t2 = table(ev, {loop, intro});
        check(t2.at(10) == t.at(10) && t2.at(11) == t.at(11), "el orden de las subs no cambia las anclas");
    }

    // ---- Empate de frame sin reclamo previo: gana la más específica ---------
    {
        SeqAnchorSub a; a.key = 20; a.trigger_signature = kHat;  a.duration_frames = 50; a.signatures = {kHat, kBass, kOther};
        SeqAnchorSub b; b.key = 21; b.trigger_signature = kBass; b.duration_frames = 50; b.signatures = {kBass, kHat};
        const auto t = table({{kHat, 10}, {kBass, 10}}, {a, b});
        check(t.at(21) == std::vector<uint32_t>{10} && t.at(20).empty(),
              "empate de frame: la de menos firmas (b) abre y reclama a la otra");
        SeqAnchorSub c = a; c.key = 19; c.signatures = {kHat, kBass};   // mismas 2 firmas que b
        const auto t3 = table({{kHat, 10}, {kBass, 10}}, {c, b});
        check(t3.at(19) == std::vector<uint32_t>{10} && t3.at(21).empty(),
              "empate total: desempata el id menor");
    }

    // ---- Sin membresía cruzada no hay reclamo ------------------------------
    {
        SeqAnchorSub a; a.key = 30; a.trigger_signature = kHat;  a.duration_frames = 100; a.signatures = {kHat};
        SeqAnchorSub b; b.key = 31; b.trigger_signature = kBass; b.duration_frames = 100; b.signatures = {kBass};
        const auto t = table({{kHat, 0}, {kBass, 10}}, {a, b});
        check(t.at(30) == std::vector<uint32_t>{0} && t.at(31) == std::vector<uint32_t>{10},
              "dos Secuencias ajenas entre sí (un espadazo dentro de la melodía) anclan las dos");
    }

    // ---- El reclamo vence con la ventana ------------------------------------
    {
        SeqAnchorSub a; a.key = 40; a.trigger_signature = kHat;  a.duration_frames = 20; a.signatures = {kHat, kBass};
        SeqAnchorSub b; b.key = 41; b.trigger_signature = kBass; b.duration_frames = 20; b.signatures = {kBass};
        const auto t = table({{kHat, 0}, {kBass, 10}, {kBass, 20}}, {a, b});
        check(t.at(41) == std::vector<uint32_t>{20},
              "el bajo de 10 (dentro de la ventana 0..19) es reclamado; el de 20 (justo al vencer) ancla");
    }

    // ---- Eventos DESORDENADOS (el detector emite por canal) ----------------
    {
        SeqAnchorSub intro; intro.key = 50; intro.trigger_signature = kHat;  intro.duration_frames = 100; intro.signatures = {kHat, kBass, kOther, 0xb};
        SeqAnchorSub loop;  loop.key  = 51; loop.trigger_signature  = kBass; loop.duration_frames  = 50;  loop.signatures  = {kBass, kHat};
        // en 100 arrancan las dos; el hi-hat viene ANTES en el array (ch5 antes que ch0)
        const auto t = table({{kHat, 0}, {kHat, 100}, {kBass, 100}}, {intro, loop});
        check(t.at(51) == std::vector<uint32_t>{100} && t.at(50) == std::vector<uint32_t>{0},
              "desordenados: el empate del frame 100 se resuelve por prioridad, no por orden del array");
    }

    // ---- CABEZA: el disparador es una variante pero la mayoría arranca ------
    // Loop con cabeza de 6 firmas (bajo + 5). En la 3a pasada el bajo es
    // OTRA firma: con 5/6 de la cabeza el Loop re-ancla igual y la Intro
    // (cuyo hi-hat es miembro del Loop) sigue reclamada.
    {
        const uint64_t h1 = 0x1, h2 = 0x2, h3 = 0x3, h4 = 0x4, bassVar = 0x6947;
        SeqAnchorSub loop; loop.key = 60; loop.trigger_signature = kBass; loop.duration_frames = 50; loop.span_frames = 50;
        loop.looping = true; loop.signatures = {kBass, kHat, h1, h2, h3, h4}; loop.head = {kBass, kHat, h1, h2, h3, h4};
        SeqAnchorSub intro; intro.key = 61; intro.trigger_signature = kHat; intro.duration_frames = 100; intro.span_frames = 100;
        intro.signatures = {kHat, kBass, h1, h2, h3, h4, kOther, 0xb, 0xc}; intro.head = {kHat};
        std::vector<Ev> ev;
        auto pass = [&](uint32_t f, uint64_t bass) {
            ev.push_back({kHat, f}); ev.push_back({h1, f}); ev.push_back({h2, f});
            ev.push_back({h3, f}); ev.push_back({h4, f}); ev.push_back({bass, f});
        };
        ev.push_back({kHat, 0});   // la Intro arranca sola en 0
        pass(100, kBass); pass(150, kBass); pass(200, bassVar); pass(250, kBass);
        const auto t = table(ev, {loop, intro});
        check(t.at(60) == std::vector<uint32_t>{100, 150, 200, 250},
              "cabeza: en 200 falta el disparador pero 5/6 arrancan -> el Loop re-ancla -> " + fmt(t.at(60)));
        check(t.at(61) == std::vector<uint32_t>{0},
              "...y la Intro no se cuela en 200 (reclamada) -> " + fmt(t.at(61)));
        check(seq_head_quorum(intro) == 0 && seq_head_quorum(loop) == 3,
              "quorum: cabeza de 1 = solo el disparador; de 6 = 3");
        // Solo 2 de 6 en 300: no alcanza, el Loop termina.
        ev.push_back({h1, 300}); ev.push_back({h2, 300});
        const auto t2 = table(ev, {loop, intro});
        check(t2.at(60) == std::vector<uint32_t>{100, 150, 200, 250},
              "cabeza sin quorum (2/6): los eventos cambiaron, el Loop no re-ancla");
    }

    // ---- CONTINUACION: la que viene sonando en loop gana el empate ----------
    {
        SeqAnchorSub a; a.key = 70; a.trigger_signature = kHat;  a.duration_frames = 50; a.span_frames = 50; a.looping = true;  a.signatures = {kHat, kBass, kOther};
        SeqAnchorSub b; b.key = 71; b.trigger_signature = kBass; b.duration_frames = 50; b.span_frames = 50; b.looping = false; b.signatures = {kBass, kHat};
        // a suena desde 0; en 50 vence su ventana y arrancan las dos: a (en loop) continua aunque b sea mas especifica
        const auto t = table({{kHat, 0}, {kHat, 50}, {kBass, 50}}, {a, b});
        check(t.at(70) == std::vector<uint32_t>{0, 50} && t.at(71).empty(),
              "continuacion: la que venia sonando en loop sigue en el empate");
        // Sin loop no hay continuacion: en 50 gana la especifica (b).
        SeqAnchorSub a2 = a; a2.looping = false;
        const auto t2 = table({{kHat, 0}, {kHat, 50}, {kBass, 50}}, {a2, b});
        check(t2.at(70) == std::vector<uint32_t>{0} && t2.at(71) == std::vector<uint32_t>{50},
              "sin loop la que termino no continua: gana la mas especifica");
    }

    std::printf("%s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}

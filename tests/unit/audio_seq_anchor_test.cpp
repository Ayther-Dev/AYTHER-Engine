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
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const std::string &what) {
  std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
  if (!ok)
    ++g_fail;
}

struct Ev {
  uint64_t sig;
  uint32_t start;
};

std::unordered_map<uint64_t, std::vector<uint32_t>>
table(const std::vector<Ev> &ev,
      const std::vector<ayther::SeqAnchorSub> &subs) {
  return ayther::seq_anchor_table(
      ev.size(), [&](size_t i) { return ev[i].sig; },
      [&](size_t i) { return ev[i].start; }, subs);
}

std::string fmt(const std::vector<uint32_t> &v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i)
    s += (i ? "," : "") + std::to_string(v[i]);
  return s + "]";
}

} // namespace

int main() {
  std::printf(
      "=== audio_seq_anchor_test — reclamo entre Secuencias (#511) ===\n");
  using ayther::SeqAnchorSub;
  constexpr uint64_t kHat = 0x9303, kBass = 0x6946, kOther = 0xaaaa;

  // ---- Segmentación greedy de siempre: una sola Secuencia ---------------
  {
    SeqAnchorSub m;
    m.key = 1;
    m.trigger_signature = kHat;
    m.duration_frames = 100;
    m.span_frames = 60;
    m.signatures = {kHat, kOther};
    // ocurrencias del disparador en 0, 30 (interna), 60 (repetición real), 200
    const auto t = table(
        {{kHat, 0}, {kHat, 30}, {kOther, 40}, {kHat, 60}, {kHat, 200}}, {m});
    check(t.at(1) == std::vector<uint32_t>{0, 60, 200},
          "greedy: la ocurrencia interna (30) no re-ancla; la repetición tras "
          "el paso sí → " +
              fmt(t.at(1)));
    SeqAnchorSub off = m;
    off.enabled = false;
    check(table({{kHat, 0}}, {off}).at(1).empty(), "sub sin asset: no ancla");
  }

  // ---- El paso NO es la ventana: la cola de la última nota no puede
  // comerse la pasada siguiente (reporte 2026-09-14, «OST - Wilderness»).
  //
  // La frase dura 853 frames, pero UNA nota sostenida resuena 4 frames más
  // allá del punto de loop. Con el paso sacado de ese máximo (857) la ventana
  // [90, 947) se traga el anclaje verdadero de f943 —una ocurrencia real del
  // disparador— y la pasada aparecía 106 frames después, en un frame donde la
  // frase no empieza. Con el paso medido por los ARRANQUES (853) ancla donde
  // suena, aunque la ventana anterior siga abierta.
  {
    const std::vector<Ev> pasadas = {
        {kHat, 90},  {kHat, 423}, {kHat, 516}, {kHat, 836}, // internas
        {kHat, 943},                                        // la pasada REAL
    };
    SeqAnchorSub m;
    m.key = 2;
    m.trigger_signature = kHat;
    m.duration_frames = 857; // ventana: hasta que la cola deja de sonar
    m.span_frames = 853;     // paso: hasta el fin de lo que arranca último
    m.signatures = {kHat};
    const auto t = table(pasadas, {m});
    check(t.at(2) == std::vector<uint32_t>{90, 943},
          "paso < ventana: la pasada siguiente ancla con la ventana todavía "
          "abierta → " +
              fmt(t.at(2)));

    // El defecto, para que la prueba MUESTRE lo que cuida.
    SeqAnchorSub viejo = m;
    viejo.span_frames = 857;
    check(table(pasadas, {viejo}).at(2) == std::vector<uint32_t>{90},
          "con el paso pegado a la ventana, la pasada de f943 se pierde (el "
          "defecto)");

    // El contrato de la AUSENCIA: 0 = segmentar por la ventana, que es lo
    // que dice todo pack horneado antes de que la clave existiera.
    SeqAnchorSub sin_paso = m;
    sin_paso.span_frames = 0;
    check(table(pasadas, {sin_paso}).at(2) == std::vector<uint32_t>{90},
          "paso 0 = segmentar por la ventana (packs anteriores a la clave)");
  }

  // ---- El caso Intro/Loop, reducido ---------------------------------------
  // Intro: abre con kHat en 1, dura 100 (HD); tiene kBass como miembro.
  // Loop: abre con kBass en 101, dura 50, paso 50; tiene kHat como miembro.
  // La toma: Intro 1..100 con kBass en 40 (miembro de la Intro); después el
  // Loop repite cada 50 frames y trae kHat en 101, 131, 151...
  {
    SeqAnchorSub intro;
    intro.key = 10;
    intro.trigger_signature = kHat;
    intro.duration_frames = 100;
    intro.span_frames = 100;
    intro.signatures = {kHat, kBass, kOther, 0xb,
                        0xc}; // 5 firmas: menos específica
    SeqAnchorSub loop;
    loop.key = 11;
    loop.trigger_signature = kBass;
    loop.duration_frames = 50;
    loop.span_frames = 50;
    loop.signatures = {kBass, kHat, kOther}; // 3 firmas: más específica
    const std::vector<Ev> ev = {
        {kHat, 1},   {kBass, 40},  {kHat, 64},  {kBass, 101}, {kHat, 101},
        {kHat, 131}, {kBass, 151}, {kHat, 151}, {kBass, 201}, {kHat, 201},
    };
    const auto t = table(ev, {intro, loop});
    check(t.at(10) == std::vector<uint32_t>{1},
          "Intro ancla en 1 y NUNCA más: sus hi-hats de 101/131/151/201 son "
          "del Loop → " +
              fmt(t.at(10)));
    check(t.at(11) == std::vector<uint32_t>{101, 151, 201},
          "Loop: el bajo de 40 es de la Intro (reclamado); ancla en 101 y en "
          "cada pasada → " +
              fmt(t.at(11)));
    // Mismo resultado con las subs en el otro orden (determinismo).
    const auto t2 = table(ev, {loop, intro});
    check(t2.at(10) == t.at(10) && t2.at(11) == t.at(11),
          "el orden de las subs no cambia las anclas");
  }

  // ---- Empate intro→loop: el que RECLAMA gana, no el más específico -------
  // Golden Axe 2026-10-14: el hi-hat de la Intro es una de las seis cabezas
  // del Loop. En el límite intro→loop disparan los dos; la Intro tiene MENOS
  // miembros (22 vs 28) y por «específica» anclaba primero y reemplazaba al
  // Loop recién empezado. El Loop lista la firma de la Intro; la Intro no
  // lista la del Loop → el Loop ancla y su ventana reclama a la Intro.
  {
    SeqAnchorSub intro;
    intro.key = 20;
    intro.trigger_signature = kHat;
    intro.duration_frames = 100;
    intro.span_frames = 100;
    intro.signatures = {kHat, 0xb}; // 2 firmas: MÁS específica que el Loop
    SeqAnchorSub loop;
    loop.key = 21;
    loop.trigger_signature = kBass;
    loop.duration_frames = 51; // el HD dura un frame más que el paso
    loop.span_frames = 50;
    loop.looping = true;
    loop.signatures = {kBass, kHat, kOther, 0xc, 0xd};
    loop.head = {kBass, kHat};
    const std::vector<Ev> ev = {
        {kHat, 1},    {kHat, 64},  {kBass, 101}, {kHat, 101},
        {kBass, 151}, {kHat, 151}, {kBass, 201}, {kHat, 201},
    };
    const auto t = table(ev, {intro, loop});
    check(t.at(20) == std::vector<uint32_t>{1},
          "Intro ancla en 1 y nunca en el límite del Loop → " + fmt(t.at(20)));
    check(t.at(21) == std::vector<uint32_t>{101, 151, 201},
          "Loop ancla en 101 (reclama el hi-hat de la Intro) y continúa en "
          "151 y 201 aunque su ventana (51) dure más que el paso (50) → " +
              fmt(t.at(21)));
    const auto t2 = table(ev, {loop, intro});
    check(t2.at(20) == t.at(20) && t2.at(21) == t.at(21),
          "reclamo en el empate: el orden de las subs no cambia las anclas");
  }

  // ---- REINICIO dentro del paso: la apertura completa re-ancla -----------
  // El juego vuelve al inicio de la música (cambio de pantalla) a mitad de la
  // pasada: disparador + quórum de cabeza juntos. Una ocurrencia suelta del
  // disparador dentro del paso sigue siendo interna.
  {
    SeqAnchorSub loop;
    loop.key = 30;
    loop.trigger_signature = kBass;
    loop.duration_frames = 50;
    loop.span_frames = 50;
    loop.looping = true;
    loop.signatures = {kBass, kHat, kOther};
    loop.head = {kBass, kHat};
    const std::vector<Ev> ev = {
        {kBass, 101}, {kHat, 101}, // ancla
        {kBass, 120},              // disparador suelto: interno
        {kBass, 130}, {kHat, 130}, // apertura completa: REINICIO
        {kBass, 180}, {kHat, 180}, // paso nuevo desde 130
    };
    const auto t = table(ev, {loop});
    check(t.at(30) == std::vector<uint32_t>{101, 130, 180},
          "reinicio: 120 interno, 130 re-ancla y el paso sigue desde ahí → " +
              fmt(t.at(30)));
    SeqAnchorSub headless = loop;
    headless.key = 31;
    headless.head.clear(); // sin cabeza no hay quórum: nunca reinicia
    const auto t2 = table(ev, {headless});
    check(t2.at(31) == std::vector<uint32_t>{101, 180},
          "sin cabeza: 130 es interno; re-ancla sólo cuando llega el paso → " +
              fmt(t2.at(31)));
  }

  // ---- REINICIO: la apertura es TODA la cabeza, sin nada sostenido --------
  // Wilderness (Golden Axe, 2026-10-14): tres de sus cinco notas de apertura
  // vuelven cada pocos compases; con quórum, cada vuelta cortaba el HD y lo
  // arrancaba de cero.
  {
    constexpr uint64_t kPad = 0xbbbb, kLead = 0xcccc, kForeign = 0xdddd;
    SeqAnchorSub loop;
    loop.key = 32;
    loop.trigger_signature = kBass;
    loop.duration_frames = 300;
    loop.span_frames = 300;
    loop.looping = true;
    loop.signatures = {kBass, kHat, kPad, kLead, kOther};
    loop.head = {kBass, kHat, kPad, kLead};
    const std::vector<Ev> ev = {
        {kBass, 100}, {kHat, 100}, {kPad, 100}, {kLead, 100},  // ancla
        {kBass, 160}, {kHat, 160}, {kPad, 160}, {kOther, 160}, // 3 de 4: compás
        {kBass, 220}, {kHat, 220}, {kPad, 220}, {kLead, 220},  // apertura
    };
    const auto t = table(ev, {loop});
    check(t.at(32) == std::vector<uint32_t>{100, 220},
          "reinicio: tres de cuatro cabezas es un compás; la cabeza entera "
          "re-ancla → " +
              fmt(t.at(32)));
    // En vivo: una firma de la Secuencia que ya sonaba el frame anterior
    // desmiente la apertura aunque la cabeza entera arranque.
    const std::vector<SeqAnchorSub> subs{loop};
    const std::vector<uint64_t> opening{kBass, kHat, kPad, kLead};
    const auto decide = [&](const std::vector<uint64_t> &sustained,
                            std::vector<ayther::SeqAnchorState> &state) {
      return ayther::seq_anchor_frame_decided(
          220, opening, sustained, subs, state,
          [](const ayther::SeqAnchorCandidateView &) noexcept {},
          [](const ayther::SeqAnchorDecisionView &) noexcept {},
          [](const ayther::SeqAnchorResolutionView &) noexcept {});
    };
    std::vector<ayther::SeqAnchorState> held(1);
    held[0].next_free = 400;
    held[0].open = true;
    held[0].win_start = 100;
    held[0].win_end = 400;
    std::vector<ayther::SeqAnchorState> foreign = held;
    check(decide({kOther}, held).empty() && held[0].next_free == 400,
          "reinicio: un miembro sostenido desde antes lo desmiente");
    check(decide({kForeign}, foreign) == std::vector<size_t>{0} &&
              foreign[0].next_free == 520,
          "reinicio: una voz ajena sostenida no cuenta; la cabeza entera "
          "re-ancla y el paso sigue desde ahí");
  }

  // ---- Empate de frame sin reclamo previo: gana la más específica ---------
  {
    SeqAnchorSub a;
    a.key = 20;
    a.trigger_signature = kHat;
    a.duration_frames = 50;
    a.signatures = {kHat, kBass, kOther};
    SeqAnchorSub b;
    b.key = 21;
    b.trigger_signature = kBass;
    b.duration_frames = 50;
    b.signatures = {kBass, kHat};
    const auto t = table({{kHat, 10}, {kBass, 10}}, {a, b});
    check(t.at(21) == std::vector<uint32_t>{10} && t.at(20).empty(),
          "empate de frame: la de menos firmas (b) abre y reclama a la otra");
    SeqAnchorSub c = a;
    c.key = 19;
    c.signatures = {kHat, kBass}; // mismas 2 firmas que b
    const auto t3 = table({{kHat, 10}, {kBass, 10}}, {c, b});
    check(t3.at(19) == std::vector<uint32_t>{10} && t3.at(21).empty(),
          "empate total: desempata el id menor");
  }

  // ---- Sin membresía cruzada no hay reclamo ------------------------------
  {
    SeqAnchorSub a;
    a.key = 30;
    a.trigger_signature = kHat;
    a.duration_frames = 100;
    a.signatures = {kHat};
    SeqAnchorSub b;
    b.key = 31;
    b.trigger_signature = kBass;
    b.duration_frames = 100;
    b.signatures = {kBass};
    const auto t = table({{kHat, 0}, {kBass, 10}}, {a, b});
    check(t.at(30) == std::vector<uint32_t>{0} &&
              t.at(31) == std::vector<uint32_t>{10},
          "dos Secuencias ajenas entre sí (un espadazo dentro de la melodía) "
          "anclan las dos");
  }

  // ---- El reclamo vence con la ventana ------------------------------------
  {
    SeqAnchorSub a;
    a.key = 40;
    a.trigger_signature = kHat;
    a.duration_frames = 20;
    a.signatures = {kHat, kBass};
    SeqAnchorSub b;
    b.key = 41;
    b.trigger_signature = kBass;
    b.duration_frames = 20;
    b.signatures = {kBass};
    const auto t = table({{kHat, 0}, {kBass, 10}, {kBass, 20}}, {a, b});
    check(t.at(41) == std::vector<uint32_t>{20},
          "el bajo de 10 (dentro de la ventana 0..19) es reclamado; el de 20 "
          "(justo al vencer) ancla");
  }

  // ---- Eventos DESORDENADOS (el detector emite por canal) ----------------
  {
    SeqAnchorSub intro;
    intro.key = 50;
    intro.trigger_signature = kHat;
    intro.duration_frames = 100;
    intro.signatures = {kHat, kBass, kOther, 0xb};
    SeqAnchorSub loop;
    loop.key = 51;
    loop.trigger_signature = kBass;
    loop.duration_frames = 50;
    loop.signatures = {kBass, kHat};
    // en 100 arrancan las dos; el hi-hat viene ANTES en el array (ch5 antes que
    // ch0)
    const auto t = table({{kHat, 0}, {kHat, 100}, {kBass, 100}}, {intro, loop});
    check(t.at(51) == std::vector<uint32_t>{100} &&
              t.at(50) == std::vector<uint32_t>{0},
          "desordenados: el empate del frame 100 se resuelve por prioridad, no "
          "por orden del array");
  }

  // ---- CABEZA: el disparador es una variante pero la mayoría arranca ------
  // Loop con cabeza de 6 firmas (bajo + 5). En la 3a pasada el bajo es
  // OTRA firma: con 5/6 de la cabeza el Loop re-ancla igual y la Intro
  // (cuyo hi-hat es miembro del Loop) sigue reclamada.
  {
    const uint64_t h1 = 0x1, h2 = 0x2, h3 = 0x3, h4 = 0x4, bassVar = 0x6947;
    SeqAnchorSub loop;
    loop.key = 60;
    loop.trigger_signature = kBass;
    loop.duration_frames = 50;
    loop.span_frames = 50;
    loop.looping = true;
    loop.signatures = {kBass, kHat, h1, h2, h3, h4};
    loop.head = {kBass, kHat, h1, h2, h3, h4};
    SeqAnchorSub intro;
    intro.key = 61;
    intro.trigger_signature = kHat;
    intro.duration_frames = 100;
    intro.span_frames = 100;
    intro.signatures = {kHat, kBass, h1, h2, h3, h4, kOther, 0xb, 0xc};
    intro.head = {kHat};
    std::vector<Ev> ev;
    auto pass = [&](uint32_t f, uint64_t bass) {
      ev.push_back({kHat, f});
      ev.push_back({h1, f});
      ev.push_back({h2, f});
      ev.push_back({h3, f});
      ev.push_back({h4, f});
      ev.push_back({bass, f});
    };
    ev.push_back({kHat, 0}); // la Intro arranca sola en 0
    pass(100, kBass);
    pass(150, kBass);
    pass(200, bassVar);
    pass(250, kBass);
    const auto t = table(ev, {loop, intro});
    check(t.at(60) == std::vector<uint32_t>{100, 150, 200, 250},
          "cabeza: en 200 falta el disparador pero 5/6 arrancan -> el Loop "
          "re-ancla -> " +
              fmt(t.at(60)));
    check(t.at(61) == std::vector<uint32_t>{0},
          "...y la Intro no se cuela en 200 (reclamada) -> " + fmt(t.at(61)));
    check(seq_head_quorum(intro) == 0 && seq_head_quorum(loop) == 3,
          "quorum: cabeza de 1 = solo el disparador; de 6 = 3");
    // Solo 2 de 6 en 300: no alcanza, el Loop termina.
    ev.push_back({h1, 300});
    ev.push_back({h2, 300});
    const auto t2 = table(ev, {loop, intro});
    check(
        t2.at(60) == std::vector<uint32_t>{100, 150, 200, 250},
        "cabeza sin quorum (2/6): los eventos cambiaron, el Loop no re-ancla");
  }

  // ---- CONTINUACION: la que viene sonando en loop gana el empate ----------
  {
    SeqAnchorSub a;
    a.key = 70;
    a.trigger_signature = kHat;
    a.duration_frames = 50;
    a.span_frames = 50;
    a.looping = true;
    a.signatures = {kHat, kBass, kOther};
    SeqAnchorSub b;
    b.key = 71;
    b.trigger_signature = kBass;
    b.duration_frames = 50;
    b.span_frames = 50;
    b.looping = false;
    b.signatures = {kBass, kHat};
    // a suena desde 0; en 50 vence su ventana y arrancan las dos: a (en loop)
    // continua aunque b sea mas especifica
    const auto t = table({{kHat, 0}, {kHat, 50}, {kBass, 50}}, {a, b});
    check(t.at(70) == std::vector<uint32_t>{0, 50} && t.at(71).empty(),
          "continuacion: la que venia sonando en loop sigue en el empate");
    // Sin loop no hay continuacion: en 50 gana la especifica (b).
    SeqAnchorSub a2 = a;
    a2.looping = false;
    const auto t2 = table({{kHat, 0}, {kHat, 50}, {kBass, 50}}, {a2, b});
    check(t2.at(70) == std::vector<uint32_t>{0} &&
              t2.at(71) == std::vector<uint32_t>{50},
          "sin loop la que termino no continua: gana la mas especifica");
  }

  // ---- RF-17.3: un rechazo no es ausencia de Secuencia ------------------
  // El consumidor necesita distinguir el evento individual válido de las
  // rutas terminales del resolver y conservar su causa; un vector de anclas
  // vacío por sí solo pierde esa información.
  {
    using ayther::SeqTriggerDisposition;
    SeqAnchorSub sq;
    sq.key = 80;
    sq.trigger_signature = kHat;
    sq.duration_frames = 50;
    sq.span_frames = 50;
    sq.signatures = {kHat};
    const std::vector<SeqAnchorSub> subs{sq};
    std::vector<ayther::SeqAnchorState> state(subs.size());
    state[0].next_free = 20;
    ayther::SeqAnchorRoutes routes{subs};
    const auto anchors = ayther::seq_anchor_frame_decided(
        10, {kHat}, subs, state,
        [&](const ayther::SeqAnchorCandidateView &value) noexcept {
          routes(value);
        },
        [&](const ayther::SeqAnchorDecisionView &value) noexcept {
          routes(value);
        },
        [](const ayther::SeqAnchorResolutionView &) noexcept {});
    const auto internal = routes.for_key(80);
    check(anchors.empty() &&
              internal.disposition ==
                  SeqTriggerDisposition::rejected_internal &&
              internal.key == 80 && internal.candidate_signature == kHat &&
              internal.owner_key == 0,
          "resultado explícito: rechazo interno conserva clave y firma causal");
    const auto individual = routes.for_key(999);
    check(
        individual.disposition == SeqTriggerDisposition::individual &&
            individual.key == 999 && individual.owner_key == 0,
        "resultado explícito: ausencia de Secuencia permite evento individual");
  }
  {
    using ayther::SeqTriggerDisposition;
    SeqAnchorSub owner;
    owner.key = 90;
    owner.trigger_signature = kBass;
    owner.duration_frames = 100;
    owner.signatures = {kBass, kHat};
    SeqAnchorSub candidate;
    candidate.key = 91;
    candidate.trigger_signature = kHat;
    candidate.duration_frames = 50;
    candidate.signatures = {kHat};
    const std::vector<SeqAnchorSub> subs{owner, candidate};
    std::vector<ayther::SeqAnchorState> state(subs.size());
    state[0].open = true;
    state[0].win_start = 0;
    state[0].win_end = 100;
    ayther::SeqAnchorRoutes routes{subs};
    const auto anchors = ayther::seq_anchor_frame_decided(
        10, {kHat}, subs, state,
        [&](const ayther::SeqAnchorCandidateView &value) noexcept {
          routes(value);
        },
        [&](const ayther::SeqAnchorDecisionView &value) noexcept {
          routes(value);
        },
        [](const ayther::SeqAnchorResolutionView &) noexcept {});
    const auto claimed = routes.for_key(91);
    check(
        anchors.empty() &&
            claimed.disposition == SeqTriggerDisposition::rejected_claimed &&
            claimed.key == 91 && claimed.owner_key == 90 &&
            claimed.candidate_signature == kHat,
        "resultado explícito: rechazo reclamado conserva propietario y causa");
  }

  // ---- Regla 6: CONTRADICCIÓN por estado de voces (título→selección) -----
  // La canción llega a su loop mientras el título se funde: ancla el Loop en
  // 913. En 968 la selección reinicia la música desde el INTRO: el driver
  // cortó todas las voces en 967 y el único key-on es el hi-hat, disparador
  // del Intro y miembro del Loop. Sin voz viva del Loop y con la apertura de
  // otra Secuencia exclusiva, el Loop se cierra y el Intro ancla en sincronía.
  {
    constexpr uint64_t kBassLoop = 0xbb53, kPadLoop = 0xfd94;
    SeqAnchorSub intro;
    intro.key = 100;
    intro.trigger_signature = kHat;
    intro.duration_frames = 575;
    intro.signatures = {kHat, kOther};
    SeqAnchorSub loop;
    loop.key = 101;
    loop.trigger_signature = kBassLoop;
    loop.duration_frames = 577;
    loop.span_frames = 576;
    loop.looping = true;
    loop.signatures = {kBassLoop, kPadLoop, kHat, kOther};
    loop.head = {kBassLoop, kPadLoop, kHat};
    const std::vector<SeqAnchorSub> subs{intro, loop};
    auto loop_open = [&] {
      std::vector<ayther::SeqAnchorState> state(2);
      state[1].open = true;
      state[1].win_start = 913;
      state[1].win_end = 1490;
      state[1].next_free = 1489;
      return state;
    };
    auto decide_at = [&](const std::vector<uint64_t> &sigs,
                         std::optional<std::vector<uint64_t>> sustained,
                         std::vector<ayther::SeqAnchorState> &state,
                         bool *contradicted, size_t *related) {
      std::optional<std::span<const uint64_t>> vs;
      if (sustained)
        vs = std::span<const uint64_t>{*sustained};
      return ayther::seq_anchor_frame_decided(
          968, sigs, vs, subs, state,
          [](const ayther::SeqAnchorCandidateView &) noexcept {},
          [&](const ayther::SeqAnchorDecisionView &value) noexcept {
            if (value.result ==
                ayther::SeqAnchorDecisionResult::contradicted_open_sequence) {
              if (contradicted)
                *contradicted = value.sub_index == 1 && value.key == 101 &&
                                value.candidate_signature == kHat;
              if (related)
                *related = value.related_sub_index;
            }
          },
          [](const ayther::SeqAnchorResolutionView &) noexcept {});
    };

    // Reinicio real: nada del Loop sostenido, sólo el hi-hat.
    auto st = loop_open();
    bool seen = false;
    size_t related = 99;
    const auto anchors =
        decide_at({kHat}, std::vector<uint64_t>{}, st, &seen, &related);
    check(seen && related == 0 && anchors == std::vector<size_t>{0} &&
              !st[1].open && st[0].open && st[0].win_start == 968 &&
              st[0].next_free == 968 + 575,
          "regla 6: sin voces del Loop + apertura del Intro → el Loop cierra "
          "y el Intro ancla en 968 con su paso hasta 1543");

    // Compás normal del loop: el bajo sigue sonando → el hi-hat es interno.
    st = loop_open();
    seen = false;
    const auto bar = decide_at({kHat}, std::vector<uint64_t>{kBassLoop}, st,
                               &seen, nullptr);
    check(!seen && bar.empty() && st[1].open && !st[0].open,
          "regla 6: con una voz del Loop sostenida no hay contradicción");

    // Sin estado de voces (la tabla) la regla está apagada.
    st = loop_open();
    seen = false;
    const auto table_path = decide_at({kHat}, std::nullopt, st, &seen, nullptr);
    check(!seen && table_path.empty() && st[1].open,
          "regla 6: sin estado de voces sigue el reclamo de siempre");

    // Un EFECTO que toma prestada la firma no contradice la música.
    std::vector<SeqAnchorSub> with_effect{intro, loop};
    with_effect[0].exclusive = false;
    {
      std::vector<ayther::SeqAnchorState> fx = loop_open();
      std::vector<uint64_t> none;
      std::optional<std::span<const uint64_t>> vs{std::span<const uint64_t>{none}};
      bool fx_seen = false;
      const auto r = ayther::seq_anchor_frame_decided(
          968, {kHat}, vs, with_effect, fx,
          [](const ayther::SeqAnchorCandidateView &) noexcept {},
          [&](const ayther::SeqAnchorDecisionView &value) noexcept {
            fx_seen = fx_seen ||
                      value.result == ayther::SeqAnchorDecisionResult::
                                          contradicted_open_sequence;
          },
          [](const ayther::SeqAnchorResolutionView &) noexcept {});
      check(!fx_seen && r.empty() && fx[1].open,
            "regla 6: sólo entre Secuencias exclusivas (música/ambiente)");
    }

    // Y la apertura tiene que ser ENTERA: si el otro tiene cabeza, falta una
    // de sus firmas → no es su apertura → no hay contradicción.
    std::vector<SeqAnchorSub> headed{intro, loop};
    headed[0].head = {kHat, kOther};
    {
      std::vector<ayther::SeqAnchorState> hs = loop_open();
      std::vector<uint64_t> none;
      std::optional<std::span<const uint64_t>> vs{std::span<const uint64_t>{none}};
      const auto r = ayther::seq_anchor_frame_decided(
          968, {kHat}, vs, headed, hs,
          [](const ayther::SeqAnchorCandidateView &) noexcept {},
          [](const ayther::SeqAnchorDecisionView &) noexcept {},
          [](const ayther::SeqAnchorResolutionView &) noexcept {});
      check(r.empty() && hs[1].open,
            "regla 6: media apertura de otra Secuencia no contradice");
    }
  }

  std::printf("%s\n", g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------
// elements_toml_test — el documento único de Identidades de Pintar (#365).
//
// POR QUÉ EXISTE. Cuadro, Panorámica, Cinemática, Animación, Utilería, Carácter
// y UI son la misma familia para el artista, y viajaban en cinco archivos
// distintos porque cinco MECANISMOS del motor las sirven. Fundirlos en uno es
// un cambio de layout de la entrega: si algo se pierde en el camino, el pack
// hornea igual y el jugador ve el juego sin la Panorámica. No falla — falta.
//
// Lo que se fija:
//
//   1. IDA Y VUELTA. Lo que entra al documento único sale idéntico. Cada
//      familia se compara campo a campo, incluidas las celdas, que es donde
//      vive el volumen y donde un separador mal puesto no se nota.
//
//   2. EL DOCUMENTO ÚNICO ES EQUIVALENTE A LOS CINCO. Se parsea el mismo
//      contenido por los dos caminos —elements.toml y los cinco `parse_*_toml`
//      sobre sus documentos separados— y tiene que dar lo mismo. Esta es la
//      compatibilidad LEGACY expresada como una igualdad y no como una promesa:
//      un pack viejo se lee por el segundo camino.
//
//   3. UNA FAMILIA VACÍA NO SE LLEVA A LAS OTRAS. El caso normal es tener sólo
//      Cuadros, o sólo una Cinemática. Con cinco archivos eso era imposible de
//      romper (el archivo no existía); con uno solo, un `return` temprano en el
//      lugar equivocado apaga a las demás en silencio.
//
// Sin ROM, sin core, sin GPU: writer y parser son funciones puras.
// ---------------------------------------------------------------------------
#include "ayther_components_toml.h"
#include "ayther_animation.h"
#include "ayther_audio_events.h"

#include <cstdio>
#include <string>
#include <vector>

// ayther_components_toml.cpp también aloja los parsers de animations.toml y
// audio_events.toml, que llaman a estos dos métodos —y sólo a estos dos— de
// otras TUs del motor. Se resuelven acá con cuerpos vacíos para que el test
// quede PURO: linkear ayther_engine entero por dos símbolos arrastraría SDL y
// Vulkan a una prueba que sólo mira texto. Ninguno de los dos se ejecuta: este
// test no toca esas dos familias.
namespace ayther {
void AnimationPlayer::define(uint64_t, const std::string&, const HdPose*,
                             uint32_t, int) {}
void AudioEventSubstitution::assign(uint64_t, const std::string&, bool) {}
}  // namespace ayther

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

using namespace ayther;

// -- Material de prueba: una de cada familia, con celdas de verdad -----------

std::vector<PackScreen> make_screens() {
    PackScreen s;
    s.id = 0xC0FFEE01; s.name = "Título";
    s.plane_mask = 0x03; s.min_match = 0.88f; s.max_extra = 0.12f;
    s.asset = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
    s.cells = { { 0x1111, 0, 3, 4 }, { 0x2222, 1, 5, 6 }, { 0x3333, 0, 7, 8 } };
    return { s };
}

std::vector<PackPanorama> make_panoramas() {
    PackPanorama p;
    p.id = 0xB00B1E02; p.name = "Bosque";
    p.plane = 1; p.origin_x = -12; p.origin_y = 3;   // origen NEGATIVO a propósito
    p.w_cells = 40; p.h_cells = 28;
    p.asset = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
    // Dos filas, con hueco en la segunda: el `lx` explícito existe por esto.
    p.cells = { { 0xAA01, -2, 0 }, { 0xAA02, -1, 0 }, { 0xAA03, 0, 0 },
                { 0xBB01, -2, 1 }, { 0xBB02,  5, 1 } };
    return { p };
}

std::vector<PackKinematic> make_kinematics() {
    PackKinematic k;
    k.id = 0xC1EA0003; k.name = "Intro";
    k.gap_frames = 12; k.loop = true; k.gain = 0.8f; k.game_gain = 0.25f;
    k.audio = "deadbeefdeadbeefdeadbeefdeadbeef";
    // El `@offset` viaja pegado al asset del paso, porque indexa el clip de ESE
    // paso: un paso sin asset no puede llevarlo, y el writer lo omite a
    // propósito. Por eso el offset va en el paso CON video y el paso sin asset
    // (que hereda el dibujo del Cuadro) va en 0.
    k.steps = { { 0xC0FFEE01, "aaaabbbbccccddddeeeeffff00001111", 900 },
                { 0xC0FFEE02, "", 0 } };
    return { k };
}

std::vector<PackPlaneSequence> make_sequences() {
    PackPlaneSequence q;
    q.id = 0x5E900004; q.name = "PRESS START";
    q.steps = { { 0x5E7A0001, "11112222333344445555666677778888", 30 },
                { 0x5E7A0002, "",                                 45 } };
    return { q };
}

void make_sets(std::vector<PackPlaneSet>& sets, std::vector<PackPlaneFont>& fonts) {
    PackPlaneFont f;
    f.id = 0xF0F00005; f.name = "Fuente chica"; f.cell_w = 1; f.cell_h = 2;
    fonts.push_back(f);

    PackPlaneSet s;
    s.id = 0x5E700006; s.name = "Cartel"; s.type = "utileria";
    s.plane = 2; s.w_cells = 4; s.h_cells = 3;
    s.asset = "99887766554433221100aabbccddeeff";
    s.members = { { 0xD001, 0, 0 }, { 0xD002, 3, 0 }, { 0xD003, 0, 2 } };
    s.font_id = 0xF0F00005; s.ch = "A";
    s.off_x = -24; s.off_y = 3;   // #231: re-anclaje del HUD, con signo
    // Referencia del tinte E1 (fundido de paleta del Objeto).
    s.ref_rgb[0] = 180; s.ref_rgb[1] = 96; s.ref_rgb[2] = 24;
    sets.push_back(s);

    // Un segundo set SIN referencia: el default {0,0,0} tiene que sobrevivir
    // la ida y vuelta (y no emitirse — dialecto byte-compatible).
    PackPlaneSet s2;
    s2.id = 0x5E700007; s2.name = "Cartel sin tinte"; s2.type = "utileria";
    s2.plane = 0; s2.w_cells = 1; s2.h_cells = 1;
    s2.asset = "00112233445566778899aabbccddeeff";
    s2.members = { { 0xD004, 0, 0 } };
    sets.push_back(s2);
}

// -- Comparadores campo a campo ---------------------------------------------

bool same(const std::vector<PackScreen>& a, const std::vector<PackScreen>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name
            || a[i].plane_mask != b[i].plane_mask || a[i].asset != b[i].asset
            || a[i].cells.size() != b[i].cells.size()) return false;
        for (size_t j = 0; j < a[i].cells.size(); ++j)
            if (a[i].cells[j].hash != b[i].cells[j].hash
                || a[i].cells[j].plane != b[i].cells[j].plane
                || a[i].cells[j].col != b[i].cells[j].col
                || a[i].cells[j].row != b[i].cells[j].row) return false;
    }
    return true;
}

bool same(const std::vector<PackPanorama>& a, const std::vector<PackPanorama>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].plane != b[i].plane
            || a[i].origin_x != b[i].origin_x || a[i].origin_y != b[i].origin_y
            || a[i].w_cells != b[i].w_cells || a[i].h_cells != b[i].h_cells
            || a[i].asset != b[i].asset
            || a[i].cells.size() != b[i].cells.size()) return false;
        for (size_t j = 0; j < a[i].cells.size(); ++j)
            if (a[i].cells[j].hash != b[i].cells[j].hash
                || a[i].cells[j].lx != b[i].cells[j].lx
                || a[i].cells[j].ly != b[i].cells[j].ly) return false;
    }
    return true;
}

bool same(const std::vector<PackKinematic>& a, const std::vector<PackKinematic>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name
            || a[i].gap_frames != b[i].gap_frames || a[i].loop != b[i].loop
            || a[i].audio != b[i].audio
            // #514: los dos gain también son parte de la ida y vuelta. Sin
            // esta comparación el oráculo era CIEGO al defecto: el writer no
            // los emitía, el parser devolvía el default y el round-trip pasaba
            // en verde mientras el dato se perdía en el bake.
            || a[i].gain != b[i].gain || a[i].game_gain != b[i].game_gain
            || a[i].steps.size() != b[i].steps.size()) return false;
        for (size_t j = 0; j < a[i].steps.size(); ++j)
            if (a[i].steps[j].screen_id != b[i].steps[j].screen_id
                || a[i].steps[j].asset != b[i].steps[j].asset
                || a[i].steps[j].video_offset != b[i].steps[j].video_offset) return false;
    }
    return true;
}

bool same(const std::vector<PackPlaneSequence>& a,
          const std::vector<PackPlaneSequence>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name
            || a[i].steps.size() != b[i].steps.size()) return false;
        for (size_t j = 0; j < a[i].steps.size(); ++j)
            if (a[i].steps[j].set_id != b[i].steps[j].set_id
                || a[i].steps[j].asset != b[i].steps[j].asset
                || a[i].steps[j].duration != b[i].steps[j].duration) return false;
    }
    return true;
}

bool same(const std::vector<PackPlaneSet>& a, const std::vector<PackPlaneSet>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].type != b[i].type
            || a[i].plane != b[i].plane || a[i].asset != b[i].asset
            || a[i].font_id != b[i].font_id || a[i].ch != b[i].ch
            || a[i].ref_rgb[0] != b[i].ref_rgb[0]
            || a[i].ref_rgb[1] != b[i].ref_rgb[1]
            || a[i].ref_rgb[2] != b[i].ref_rgb[2]
            || a[i].members.size() != b[i].members.size()) return false;
        for (size_t j = 0; j < a[i].members.size(); ++j)
            if (a[i].members[j].hash != b[i].members[j].hash
                || a[i].members[j].cx != b[i].members[j].cx
                || a[i].members[j].cy != b[i].members[j].cy) return false;
    }
    return true;
}

struct Bag {
    std::vector<PackScreen>        scr;
    std::vector<PackPanorama>      pans;
    std::vector<PackKinematic>     kins;
    std::vector<PackPlaneSequence> seqs;
    std::vector<PackPlaneSet>      sets;
    std::vector<PackPlaneFont>     fonts;
};

}  // namespace

int main() {
    std::printf("=== elements_toml_test ===\n");

    const auto scr0  = make_screens();
    const auto pans0 = make_panoramas();
    const auto kins0 = make_kinematics();
    const auto seqs0 = make_sequences();
    std::vector<PackPlaneSet>  sets0;
    std::vector<PackPlaneFont> fonts0;
    make_sets(sets0, fonts0);

    // -- 1. Ida y vuelta por el documento único ------------------------------
    const std::string doc = bake_elements_toml(scr0, pans0, kins0, seqs0, sets0, fonts0);
    check(!doc.empty(), "el documento unico se hornea");

    Bag u;
    const size_t n = parse_elements_toml(doc, u.scr, u.pans, u.kins, u.seqs,
                                         u.sets, u.fonts);
    check(n > 0, "el documento unico parsea");
    check(same(scr0,  u.scr),  "Cuadro: ida y vuelta campo a campo");
    check(same(pans0, u.pans), "Panoramica: ida y vuelta, con origen negativo y hueco de fila");
    check(same(kins0, u.kins), "Cinematica: ida y vuelta, con paso sin asset y offset de video");
    check(same(seqs0, u.seqs), "Animacion: ida y vuelta, con duraciones por paso");
    check(same(sets0, u.sets), "Utileria: ida y vuelta, con fuente y caracter");
    check(u.fonts.size() == fonts0.size()
              && !u.fonts.empty() && u.fonts[0].id == fonts0[0].id
              && u.fonts[0].cell_h == fonts0[0].cell_h,
          "Caracter: la tipografia viaja");

    // -- 2. El documento único equivale a los CINCO separados ----------------
    // Es la compatibilidad legacy escrita como igualdad: un pack viejo trae los
    // cinco y se lee por este segundo camino.
    Bag v;
    parse_screens_toml        (bake_screens_toml(scr0),          v.scr);
    parse_panoramas_toml      (bake_panoramas_toml(pans0),       v.pans);
    parse_kinematics_toml     (bake_kinematics_toml(kins0),      v.kins);
    parse_plane_sequences_toml(bake_plane_sequences_toml(seqs0), v.seqs);
    parse_plane_sets_toml     (bake_plane_sets_toml(sets0, fonts0), v.sets, v.fonts);
    check(same(u.scr,  v.scr)  && same(u.pans, v.pans)
              && same(u.kins, v.kins) && same(u.seqs, v.seqs)
              && same(u.sets, v.sets) && u.fonts.size() == v.fonts.size(),
          "documento UNICO y los CINCO separados dan exactamente lo mismo");

    // -- 3. Una familia vacía no se lleva a las otras ------------------------
    // Con cinco archivos esto no se podía romper: el archivo no existía. Con
    // uno solo, un `return` temprano mal puesto apaga a las demás sin ruido.
    {
        Bag w;
        const std::string solo_kin = bake_elements_toml({}, {}, kins0, {}, {}, {});
        parse_elements_toml(solo_kin, w.scr, w.pans, w.kins, w.seqs, w.sets, w.fonts);
        check(same(kins0, w.kins) && w.scr.empty() && w.pans.empty()
                  && w.seqs.empty() && w.sets.empty(),
              "solo Cinematica: llega, y las familias vacias quedan vacias");
    }
    {
        Bag w;
        const std::string without_screens = bake_elements_toml({}, pans0, {}, seqs0, sets0, fonts0);
        parse_elements_toml(without_screens, w.scr, w.pans, w.kins, w.seqs, w.sets, w.fonts);
        check(w.scr.empty() && same(pans0, w.pans) && same(seqs0, w.seqs)
                  && same(sets0, w.sets),
              "sin Cuadros: las demas familias siguen llegando");
    }
    {
        Bag w;
        const size_t z = parse_elements_toml(bake_elements_toml({}, {}, {}, {}, {}, {}),
                                             w.scr, w.pans, w.kins, w.seqs,
                                             w.sets, w.fonts);
        check(z == 0 && w.scr.empty() && w.pans.empty() && w.kins.empty(),
              "documento sin ninguna Identidad: cero, sin ruido");
    }
    {
        // Un documento roto se lleva TODAS las Identidades, no una familia:
        // ese es el precio del archivo único y tiene que degradar limpio.
        Bag w;
        const size_t z = parse_elements_toml("[[screen]]\nid = ", w.scr, w.pans,
                                             w.kins, w.seqs, w.sets, w.fonts);
        check(z == 0 && w.scr.empty(), "documento roto: cero, sin crash");
    }

    // -- 4. [[enhance]] (#493): «Mejorar por software» -----------------------
    {
        std::vector<PackEnhance> enh0;
        enh0.push_back({ 0x1001, "Logo SEGA", 1, { 0x0a0a0a0a0a0a0a0aull, 0x0b0b0b0b0b0b0b0bull } });
        enh0.push_back({ 0x1002, "Copyright", 0, { 0x0c0c0c0c0c0c0c0cull } });
        enh0.push_back({ 0x1003, "Sombra",    3, { 0x0d0d0d0d0d0d0d0dull } });
        // Sin marcas el documento es BYTE-IDÉNTICO al previo a la feature.
        check(bake_elements_toml(scr0, pans0, kins0, seqs0, sets0, fonts0, {}) == doc,
              "enhance: sin marcas el documento no cambia ni un byte");
        const std::string doc_e = bake_elements_toml(scr0, pans0, kins0, seqs0, sets0, fonts0, enh0);
        check(doc_e.find("[[enhance]]") != std::string::npos
                  && doc_e.find("hashes = \"0x0a0a0a0a0a0a0a0a|0x0b0b0b0b0b0b0b0b\"") != std::string::npos
                  && doc_e.find("layer = 1") != std::string::npos,
              "enhance: se hornea con layer y pipe-list de hashes");
        Bag w; std::vector<PackEnhance> enh1;
        const size_t n_e = parse_elements_toml(doc_e, w.scr, w.pans, w.kins, w.seqs,
                                               w.sets, w.fonts, &enh1);
        bool rt = n_e == n + enh0.size() && enh1.size() == enh0.size();
        for (size_t i = 0; rt && i < enh0.size(); ++i)
            rt = enh1[i].id == enh0[i].id && enh1[i].name == enh0[i].name
                 && enh1[i].layer == enh0[i].layer && enh1[i].hashes == enh0[i].hashes;
        check(rt, "enhance: ida y vuelta campo a campo, las demas familias intactas");
        check(same(scr0, w.scr) && same(sets0, w.sets),
              "enhance: [[screen]]/[[set]] no cambian con el array nuevo");
        // Forward-compat: un consumidor que NO pide la lista (player viejo =
        // nullptr) ignora el array y lee todo lo demás igual.
        Bag old_player;
        const size_t n_old = parse_elements_toml(doc_e, old_player.scr, old_player.pans,
                                                 old_player.kins, old_player.seqs,
                                                 old_player.sets, old_player.fonts);
        check(n_old == n && same(scr0, old_player.scr) && same(seqs0, old_player.seqs),
              "enhance: un lector que no lo conoce ignora el array y no rompe");
        // Array DESCONOCIDO del futuro: tampoco rompe.
        Bag f; std::vector<PackEnhance> enh2;
        const size_t n_f = parse_elements_toml(
            doc_e + "\n[[vectorize]]\nid = \"0x1\"\nmode = \"bezier\"\n",
            f.scr, f.pans, f.kins, f.seqs, f.sets, f.fonts, &enh2);
        check(n_f == n_e && enh2.size() == enh0.size(),
              "enhance: un array desconocido se ignora (forward-compat)");
        // Entradas inválidas: capa fuera de rango o sin hashes se descartan.
        std::vector<PackEnhance> bad;
        parse_enhance_toml("[[enhance]]\nid = \"0x1\"\nlayer = 7\nhashes = \"0x1\"\n"
                           "[[enhance]]\nid = \"0x2\"\nlayer = 2\nhashes = \"\"\n"
                           "[[enhance]]\nid = \"0x3\"\nlayer = 2\nhashes = \"0x9\"\n", bad);
        check(bad.size() == 1 && bad[0].id == 3, "enhance: capa invalida o sin hashes se descarta");
        // #503: intensidad k. Ausente = 255; el default NO se emite (byte-
        // idéntico con el pack previo a la feature); 128 hornea y round-trippea;
        // fuera de rango se clampea (no apaga la mejora).
        check(enh1[0].k == 255 && doc_e.find("\nk = ") == std::string::npos,
              "enhance k: ausente = 255 y el default no emite la clave");
        std::vector<PackEnhance> enhk = enh0;
        enhk[1].k = 128;
        const std::string doc_k = bake_elements_toml(scr0, pans0, kins0, seqs0, sets0, fonts0, enhk);
        std::vector<PackEnhance> enhk1; Bag wk;
        parse_elements_toml(doc_k, wk.scr, wk.pans, wk.kins, wk.seqs, wk.sets, wk.fonts, &enhk1);
        check(doc_k.find("\nk = 128\n") != std::string::npos && enhk1.size() == 3 &&
              enhk1[0].k == 255 && enhk1[1].k == 128 && enhk1[2].k == 255,
              "enhance k: 128 se hornea solo en su bloque y vuelve igual");
        std::vector<PackEnhance> kc;
        parse_enhance_toml("[[enhance]]\nid = \"0x1\"\nlayer = 2\nk = 900\nhashes = \"0x9\"\n"
                           "[[enhance]]\nid = \"0x2\"\nlayer = 2\nk = -4\nhashes = \"0x9\"\n", kc);
        check(kc.size() == 2 && kc[0].k == 255 && kc[1].k == 0,
              "enhance k: fuera de rango se clampea a 0..255");
    }

    // -- instruments.toml (#519) ------------------------------------------
    // La re-sintesis POR TIMBRE del pack. Antes nadie la traia a la sesion: en
    // el Lab el catalogo llega del frontend cada frame, y en Play NO LLEGA
    // NADIE, asi que la re-sintesis no sonaba nunca.
    {
        const std::string doc =
            "[[instrument]]\n"
            "patch = \"0x00000000deadbeef\"\n"
            "soundfont = \"aabbccdd\"\n"
            "bank = 1\npreset = 42\ntranspose = -3\ngain = 1.5\n\n"
            "[[instrument]]\n"
            "patch = \"0x00000000cafe0001\"\n"
            "soundfont = \"eeff0011\"\n"
            "bank = 0\npreset = 7\n";
        std::vector<PackInstrument> ins;
        const size_t n = parse_instruments_toml(doc, ins);
        check(n == 2 && ins.size() == 2, "instruments: los dos timbres llegan");
        check(ins[0].patch == 0xdeadbeefull && ins[0].soundfont == "aabbccdd"
              && ins[0].bank == 1 && ins[0].preset == 42
              && ins[0].transpose == -3 && ins[0].gain == 1.5f,
              "instruments: ida campo a campo, con transpose negativo");
        check(ins[1].transpose == 0 && ins[1].gain == 1.0f,
              "instruments: los ausentes toman su default (transpose 0, gain 1)");
        std::vector<PackInstrument> sinsf;
        check(parse_instruments_toml("[[instrument]]\npatch = \"0x1\"\npreset = 3\n",
                                     sinsf) == 0 && sinsf.empty(),
              "instruments: sin soundfont se descarta");
        std::vector<PackInstrument> sinpatch;
        check(parse_instruments_toml("[[instrument]]\nsoundfont = \"aa\"\n",
                                     sinpatch) == 0,
              "instruments: sin patch se descarta");
        std::vector<PackInstrument> g0;
        parse_instruments_toml("[[instrument]]\npatch = \"0x2\"\n"
                               "soundfont = \"aa\"\ngain = 0\n", g0);
        check(g0.size() == 1 && g0[0].gain == 1.0f,
              "instruments: gain 0 cae al neutro (1.0), no silencia el timbre");
        std::vector<PackInstrument> invalid_entries;
        check(parse_instruments_toml("[[instrument\npatch = ", invalid_entries) == 0,
              "instruments: documento roto = cero, sin crash");
    }

    // -- gain por firma en audio_events.toml (#515) ------------------------
    // La ganancia AUTORADA de la Secuencia. Antes no viajaba y el mixer
    // recibia 1.0f fijo: mover el slider en Mezclar no se oia en el pack.
    {
        const std::string doc =
            "[[event]]\nsignature = \"0x00000000aaaa0001\"\n"
            "asset = \"aa\"\ngain = 0.5\n"
            "[[event]]\nsignature = \"0x00000000aaaa0002\"\nasset = \"bb\"\n"
            "[[event]]\nsignature = \"0x00000000aaaa0003\"\nasset = \"cc\"\ngain = 0\n";
        const auto g = parse_audio_event_gains(doc);
        check(g.size() == 1, "gain: solo las firmas que DECLARAN el campo entran");
        check(g.count(0xaaaa0001ull) && g.at(0xaaaa0001ull) == 0.5f,
              "gain: el valor autorado llega");
        check(!g.count(0xaaaa0002ull),
              "gain: ausente no entra al mapa (el consumidor decide el neutro)");
        check(!g.count(0xaaaa0003ull),
              "gain: 0 no entra — silenciar es del mute, no de un numero mal escrito");
        check(parse_audio_event_gains("[[event\n").empty(),
              "gain: documento roto = vacio, sin crash");
    }

    // -- region de loop por firma (#228) -----------------------------------
    // El mixer ya sabia ciclar una region con precision de muestra; lo que
    // faltaba era que el dato autorado llegara hasta el.
    {
        const std::string doc =
            "[[event]]\nsignature = \"0x00000000bbbb0001\"\n"
            "asset = \"aa\"\nloop = true\nloop_begin = 100\nloop_end = 900\n"
            "[[event]]\nsignature = \"0x00000000bbbb0002\"\nasset = \"bb\"\n"
            "[[event]]\nsignature = \"0x00000000bbbb0003\"\nasset = \"cc\"\n"
            "loop_begin = 500\nloop_end = 200\n"
            "[[event]]\nsignature = \"0x00000000bbbb0004\"\nasset = \"dd\"\n"
            "loop_begin = 10\n";
        const auto lp = parse_audio_event_loops(doc);
        check(lp.size() == 1, "loop: solo la firma con region VALIDA entra");
        check(lp.count(0xbbbb0001ull) && lp.at(0xbbbb0001ull).first == 100
              && lp.at(0xbbbb0001ull).second == 900,
              "loop: inicio y fin llegan en cuadros del asset");
        check(!lp.count(0xbbbb0002ull),
              "loop: ausente no entra (el consumidor usa el asset entero)");
        check(!lp.count(0xbbbb0003ull),
              "loop: fin <= inicio se descarta al parsear, no mas lejos");
        check(!lp.count(0xbbbb0004ull),
              "loop: la mitad del par no alcanza");

        // #562: EL CONTRATO ENTRE EL PRODUCTOR Y ESTE PARSER, con el numero
        // escrito. El panel autora en MILISEGUNDOS y el mixer consume CUADROS
        // DE AUDIO a 44100; `lab::audio_seq_loop_toml` hace la conversion y
        // emite exactamente esta forma.
        //
        // El caso vive de este lado —FOSS— porque lo que fija es que el
        // CONSUMIDOR entienda lo que el productor escribe. El bake tiene su
        // propio test del otro lado de la frontera (lab_audio_loop_bake), y
        // entre los dos cubren la cadena sin que ninguno cruce.
        //
        // 1 s = 44100 cuadros; 2 s = 88200. Si alguien cambia la unidad de un
        // lado, este numero deja de coincidir con el del otro test.
        const auto rt = parse_audio_event_loops(
            "[[event]]\nsignature = \"0x00000000cccc0001\"\n"
            "asset = \"tema.ogg\"\nloop = true\n"
            "loop_begin = 44100\nloop_end = 88200\n");
        check(rt.size() == 1 && rt.count(0xcccc0001ull)
              && rt.at(0xcccc0001ull).first == 44100u
              && rt.at(0xcccc0001ull).second == 88200u,
              "loop: lo que el bake escribe (1s-2s en cuadros a 44100) es lo que se lee");
        check(parse_audio_event_loops("[[event\n").empty(),
              "loop: documento roto = vacio, sin crash");
    }

    std::printf(g_fail ? "\n=== FAIL (%d) ===\n" : "\n=== OK ===\n", g_fail);
    return g_fail ? 1 : 0;
}

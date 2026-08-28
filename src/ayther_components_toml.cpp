// ---------------------------------------------------------------------------
// ayther_components_toml.cpp — round-trip TOML de Componentes (ver header).
// Horneo a mano (formato estable, hashes como "0x%016llx"); parseo con toml++
// header-only (misma convención que ayther_config.cpp).
// ---------------------------------------------------------------------------

#include "ayther_components_toml.h"

#include <toml++/toml.hpp>

#include <cinttypes>
#include <cstdio>

namespace ayther {

// : cada `parse_*_toml` quedó partido en dos — el wrapper que PARSEA el
// texto y el `_from` que decodifica un documento ya parseado. La razón es
// elements.toml: leer las cinco familias de un solo documento tiene que costar
// UNA sola pasada de toml::parse, no cinco sobre los mismos bytes (una
// Panorámica larga son ~1 MB de celdas, y eso es tiempo de apertura de escena).
// Declarados acá porque cada wrapper precede a su `_from`.
size_t parse_plane_sets_from(const toml::table& tbl,
                             std::vector<PackPlaneSet>& out_sets,
                             std::vector<PackPlaneFont>& out_fonts);
size_t parse_screens_from(const toml::table& tbl, std::vector<PackScreen>& out);
size_t parse_panoramas_from(const toml::table& tbl, std::vector<PackPanorama>& out);
size_t parse_kinematics_from(const toml::table& tbl, std::vector<PackKinematic>& out);
size_t parse_plane_sequences_from(const toml::table& tbl,
                                  std::vector<PackPlaneSequence>& out);
size_t parse_enhance_from(const toml::table& tbl, std::vector<PackEnhance>& out);

namespace {

void append_hex(std::string& out, const char* key, uint64_t v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s = \"0x%016" PRIx64 "\"\n", key, v);
    out += buf;
}

void append_rect(std::string& out, const char* key, float x, float y, float w, float h) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s = [%.2f, %.2f, %.2f, %.2f]\n", key, x, y, w, h);
    out += buf;
}

uint64_t parse_hex(const toml::table& t, const char* key) {
    const auto s = t[key].value<std::string>();
    if (!s) return 0;
    return std::strtoull(s->c_str(), nullptr, 0);   // acepta "0x..." y decimal
}

/// Lee un array TOML [x,y,w,h] de floats en `out4`; false si falta/está corto.
bool parse_rect(const toml::table& t, const char* key, float out4[4]) {
    const toml::array* a = t[key].as_array();
    if (!a || a->size() < 4) return false;
    for (size_t i = 0; i < 4; ++i)
        out4[i] = static_cast<float>((*a)[i].value<double>().value_or(0.0));
    return true;
}

}  // namespace

// -- Horneo --------------------------------------------------------------------

std::string bake_animations_toml(const std::vector<AnimationDef>& defs) {
    std::string out =
        "# animations.toml — animaciones HD en fase (C-S2/C-S4).\n"
        "# Horneado por el Deliver de Ayther Lab; cargado por AytherSession.\n";
    char buf[160];
    for (const AnimationDef& d : defs) {
        out += "\n[[animation]]\n";
        append_hex(out, "clip", d.clip_id);
        std::snprintf(buf, sizeof(buf), "sheet = \"%s\"\ntween = %d\n",
                      d.sheet.c_str(), d.tween_level);
        out += buf;
        for (const HdPose& p : d.poses) {
            out += "\n[[animation.pose]]\n";
            append_hex(out, "pose", p.pose);
            append_rect(out, "src", p.src_x, p.src_y, p.src_w, p.src_h);
            append_rect(out, "anchor", p.anchor.x, p.anchor.y, p.anchor.w, p.anchor.h);
            std::snprintf(buf, sizeof(buf), "ticks = %u\n", unsigned(p.duration_ticks));
            out += buf;
        }
    }
    return out;
}

std::string bake_audio_events_toml(const std::vector<AudioEventAssignment>& assigns) {
    std::string out =
        "# audio_events.toml — sustitución HD por evento de audio (C-A2/C-A4).\n"
        "# Mismo esquema que parsea el core (AudioSubstitutor::parse_events_toml).\n";
    char buf[64];
    for (const AudioEventAssignment& a : assigns) {
        out += "\n[[event]]\n";
        append_hex(out, "signature", a.signature);
        out += "asset = \"" + a.asset + "\"\n";
        std::snprintf(buf, sizeof(buf), "loop = %s\n", a.looping ? "true" : "false");
        out += buf;
    }
    return out;
}

// -- Parseo ----------------------------------------------------------------------

size_t parse_animations_toml(const std::string& text, AnimationPlayer& into) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] animations.toml: parse error: %s\n", e.what());
        return 0;
    }
    const toml::array* anims = tbl["animation"].as_array();
    if (!anims) return 0;

    size_t loaded = 0;
    for (const toml::node& n : *anims) {
        const toml::table* a = n.as_table();
        if (!a) continue;
        const uint64_t clip = parse_hex(*a, "clip");
        const auto sheet    = (*a)["sheet"].value<std::string>();
        if (!clip || !sheet) continue;
        const int tween = int((*a)["tween"].value<int64_t>().value_or(0));

        std::vector<HdPose> poses;
        if (const toml::array* ps = (*a)["pose"].as_array()) {
            for (const toml::node& pn : *ps) {
                const toml::table* pt = pn.as_table();
                if (!pt) continue;
                HdPose p;
                p.pose = parse_hex(*pt, "pose");
                if (!p.pose) continue;
                float r[4];
                if (parse_rect(*pt, "src", r)) {
                    p.src_x = r[0]; p.src_y = r[1]; p.src_w = r[2]; p.src_h = r[3];
                }
                if (parse_rect(*pt, "anchor", r))
                    p.anchor = { r[0], r[1], r[2], r[3] };
                p.duration_ticks =
                    uint16_t((*pt)["ticks"].value<int64_t>().value_or(1));
                poses.push_back(p);
            }
        }
        if (poses.empty()) continue;
        into.define(clip, *sheet, poses.data(), uint32_t(poses.size()), tween);
        ++loaded;
    }
    return loaded;
}

size_t parse_audio_events_toml(const std::string& text, AudioEventSubstitution& into) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] audio_events.toml: parse error: %s\n", e.what());
        return 0;
    }
    const toml::array* events = tbl["event"].as_array();
    if (!events) return 0;

    size_t loaded = 0;
    for (const toml::node& n : *events) {
        const toml::table* e = n.as_table();
        if (!e) continue;
        const uint64_t sig = parse_hex(*e, "signature");
        const auto asset   = (*e)["asset"].value<std::string>();
        if (!sig || !asset || asset->empty()) continue;
        const bool looping = (*e)["loop"].value<bool>().value_or(false);
        into.assign(sig, *asset, looping);
        ++loaded;
    }
    return loaded;
}

std::unordered_map<uint64_t, std::vector<uint64_t>>
parse_audio_event_members(const std::string& text) {
    return parse_audio_event_sig_list(text, "members");
}

std::unordered_map<uint64_t, std::vector<uint64_t>>
parse_audio_event_sig_list(const std::string& text, const char* field) {
    std::unordered_map<uint64_t, std::vector<uint64_t>> out;
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] audio_events.toml: parse error: %s\n", e.what());
        return out;
    }
    const toml::array* events = tbl["event"].as_array();
    if (!events) return out;
    for (const toml::node& n : *events) {
        const toml::table* e = n.as_table();
        if (!e) continue;
        const uint64_t sig = parse_hex(*e, "signature");
        if (!sig) continue;
        const toml::array* ms = (*e)[field].as_array();
        if (!ms) continue;
        std::vector<uint64_t> v;
        for (const toml::node& mn : *ms)
            if (const auto sv = mn.value<std::string>())
                if (const uint64_t m = std::strtoull(sv->c_str(), nullptr, 0))
                    v.push_back(m);
        if (!v.empty()) out.emplace(sig, std::move(v));
    }
    return out;
}

std::unordered_map<uint64_t, uint32_t>
parse_audio_event_tails(const std::string& text) {
    std::unordered_map<uint64_t, uint32_t> out;
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] audio_events.toml: parse error: %s\n", e.what());
        return out;
    }
    const toml::array* events = tbl["event"].as_array();
    if (!events) return out;
    for (const toml::node& n : *events) {
        const toml::table* e = n.as_table();
        if (!e) continue;
        const uint64_t sig = parse_hex(*e, "signature");
        if (!sig) continue;
        // Solo las firmas que DECLARAN el campo entran al mapa — la ausencia
        // significa «ilimitado» (legacy) y se decide en el consumidor.
        if (const auto t = (*e)["tail_frames"].value<int64_t>();
            t && *t >= 0)
            out.emplace(sig, static_cast<uint32_t>(*t));
    }
    return out;
}

std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>>
parse_audio_event_loops(const std::string& text) {
    // : region de loop por firma, en CUADROS del asset. Misma regla de
    // ausencia que tails/fades/gains: solo las firmas que DECLARAN el campo
    // entran. Ausente = (0,0) = el asset entero, que es el contrato de siempre.
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> out;
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] audio_events.toml: parse error: %s\n", e.what());
        return out;
    }
    const toml::array* events = tbl["event"].as_array();
    if (!events) return out;
    for (const toml::node& n : *events) {
        const toml::table* e = n.as_table();
        if (!e) continue;
        const uint64_t sig = parse_hex(*e, "signature");
        if (!sig) continue;
        const auto b = (*e)["loop_begin"].value<int64_t>();
        const auto en = (*e)["loop_end"].value<int64_t>();
        if (!b || !en) continue;
        // Un rango invalido (fin <= inicio) NO se transporta: el mixer ya cae
        // al asset entero, y dejarlo pasar solo moveria el error mas lejos del
        // lugar donde se puede explicar.
        if (*b < 0 || *en <= *b) continue;
        out.emplace(sig, std::make_pair(static_cast<uint32_t>(*b),
                                        static_cast<uint32_t>(*en)));
    }
    return out;
}

std::unordered_map<uint64_t, float>
parse_audio_event_gains(const std::string& text) {
    // : mismo transporte y misma regla de ausencia que `tails` y `fades` —
    // solo las firmas que DECLARAN el campo entran al mapa. Aca la ausencia
    // significa ganancia NEUTRA (1.0), que es lo que el mixer aplicaba fijo.
    std::unordered_map<uint64_t, float> out;
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] audio_events.toml: parse error: %s\n", e.what());
        return out;
    }
    const toml::array* events = tbl["event"].as_array();
    if (!events) return out;
    for (const toml::node& n : *events) {
        const toml::table* e = n.as_table();
        if (!e) continue;
        const uint64_t sig = parse_hex(*e, "signature");
        if (!sig) continue;
        // Un gain <= 0 no silencia: cae al neutro. Silenciar es una decision
        // del mute, no un efecto lateral de un numero mal escrito.
        if (const auto g = (*e)["gain"].value<double>(); g && *g > 0.0)
            out.emplace(sig, static_cast<float>(*g));
    }
    return out;
}

std::unordered_map<uint64_t, uint32_t>
parse_audio_event_fades(const std::string& text) {
    // : mismo transporte y misma regla de ausencia que `tails` — sólo las
    // firmas que DECLARAN el campo entran al mapa. Acá la ausencia significa
    // «sin fade» (la política de fin la decide ), no «ilimitado».
    std::unordered_map<uint64_t, uint32_t> out;
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] audio_events.toml: parse error: %s\n", e.what());
        return out;
    }
    const toml::array* events = tbl["event"].as_array();
    if (!events) return out;
    for (const toml::node& n : *events) {
        const toml::table* e = n.as_table();
        if (!e) continue;
        const uint64_t sig = parse_hex(*e, "signature");
        if (!sig) continue;
        if (const auto f = (*e)["fade_frames"].value<int64_t>();
            f && *f > 0)
            out.emplace(sig, static_cast<uint32_t>(*f));
    }
    return out;
}

// -- plane_sets.toml (Utilería CU002 · Glifos CU005) --------------------------

std::string bake_plane_sets_toml(const std::vector<PackPlaneSet>& sets,
                                 const std::vector<PackPlaneFont>& fonts) {
    std::string out =
        "# plane_sets.toml — Utilería y Glifos: sustitución HD por ELEMENTO\n"
        "# multi-tile de plano. Horneado por el Deliver de Ayther Lab; cargado\n"
        "# por AytherSession (define_plane_set). Los offsets son en CELDAS,\n"
        "# relativos al top-left del conjunto. Los flips NO viajan: el hash de\n"
        "# tile de plano es flip-invariante y el matcher no los exige.\n";
    char buf[512];
    for (const PackPlaneFont& f : fonts) {
        if (!f.id) continue;
        out += "\n[[font]]\n";
        append_hex(out, "id", f.id);
        std::snprintf(buf, sizeof(buf), "name = \"%s\"\ncell_w = %u\ncell_h = %u\n",
                      f.name.c_str(), unsigned(f.cell_w), unsigned(f.cell_h));
        out += buf;
    }
    for (const PackPlaneSet& s : sets) {
        if (!s.id || s.asset.empty() || s.members.empty()) continue;
        if (s.members.size() > kMaxPlaneSetMembers) continue;
        out += "\n[[set]]\n";
        append_hex(out, "id", s.id);
        std::snprintf(buf, sizeof(buf),
                      "name = \"%s\"\ntype = \"%s\"\nplane = %u\n"
                      "w_cells = %u\nh_cells = %u\nasset = \"%s\"\n",
                      s.name.c_str(), s.type.empty() ? "utileria" : s.type.c_str(),
                      unsigned(s.plane), unsigned(s.w_cells), unsigned(s.h_cells),
                      s.asset.c_str());
        out += buf;
        out += "tiles = \"";
        for (size_t i = 0; i < s.members.size(); ++i) {
            const PackPlaneSetMember& m = s.members[i];
            std::snprintf(buf, sizeof(buf), "%s0x%016" PRIx64 ":%d,%d",
                          i ? "|" : "", m.hash, int(m.cx), int(m.cy));
            out += buf;
        }
        out += "\"\n";
        // : el re-anclaje del HUD viaja SOLO si el autor lo movio — un
        // pack ya horneado no cambia un byte.
        if (s.off_x || s.off_y) {
            char ob[64];
            std::snprintf(ob, sizeof(ob), "off = \"%d,%d\"\n",
                          (int)s.off_x, (int)s.off_y);
            out += ob;
        }
        if (s.font_id) append_hex(out, "font", s.font_id);
        if (!s.ch.empty()) {
            std::snprintf(buf, sizeof(buf), "ch = \"%s\"\n", s.ch.c_str());
            out += buf;
        }
        // Tinte E1: la referencia autorada viaja como en las poses. Sólo si
        // existe — un pack sin la feature queda byte-idéntico.
        if (s.ref_rgb[0] | s.ref_rgb[1] | s.ref_rgb[2]) {
            std::snprintf(buf, sizeof(buf), "ref = \"%u,%u,%u\"\n",
                          unsigned(s.ref_rgb[0]), unsigned(s.ref_rgb[1]),
                          unsigned(s.ref_rgb[2]));
            out += buf;
        }
    }
    return out;
}

size_t parse_plane_sets_toml(const std::string& text,
                             std::vector<PackPlaneSet>& out_sets,
                             std::vector<PackPlaneFont>& out_fonts) {
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] plane_sets.toml: parse error: %s\n", e.what());
        return 0;
    }
    return parse_plane_sets_from(tbl, out_sets, out_fonts);
}

size_t parse_plane_sets_from(const toml::table& tbl,
                             std::vector<PackPlaneSet>& out_sets,
                             std::vector<PackPlaneFont>& out_fonts) {
    if (const toml::array* fs = tbl["font"].as_array()) {
        for (const toml::node& n : *fs) {
            const toml::table* f = n.as_table();
            if (!f) continue;
            PackPlaneFont d;
            d.id = parse_hex(*f, "id");
            if (!d.id) continue;
            d.name   = (*f)["name"].value<std::string>().value_or(std::string());
            d.cell_w = uint16_t((*f)["cell_w"].value<int64_t>().value_or(1));
            d.cell_h = uint16_t((*f)["cell_h"].value<int64_t>().value_or(1));
            out_fonts.push_back(std::move(d));
        }
    }
    const toml::array* ss = tbl["set"].as_array();
    if (!ss) return 0;
    size_t loaded = 0;
    for (const toml::node& n : *ss) {
        const toml::table* s = n.as_table();
        if (!s) continue;
        PackPlaneSet d;
        d.id = parse_hex(*s, "id");
        const auto asset = (*s)["asset"].value<std::string>();
        if (!d.id || !asset || asset->empty()) continue;
        d.asset   = *asset;
        d.name    = (*s)["name"].value<std::string>().value_or(std::string());
        d.type    = (*s)["type"].value<std::string>().value_or(std::string("utileria"));
        d.plane   = uint8_t ((*s)["plane"].value<int64_t>().value_or(0));
        d.w_cells = uint16_t((*s)["w_cells"].value<int64_t>().value_or(0));
        d.h_cells = uint16_t((*s)["h_cells"].value<int64_t>().value_or(0));
        // : "x,y" en pixeles. Ausente = (0,0) = el Objeto no se re-ancla.
        if (const auto ov = (*s)["off"].value<std::string>()) {
            int ox = 0, oy = 0;
            std::sscanf(ov->c_str(), "%d,%d", &ox, &oy);
            d.off_x = (int16_t)ox;
            d.off_y = (int16_t)oy;
        }
        d.font_id = parse_hex(*s, "font");
        d.ch      = (*s)["ch"].value<std::string>().value_or(std::string());
        // Tinte E1: ref = "r,g,b" (ausente = {0,0,0} → sin tinte).
        if (const auto rv = (*s)["ref"].value<std::string>(); rv && !rv->empty()) {
            const char* p2 = rv->c_str();
            char* end = nullptr;
            for (int c = 0; c < 3; ++c) {
                d.ref_rgb[c] = uint8_t(std::strtol(p2, &end, 10));
                if (!end || *end != ',') break;
                p2 = end + 1;
            }
        }
        // tiles = "0xhash:cx,cy|0xhash:cx,cy|…"
        const auto tiles = (*s)["tiles"].value<std::string>();
        if (!tiles || tiles->empty()) continue;
        const char* p = tiles->c_str();
        while (*p) {
            char* end = nullptr;
            const uint64_t h = std::strtoull(p, &end, 0);
            if (end == p) break;
            p = end;
            PackPlaneSetMember m;
            m.hash = h;
            if (*p == ':') {
                ++p;
                m.cx = int16_t(std::strtol(p, &end, 10)); p = end;
                if (*p == ',') { ++p; m.cy = int16_t(std::strtol(p, &end, 10)); p = end; }
            }
            if (m.hash) d.members.push_back(m);
            while (*p && *p != '|') ++p;      // saltea flips legacy u otra cola
            if (*p == '|') ++p;
        }
        if (d.members.empty() || d.members.size() > kMaxPlaneSetMembers) continue;
        if (!d.w_cells || !d.h_cells) {       // bbox derivable si el pack no lo trae
            int16_t mx = 0, my = 0;
            for (const auto& m : d.members) {
                if (m.cx > mx) mx = m.cx;
                if (m.cy > my) my = m.cy;
            }
            d.w_cells = uint16_t(mx + 1);
            d.h_cells = uint16_t(my + 1);
        }
        out_sets.push_back(std::move(d));
        ++loaded;
    }
    return loaded;
}

// -- screens.toml (CUADRO · CU001) -------------------------------------------

std::string bake_screens_toml(const std::vector<PackScreen>& screens) {
    std::string out =
        "# screens.toml — CUADROS: pantalla estática completa reemplazada por UN\n"
        "# asset HD. El reconocimiento es por COBERTURA: `min_match` de las celdas\n"
        "# declaradas presentes en su posición exacta, y a lo sumo `max_extra` de\n"
        "# celdas ajenas — sin ese segundo guarda, una pantalla que CONTIENE a\n"
        "# esta matchearía con cobertura perfecta.\n"
        "# `planes` es la máscara de capas que COMPONEN el Cuadro (bit0=A ·\n"
        "# bit1=B · bit2=Window): uno de una sola capa se sigue reconociendo\n"
        "# aunque las otras cambien.\n";
    char buf[320];
    for (const PackScreen& sc : screens) {
        if (!sc.id || sc.asset.empty() || sc.cells.empty()) continue;
        out += "\n[[screen]]\n";
        append_hex(out, "id", sc.id);
        std::snprintf(buf, sizeof(buf),
                      "name = \"%s\"\nplanes = %u\nmin_match = %.3f\n"
                      "max_extra = %.3f\nasset = \"%s\"\n",
                      sc.name.c_str(), unsigned(sc.plane_mask),
                      double(sc.min_match), double(sc.max_extra), sc.asset.c_str());
        out += buf;
        out += "cells = \"";
        for (size_t i = 0; i < sc.cells.size(); ++i) {
            const PackScreenCell& c = sc.cells[i];
            std::snprintf(buf, sizeof(buf), "%s0x%016" PRIx64 ":%u,%u,%u",
                          i ? "|" : "", c.hash, unsigned(c.plane),
                          unsigned(c.col), unsigned(c.row));
            out += buf;
        }
        out += "\"\n";
    }
    return out;
}

size_t parse_screens_toml(const std::string& text, std::vector<PackScreen>& out) {
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] screens.toml: parse error: %s\n", e.what());
        return 0;
    }
    return parse_screens_from(tbl, out);
}

size_t parse_screens_from(const toml::table& tbl, std::vector<PackScreen>& out) {
    const toml::array* ss = tbl["screen"].as_array();
    if (!ss) return 0;
    size_t loaded = 0;
    for (const toml::node& n : *ss) {
        const toml::table* t = n.as_table();
        if (!t) continue;
        PackScreen d;
        d.id = parse_hex(*t, "id");
        const auto asset = (*t)["asset"].value<std::string>();
        if (!d.id || !asset || asset->empty()) continue;
        d.asset      = *asset;
        d.name       = (*t)["name"].value<std::string>().value_or(std::string());
        d.plane_mask = uint8_t((*t)["planes"].value<int64_t>().value_or(7) & 7);
        d.min_match  = float((*t)["min_match"].value<double>().value_or(0.92));
        d.max_extra  = float((*t)["max_extra"].value<double>().value_or(0.08));
        const auto cells = (*t)["cells"].value<std::string>();
        if (!cells || cells->empty()) continue;
        const char* p = cells->c_str();
        while (*p) {
            char* end = nullptr;
            const uint64_t h = std::strtoull(p, &end, 0);
            if (end == p) break;
            p = end;
            PackScreenCell c{ h, 0, 0, 0 };
            if (*p == ':') {
                ++p;
                c.plane = uint8_t(std::strtol(p, &end, 10) & 3); p = end;
                if (*p == ',') { ++p; c.col = uint8_t(std::strtol(p, &end, 10)); p = end; }
                if (*p == ',') { ++p; c.row = uint8_t(std::strtol(p, &end, 10)); p = end; }
            }
            if (c.hash) d.cells.push_back(c);
            while (*p && *p != '|') ++p;
            if (*p == '|') ++p;
        }
        if (d.cells.empty()) continue;
        out.push_back(std::move(d));
        ++loaded;
    }
    return loaded;
}

// -- panoramas.toml (PANORÁMICA · CU003) -------------------------------------

std::string bake_panoramas_toml(const std::vector<PackPanorama>& pans) {
    std::string out =
        "# panoramas.toml — PANORÁMICAS: la tira del nivel de una capa, en HD.\n"
        "# Archivo propio y no una entrada más del catálogo: una tira son miles\n"
        "# de celdas (~1 MB en un nivel largo), y meterla con los elementos haría\n"
        "# que renombrar un glifo reescribiera todo eso.\n"
        "# El reconocimiento es por ANCLAJE: cada celda visible cuyo hash está en\n"
        "# la tira vota dónde está la cámara, y gana la moda. Por eso conviene\n"
        "# MÁS celdas, al revés que una Utilería (que las exige todas presentes).\n"
        "# `cells` lleva una entrada por FILA: \"ly: lx:hash|lx:hash|…\". El lx va\n"
        "# explícito para que la fila pueda tener huecos, y ambos con SIGNO: el\n"
        "# origen es el mínimo observado y hay celdas por delante de él.\n";
    char buf[320];
    for (const PackPanorama& p : pans) {
        if (!p.id || p.cells.empty()) continue;   // el asset puede faltar: el
                                                  // anclaje funciona igual y
                                                  // define_panorama no lo exige
        out += "\n[[panorama]]\n";
        append_hex(out, "id", p.id);
        // Backslashes de Windows en un basic string TOML son escapes inválidos
        // (\U…) → toml::parse tira al releer y parse_panoramas_toml devuelve 0:
        // el proyecto pierde todas las tiras (la «Demo» de GA, 2026-07-31).
        // Mismo pathq que catalog_io/actors.toml. name y asset van por
        // std::string y NO por el buf fijo: un path largo se truncaba mudo.
        std::string asset = p.asset;
        for (char& c : asset) if (c == '\\') c = '/';
        out += "name = \"" + p.name + "\"\n";
        std::snprintf(buf, sizeof(buf),
                      "plane = %u\norigin = \"%d,%d\"\nsize = \"%ux%u\"\n",
                      unsigned(p.plane), p.origin_x, p.origin_y,
                      unsigned(p.w_cells), unsigned(p.h_cells));
        out += buf;
        out += "asset = \"" + asset + "\"\n";

        // Agrupar por fila conservando el orden de llegada de las filas. Las
        // celdas ya vienen ordenadas (bg_cells garantiza fila, luego columna),
        // así que basta con cortar cuando cambia ly.
        out.reserve(out.size() + p.cells.size() * 26);
        out += "cells = [\n";
        size_t i = 0;
        while (i < p.cells.size()) {
            const int32_t ly = p.cells[i].ly;
            std::snprintf(buf, sizeof(buf), "  \"%d:", ly);
            out += buf;
            bool first = true;
            for (; i < p.cells.size() && p.cells[i].ly == ly; ++i) {
                std::snprintf(buf, sizeof(buf), "%s%d,0x%016" PRIx64,
                              first ? "" : "|", p.cells[i].lx, p.cells[i].hash);
                out += buf;
                first = false;
            }
            out += "\",\n";
        }
        out += "]\n";
    }
    return out;
}

size_t parse_panoramas_toml(const std::string& text, std::vector<PackPanorama>& out) {
    toml::table tbl;
    // parse_error CON AVISO — un TOML roto acá perdía TODAS las tiras del
    // proyecto sin dejar rastro (la Panorámica «Demo», 2026-07-31).
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] panoramas.toml: parse error: %s\n", e.what());
        return 0;
    }
    return parse_panoramas_from(tbl, out);
}

size_t parse_panoramas_from(const toml::table& tbl, std::vector<PackPanorama>& out) {
    const auto* arr = tbl["panorama"].as_array();
    if (!arr) return 0;
    size_t loaded = 0;
    for (const auto& node : *arr) {
        const auto* t = node.as_table();
        if (!t) continue;
        PackPanorama d;
        d.id = parse_hex(*t, "id");
        if (!d.id) continue;
        d.name  = (*t)["name"].value_or(std::string{});
        d.plane = uint8_t((*t)["plane"].value_or(0));
        if (d.plane > 2) d.plane = 0;
        d.asset = (*t)["asset"].value_or(std::string{});
        {   // origin = "x,y" — con signo
            const std::string o = (*t)["origin"].value_or(std::string{});
            const size_t c = o.find(',');
            if (c != std::string::npos) {
                d.origin_x = int32_t(std::strtol(o.c_str(), nullptr, 10));
                d.origin_y = int32_t(std::strtol(o.c_str() + c + 1, nullptr, 10));
            }
        }
        {   // size = "WxH"
            const std::string s = (*t)["size"].value_or(std::string{});
            const size_t x = s.find('x');
            if (x != std::string::npos) {
                d.w_cells = uint16_t(std::strtoul(s.c_str(), nullptr, 10));
                d.h_cells = uint16_t(std::strtoul(s.c_str() + x + 1, nullptr, 10));
            }
        }
        const auto* rows = (*t)["cells"].as_array();
        if (!rows) continue;
        for (const auto& rn : *rows) {
            const auto* rs = rn.value<std::string>() ? &rn : nullptr;
            if (!rs) continue;
            const std::string row = rn.value_or(std::string{});
            const size_t colon = row.find(':');
            if (colon == std::string::npos) continue;
            const int32_t ly = int32_t(std::strtol(row.c_str(), nullptr, 10));
            // Pointer-walk, no substr por celda: el reader del catálogo hace
            // una asignación de heap por celda y acá son decenas de miles.
            const char* p = row.c_str() + colon + 1;
            while (*p) {
                char* end = nullptr;
                const long lx = std::strtol(p, &end, 10);
                if (end == p || *end != ',') break;
                p = end + 1;
                if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) break;
                const uint64_t h = std::strtoull(p + 2, &end, 16);
                if (end == p + 2) break;
                d.cells.push_back({ h, int32_t(lx), ly });
                p = (*end == '|') ? end + 1 : end;
                if (*p == '\0') break;
                if (*(p - 1) != '|') break;
            }
        }
        if (d.cells.empty()) continue;
        out.push_back(std::move(d));
        ++loaded;
    }
    return loaded;
}

// -- kinematics.toml (CINEMÁTICA · CU004) ------------------------------------

std::string bake_kinematics_toml(const std::vector<PackKinematic>& kins) {
    std::string out =
        "# kinematics.toml — CINEMÁTICAS: una SECUENCIA ORDENADA de Cuadros.\n"
        "# Lo que aporta sobre Cuadros sueltos no es el dibujo sino el ORDEN:\n"
        "# desambigua dos pantallas idénticas que aparecen en cinemáticas\n"
        "# distintas, y permite cancelar en el acto si el jugador se sale de la\n"
        "# secuencia. Se puede entrar en CUALQUIER paso — la posición sale del\n"
        "# contenido de la pantalla, no de un contador.\n"
        "# `gap` = frames tolerados sin Cuadro confirmado antes de cancelar. NO\n"
        "# puede ser 0: el match cae a 0 durante un frame en toda transición\n"
        "# limpia, porque la histéresis pide dos frames para confirmar el nuevo.\n"
        "# `steps` = pipe-list de `0xid` (o `0xid:asset` para reemplazar ese paso\n"
        "# con un asset propio en vez del del Cuadro).\n"
        "# Si el asset es un `.ivf`, `0xid:asset@N` fija en qué frame del clip\n"
        "# arranca ese paso — así UN video cubre varios pasos y entrar por el\n"
        "# medio de la secuencia cae en el plano correcto. Se omite cuando es 0.\n"
        "# `loop` = el video CICLA si es más corto que el tramo; el default es\n"
        "# SOSTENER su último frame (rebobinar una escena narrada a mitad de\n"
        "# camino es un defecto, no una función).\n"
        "# `audio` = pista de audio del video, como asset aparte: el IVF es sólo\n"
        "# video. Vacío = la Cinemática suena con el audio del juego.\n"
        "# `gain` = volumen de la pista del video · `game_gain` = ducking de la\n"
        "# banda sonora del juego mientras corre la Cinemática. Ambos 1.0 por\n"
        "# defecto, y sólo viajan si el autor los movió ().\n";
    char buf[320];
    for (const PackKinematic& k : kins) {
        // Menos de dos pasos no es una secuencia: es un Cuadro con otro nombre.
        if (!k.id || k.steps.size() < 2) continue;
        out += "\n[[kinematic]]\n";
        append_hex(out, "id", k.id);
        std::snprintf(buf, sizeof(buf), "name = \"%s\"\ngap = %u\n",
                      k.name.c_str(), unsigned(k.gap_frames));
        out += buf;
        // Sólo si difieren del default: ningún pack ya horneado cambia un byte
        // (mismo criterio que el resto de los bakers).
        if (k.loop)           out += "loop = true\n";
        if (!k.audio.empty()) out += "audio = \"" + k.audio + "\"\n";
        // : los dos gain se autoraban, se asignaban y el parser sabía
        // leerlos — pero este writer nunca los emitía, así que el dato moría
        // en el bake. Van con el mismo criterio que `loop` y `audio`: sólo si
        // difieren del default (1.0), para que un pack ya horneado no cambie
        // un byte.
        if (k.gain != 1.0f) {
            std::snprintf(buf, sizeof(buf), "gain = %g\n", double(k.gain));
            out += buf;
        }
        if (k.game_gain != 1.0f) {
            std::snprintf(buf, sizeof(buf), "game_gain = %g\n",
                          double(k.game_gain));
            out += buf;
        }
        out += "steps = \"";
        for (size_t i = 0; i < k.steps.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s0x%016" PRIx64,
                          i ? "|" : "", k.steps[i].screen_id);
            out += buf;
            if (!k.steps[i].asset.empty()) {
                out += ":"; out += k.steps[i].asset;
                // El `@offset` SÓLO si no es 0: así un proyecto ya autorado se
                // hornea byte por byte igual que antes de que el video existiera.
                if (k.steps[i].video_offset) {
                    std::snprintf(buf, sizeof(buf), "@%u",
                                  unsigned(k.steps[i].video_offset));
                    out += buf;
                }
            }
        }
        out += "\"\n";
    }
    return out;
}

size_t parse_instruments_toml(const std::string& text, std::vector<PackInstrument>& out) {
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] instruments.toml: parse error: %s\n", e.what());
        return 0;
    }
    const auto* arr = tbl["instrument"].as_array();
    if (!arr) return 0;
    size_t n = 0;
    for (const auto& node : *arr) {
        const auto* t = node.as_table();
        if (!t) continue;
        PackInstrument d;
        // El patch viaja como cadena hexadecimal ("0x00000000deadbeef") por lo
        // mismo que el resto de los ids del pack: un entero TOML es i64 con
        // signo y los ids usan los 64 bits.
        const std::string ph = (*t)["patch"].value_or(std::string{});
        d.patch = ph.empty() ? 0 : std::strtoull(ph.c_str(), nullptr, 0);
        d.soundfont = (*t)["soundfont"].value_or(std::string{});
        if (!d.patch || d.soundfont.empty()) continue;
        d.bank      = uint16_t((*t)["bank"].value_or(0));
        d.preset    = uint16_t((*t)["preset"].value_or(0));
        d.transpose = int8_t((*t)["transpose"].value_or(0));
        const double g = (*t)["gain"].value_or(1.0);
        d.gain = g > 0.0 ? float(g) : 1.0f;
        out.push_back(std::move(d));
        ++n;
    }
    return n;
}

size_t parse_kinematics_toml(const std::string& text, std::vector<PackKinematic>& out) {
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] kinematics.toml: parse error: %s\n", e.what());
        return 0;
    }
    return parse_kinematics_from(tbl, out);
}

size_t parse_kinematics_from(const toml::table& tbl, std::vector<PackKinematic>& out) {
    const auto* arr = tbl["kinematic"].as_array();
    if (!arr) return 0;
    size_t loaded = 0;
    for (const auto& node : *arr) {
        const auto* t = node.as_table();
        if (!t) continue;
        PackKinematic d;
        d.id = parse_hex(*t, "id");
        if (!d.id) continue;
        d.name = (*t)["name"].value_or(std::string{});
        d.gap_frames = uint32_t((*t)["gap"].value_or(12));
        d.loop  = (*t)["loop"].value_or(false);
        d.audio = (*t)["audio"].value_or(std::string{});
        d.gain      = float((*t)["gain"].value_or(1.0));
        d.game_gain = float((*t)["game_gain"].value_or(1.0));
        const std::string st = (*t)["steps"].value_or(std::string{});
        size_t p = 0;
        while (p < st.size()) {
            size_t bar = st.find('|', p);
            if (bar == std::string::npos) bar = st.size();
            const std::string tok = st.substr(p, bar - p);
            const size_t colon = tok.find(':');
            PackKinematicStep s;
            s.screen_id = std::strtoull(tok.c_str(), nullptr, 0);
            if (colon != std::string::npos) {
                s.asset = tok.substr(colon + 1);
                // `@offset` (). Se lee DESDE LA DERECHA y sólo se acepta si
                // lo que sigue al `@` son dígitos hasta el final: una ruta puede
                // contener `@` legítimamente, y partir por el primero rompería
                // el nombre del asset. Si no cumple, no hay offset y el `@`
                // queda donde estaba, que es parte del nombre.
                const size_t at = s.asset.rfind('@');
                if (at != std::string::npos && at + 1 < s.asset.size()) {
                    bool digits = true;
                    for (size_t q = at + 1; q < s.asset.size(); ++q)
                        if (s.asset[q] < '0' || s.asset[q] > '9') { digits = false; break; }
                    if (digits) {
                        s.video_offset = uint32_t(std::strtoul(
                            s.asset.c_str() + at + 1, nullptr, 10));
                        s.asset.resize(at);
                    }
                }
            }
            if (s.screen_id) d.steps.push_back(std::move(s));
            p = bar + 1;
        }
        if (d.steps.size() < 2) continue;
        out.push_back(std::move(d));
        ++loaded;
    }
    return loaded;
}

// -- plane_sequences.toml (ANIMACIÓN ) -----------------------------------

uint32_t plane_sequence_total(const uint16_t* durations, uint32_t n,
                              uint16_t default_dur) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; ++i)
        total += durations[i] ? durations[i] : default_dur;
    return total;
}

uint32_t plane_sequence_step_at(const uint16_t* durations, uint32_t n,
                                uint64_t t, uint16_t default_dur) {
    if (!durations || n == 0) return 0;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; ++i) {
        acc += durations[i] ? durations[i] : default_dur;
        if (t < acc) return i;
    }
    return n - 1;
}

std::string bake_plane_sequences_toml(const std::vector<PackPlaneSequence>& seqs) {
    std::string out =
        "# plane_sequences.toml — ANIMACIÓN: una secuencia de Objetos (plane\n"
        "# sets) con RELOJ PROPIO. No es lo mismo que animations.toml, que son\n"
        "# las ACCIONES (poses HD en fase de un Actor).\n"
        "#\n"
        "# Lo que aporta sobre un plane set suelto: el set ya sustituye por\n"
        "# hash, y eso alcanza cuando el juego anima y cada fase tiene su hash.\n"
        "# La Animación es para lo que eso NO puede — que el HD tenga más fases\n"
        "# que el original, o corra a otra cadencia. Cuando CUALQUIER paso\n"
        "# matchea, esa posición la toma la secuencia y se dibuja el asset del\n"
        "# paso vigente por reloj, no el del que matcheó.\n"
        "#\n"
        "# `steps` = pipe-list de `0xid` · `0xid:asset` (asset propio del paso)\n"
        "# · `0xid~N` (cadencia en frames de juego) — mismo dialecto que los\n"
        "# pasos de kinematics.toml, con `~dur` en vez de `@offset`. Se omiten\n"
        "# los defaults.\n";
    char buf[320];
    for (const PackPlaneSequence& q : seqs) {
        // Menos de dos pasos es el Objeto solo: nada que ciclar.
        if (!q.id || q.steps.size() < 2) continue;
        out += "\n[[sequence]]\n";
        append_hex(out, "id", q.id);
        std::snprintf(buf, sizeof(buf), "name = \"%s\"\n", q.name.c_str());
        out += buf;
        out += "steps = \"";
        for (size_t i = 0; i < q.steps.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s0x%016" PRIx64,
                          i ? "|" : "", q.steps[i].set_id);
            out += buf;
            if (!q.steps[i].asset.empty()) out += ":" + q.steps[i].asset;
            if (q.steps[i].duration) {
                std::snprintf(buf, sizeof(buf), "~%u", unsigned(q.steps[i].duration));
                out += buf;
            }
        }
        out += "\"\n";
    }
    return out;
}

size_t parse_plane_sequences_toml(const std::string& text,
                                  std::vector<PackPlaneSequence>& out) {
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] plane_sequences.toml: parse error: %s\n",
                     e.what());
        return 0;
    }
    return parse_plane_sequences_from(tbl, out);
}

size_t parse_plane_sequences_from(const toml::table& tbl,
                                  std::vector<PackPlaneSequence>& out) {
    const auto* arr = tbl["sequence"].as_array();
    if (!arr) return 0;
    size_t loaded = 0;
    for (const auto& node : *arr) {
        const auto* t = node.as_table();
        if (!t) continue;
        PackPlaneSequence q;
        q.id = parse_hex(*t, "id");
        if (!q.id) continue;
        q.name = (*t)["name"].value_or(std::string{});
        const std::string st = (*t)["steps"].value_or(std::string{});
        size_t p = 0;
        while (p < st.size()) {
            size_t bar = st.find('|', p);
            if (bar == std::string::npos) bar = st.size();
            std::string tok = st.substr(p, bar - p);
            PackPlaneSeqStep s;
            // `~dur` DESDE LA DERECHA y sólo si son dígitos hasta el final:
            // una ruta puede contener `~` (los nombres cortos 8.3 de Windows
            // lo usan) y partir por el primero rompería el nombre del asset.
            // Mismo criterio que el `@offset` de kinematics.toml.
            const size_t tl = tok.rfind('~');
            if (tl != std::string::npos && tl + 1 < tok.size()) {
                bool digits = true;
                for (size_t q2 = tl + 1; q2 < tok.size(); ++q2)
                    if (tok[q2] < '0' || tok[q2] > '9') { digits = false; break; }
                if (digits) {
                    s.duration = uint16_t(std::strtoul(tok.c_str() + tl + 1, nullptr, 10));
                    tok.resize(tl);
                }
            }
            const size_t colon = tok.find(':');
            s.set_id = std::strtoull(tok.c_str(), nullptr, 0);
            if (colon != std::string::npos) s.asset = tok.substr(colon + 1);
            if (s.set_id) q.steps.push_back(std::move(s));
            p = bar + 1;
        }
        if (q.steps.size() < 2) continue;
        out.push_back(std::move(q));
        ++loaded;
    }
    return loaded;
}

// -- [[enhance]] () — «Mejorar por software», ya expandido a (capa, hash) --

std::string bake_enhance_toml(const std::vector<PackEnhance>& enh) {
    std::string out;
    if (enh.empty()) return out;
    out +=
        "\n# [[enhance]] (): Identidades marcadas «Mejorar por software» — el\n"
        "# motor mejora el gráfico ORIGINAL con un filtro (EPX sobre índices de\n"
        "# paleta) en ejecución, sin asset externo. Recomendado para logos, avisos\n"
        "# de copyright y otras marcas. Ya viene EXPANDIDO a la identidad del motor:\n"
        "# `layer` = capa de SceneElement (0=B · 1=A · 2=Window · 3=Sprite) y\n"
        "# `hashes` = pipe-list de hashes flip-invariantes. Otras apariciones del\n"
        "# mismo (capa, hash) también se mejoran; lo que reclama un HD no. Un player\n"
        "# viejo ignora este array y degrada a original. `k` () = intensidad\n"
        "# del suavizado 0..255 (ausente = 255, vector limpio; 0 = bordes al 50 %).\n";
    char buf[64];
    for (const PackEnhance& e : enh) {
        if (e.hashes.empty() || e.layer > 3) continue;
        out += "\n[[enhance]]\n";
        append_hex(out, "id", e.id);
        std::snprintf(buf, sizeof(buf), "name = \"%s\"\n", e.name.c_str());
        out += buf;
        std::snprintf(buf, sizeof(buf), "layer = %u\n", unsigned(e.layer));
        out += buf;
        if (e.k != 255) {   // : solo no-default (byte-idéntico)
            std::snprintf(buf, sizeof(buf), "k = %u\n", unsigned(e.k));
            out += buf;
        }
        out += "hashes = \"";
        for (size_t i = 0; i < e.hashes.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s0x%016" PRIx64, i ? "|" : "", e.hashes[i]);
            out += buf;
        }
        out += "\"\n";
    }
    return out;
}

size_t parse_enhance_toml(const std::string& text, std::vector<PackEnhance>& out) {
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        std::fprintf(stderr, "[Componentes] enhance: parse error: %s\n", e.what());
        return 0;
    }
    return parse_enhance_from(tbl, out);
}

size_t parse_enhance_from(const toml::table& tbl, std::vector<PackEnhance>& out) {
    const auto* arr = tbl["enhance"].as_array();
    if (!arr) return 0;
    size_t loaded = 0;
    for (const auto& node : *arr) {
        const auto* t = node.as_table();
        if (!t) continue;
        PackEnhance e;
        e.id   = parse_hex(*t, "id");
        e.name = (*t)["name"].value_or(std::string{});
        const int64_t layer = (*t)["layer"].value_or(int64_t(3));
        if (layer < 0 || layer > 3) continue;
        e.layer = uint8_t(layer);
        {   // : k fuera de rango se clampea (un valor raro no apaga la mejora)
            const int64_t k = (*t)["k"].value_or(int64_t(255));
            e.k = uint8_t(k < 0 ? 0 : k > 255 ? 255 : k);
        }
        const std::string hs = (*t)["hashes"].value_or(std::string{});
        size_t p = 0;
        while (p < hs.size()) {
            size_t bar = hs.find('|', p);
            if (bar == std::string::npos) bar = hs.size();
            const uint64_t h = std::strtoull(hs.substr(p, bar - p).c_str(), nullptr, 0);
            if (h) e.hashes.push_back(h);
            p = bar + 1;
        }
        if (e.hashes.empty()) continue;
        out.push_back(std::move(e));
        ++loaded;
    }
    return loaded;
}

// -- elements.toml () — TODAS las Identidades de Pintar, un solo documento -

std::string bake_elements_toml(const std::vector<PackScreen>& screens,
                               const std::vector<PackPanorama>& pans,
                               const std::vector<PackKinematic>& kins,
                               const std::vector<PackPlaneSequence>& seqs,
                               const std::vector<PackPlaneSet>& sets,
                               const std::vector<PackPlaneFont>& fonts,
                               const std::vector<PackEnhance>& enh) {
    std::string out =
        "# elements.toml — las Identidades autorables de Pintar, en UN documento.\n"
        "#\n"
        "# Antes esto eran cinco archivos —screens · panoramas · kinematics ·\n"
        "# plane_sequences · plane_sets— y los cinco guardaban la MISMA familia:\n"
        "# lo que el artista autora como Cuadro, Panorámica, Cinemática,\n"
        "# Animación, Utilería, Carácter o UI. Lo que los separaba no era la\n"
        "# Identidad sino el MECANISMO del motor que las sirve, o sea un detalle\n"
        "# de implementación decidiendo el layout de la entrega ().\n"
        "#\n"
        "# Acá el mecanismo es el nombre del array —[[screen]], [[panorama]],\n"
        "# [[kinematic]], [[sequence]], [[set]], [[font]]— y no un archivo. Cada\n"
        "# uno conserva su forma exacta: este documento es la CONCATENACIÓN de lo\n"
        "# que antes salía por separado, así que los parsers no cambiaron y un\n"
        "# pack viejo se sigue leyendo por el camino de siempre.\n";
    // Orden fijo (y no el de llegada) para que el mismo proyecto hornee los
    // mismos bytes: el .ay tiene que ser reproducible — ver pack_layout_smoke.
    out += bake_screens_toml(screens);
    out += bake_panoramas_toml(pans);
    out += bake_kinematics_toml(kins);
    out += bake_plane_sequences_toml(seqs);
    out += bake_plane_sets_toml(sets, fonts);
    // : al FINAL y sólo si hay marcas — un pack sin «Mejorar por software»
    // hornea exactamente los mismos bytes que antes de la feature.
    out += bake_enhance_toml(enh);
    return out;
}

size_t parse_elements_toml(const std::string& text,
                           std::vector<PackScreen>& screens,
                           std::vector<PackPanorama>& pans,
                           std::vector<PackKinematic>& kins,
                           std::vector<PackPlaneSequence>& seqs,
                           std::vector<PackPlaneSet>& sets,
                           std::vector<PackPlaneFont>& fonts,
                           std::vector<PackEnhance>* enh) {
    // UNA sola pasada de toml::parse. Delegar en los cinco `parse_*_toml`
    // habría sido dos líneas, pero parsea el mismo documento cinco veces — y
    // una Panorámica larga es ~1 MB de celdas, así que eso es tiempo de
    // apertura de la escena, no una prolijidad.
    toml::table tbl;
    try { tbl = toml::parse(text); }
    catch (const toml::parse_error& e) {
        // Un solo documento significa que un error de sintaxis se lleva puestas
        // TODAS las Identidades, no una familia. Por eso avisa fuerte: el
        // antecedente es la Panorámica «Demo» de 2026-07-31, que desapareció
        // sin dejar rastro.
        std::fprintf(stderr, "[Componentes] elements.toml: parse error: %s\n", e.what());
        return 0;
    }
    size_t n = 0;
    n += parse_screens_from(tbl, screens);
    n += parse_panoramas_from(tbl, pans);
    n += parse_kinematics_from(tbl, kins);
    n += parse_plane_sequences_from(tbl, seqs);
    n += parse_plane_sets_from(tbl, sets, fonts);
    if (enh) n += parse_enhance_from(tbl, *enh);   // 
    return n;
}

}  // namespace ayther

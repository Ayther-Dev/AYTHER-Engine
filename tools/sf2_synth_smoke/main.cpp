// ---------------------------------------------------------------------------
// sf2_synth_smoke — el sintetizador SoundFont cruza el FFI y suena (#261).
//
// QUÉ VERIFICA, y por qué no alcanza con los tests de Rust:
//
//   Los tests de `core/src/sf2.rs` prueban la clase EN Rust. Este prueba que la
//   misma clase, manejada desde C++ a través del FFI, produce señal — que es
//   como la va a usar el motor. Entre una cosa y la otra hay una frontera donde
//   se rompen los punteros crudos, los tamaños de buffer y el orden de canales,
//   y ninguno de esos fallos aparece del lado Rust.
//
//   Además cubre el caso que el motor SÍ va a encontrar: un handle NULO (sin
//   SoundFont asignado, o un SF2 que no cargó). Ahí el render tiene que escribir
//   SILENCIO y no dejar el buffer intacto — el llamador lo encola igual, y con
//   basura sonaría ruido blanco a todo volumen.
//
// Uso:  sf2_synth_smoke <archivo.sf2>
// Sin argumento verifica sólo el camino nulo y sale verde: el ctest tiene que
// correr en una máquina sin SoundFonts.
// ---------------------------------------------------------------------------
#include "ayther_core_ffi.h"
#include "ayther_file.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

std::vector<uint8_t> slurp(const char* path) {
    std::vector<uint8_t> out;
    std::FILE* f = ayther::file_open(path, "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

float peak(const std::vector<float>& b) {
    float p = 0.0f;
    for (float v : b) { const float a = v < 0 ? -v : v; if (a > p) p = a; }
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("=== sf2_synth_smoke — el sintetizador cruza el FFI (#261) ===\n");

    // ---- El camino NULO, que es el que el motor va a encontrar de verdad ----
    {
        std::vector<float> buf(2048, 1.0f);   // sembrado con basura a propósito
        ayther_sf2_render(nullptr, buf.data(), buf.size() / 2);
        check(peak(buf) == 0.0f,
              "con handle nulo el render escribe SILENCIO (no deja basura)");
        // Y que las demás no exploten con nulo: el motor las llama sin saber si
        // hay SoundFont cargado.
        ayther_sf2_note_on(nullptr, 0, 60, 100);
        ayther_sf2_note_off(nullptr, 0, 60);
        ayther_sf2_program(nullptr, 0, 3);
        ayther_sf2_all_notes_off(nullptr);
        ayther_sf2_free(nullptr);
        check(true, "las operaciones con handle nulo no rompen");
    }

    if (argc < 2) {
        std::printf("[skip] sin .sf2 — pasar uno como argumento para verificar "
                    "que SUENA a través del FFI\n");
        std::printf("%s\n", g_fail ? "=== FAIL ===" : "=== ok ===");
        return g_fail ? 1 : 0;
    }

    const std::vector<uint8_t> sf2 = slurp(argv[1]);
    check(!sf2.empty(), std::string("se pudo leer ") + argv[1]);
    if (sf2.empty()) return 1;

    // ---- Listar presets sin cargar (lo que alimenta la biblioteca) ---------
    {
        std::vector<uint16_t> bank(64), preset(64);
        const uint32_t total = ayther_sf2_list_presets(
            sf2.data(), sf2.size(), bank.data(), preset.data(), 64);
        std::printf("       %u presets en el archivo\n", total);
        check(total > 0, "list_presets devuelve presets SIN cargar el sintetizador");
    }

    // ---- Cargar y sonar ---------------------------------------------------
    AytherSf2* s = ayther_sf2_new(sf2.data(), sf2.size(), 44100);
    check(s != nullptr, "ayther_sf2_new abre el SoundFont");
    if (!s) return 1;

    // Silencio antes de tocar: si esto ya trae señal, lo que mide el paso
    // siguiente no es la nota.
    std::vector<float> quiet(4096, 0.0f);
    ayther_sf2_render(s, quiet.data(), quiet.size() / 2);
    check(peak(quiet) < 1e-6f, "sin notas, silencio");

    ayther_sf2_note_on(s, 0, 60, 100);
    std::vector<float> buf(8192, 0.0f);
    ayther_sf2_render(s, buf.data(), buf.size() / 2);
    const float p = peak(buf);
    std::printf("       pico con nota: %.4f\n", p);
    check(p > 1e-3f, "una nota produce señal A TRAVÉS DEL FFI");

    // El corte inmediato: es lo que el motor usa cuando el juego cambia de
    // escena, y una nota colgada ahí sonaría sobre lo que sigue.
    ayther_sf2_all_notes_off(s);
    std::vector<float> tail(32768, 0.0f);
    ayther_sf2_render(s, tail.data(), tail.size() / 2);
    std::vector<float> last(tail.end() - 2048, tail.end());
    check(peak(last) < 1e-3f, "tras all_notes_off el final calla");

    ayther_sf2_free(s);
    std::printf("%s\n", g_fail ? "=== FAIL ===" : "=== ok ===");
    return g_fail ? 1 : 0;
}

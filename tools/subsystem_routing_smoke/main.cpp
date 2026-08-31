// ---------------------------------------------------------------------------
// subsystem_routing_smoke — #292: apagar UN subsistema devuelve el original, y
// no se lleva puestos a los demás.
//
// POR QUÉ ESTE ORÁCULO Y NO OTRO. Lo que hace peligroso al routing no es que un
// toggle no funcione —eso se ve enseguida— sino que funcione DE MÁS: apagar los
// metasprites y que desaparezcan también los sprites sueltos que quedaban
// debajo, o apagar la música y que se vaya un efecto. Es un defecto que en
// pantalla parece «el pack está roto» y que sólo se nota comparando.
//
// Por eso cada check afirma DOS cosas: que lo apagado se apagó y que lo de al
// lado siguió. Un check que sólo mira lo primero pasa con un routing que apaga
// todo junto.
//
// EL ESTÍMULO. La ROM sintética del repo (tools/common/synth_rom.h) dibuja una
// celda de plano A y un sprite en coordenadas conocidas, y hace sonar FM y PSG.
// Los assets HD no hacen falta: lo que se mide es si el motor RESUELVE la
// sustitución, y eso se ve en los contadores de la FrameView sin necesidad de
// tener un PNG que poner.
//
// LO QUE NO MIDE: que el pack declare sus subsistemas (eso es #294 y tiene sus
// propios tests en el core) ni la UI del toggle (#297).
// ---------------------------------------------------------------------------
#include "ayther_session.h"

#include "../common/synth_rom.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

}  // namespace

int main() {
    std::printf("=== subsystem_routing_smoke — #292: apagar uno no apaga los otros ===\n");

    const std::string core =
        std::string(AYTHER_SOURCE_DIR) + "/third_party/cores/genesis_plus_gx_libretro_vram.dll";
    if (!std::filesystem::exists(core)) {
        std::printf("[skip] no está el core del fork: %s\n", core.c_str());
        return 77;
    }
    const std::string rom = ayther::synth::canonical_rom_path();
    if (rom.empty()) { std::fprintf(stderr, "[FAIL] no pude escribir la ROM\n"); return 1; }

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto sr = ayther::AytherSession::create(c);
    if (!sr) { std::fprintf(stderr, "[FAIL] no se pudo crear la sesión\n"); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *sr;

    using S = ayther::Subsystem;

    // ---- El default, que es lo que ve una sesión sin frontend --------------
    check(s->subsystem_enabled(S::Sprites) && s->subsystem_enabled(S::Music),
          "DEFAULT: todos los subsistemas arrancan ENCENDIDOS");
    check(s->subsystems_enabled_mask() == (1u << ayther::kSubsystemCount) - 1u,
          "…y la máscara los tiene a todos, sin bits de más");

    // ---- Independencia: apagar uno no toca a los demás --------------------
    s->set_subsystem_enabled(S::Sprites, false);
    check(!s->subsystem_enabled(S::Sprites),
          "apagar Sprites lo apaga");
    check(s->subsystem_enabled(S::Metasprites) && s->subsystem_enabled(S::Tiles) &&
          s->subsystem_enabled(S::Planes) && s->subsystem_enabled(S::Music) &&
          s->subsystem_enabled(S::Sfx),
          "…y NO se lleva puesto a ningún otro (el defecto que importa)");

    s->set_subsystem_enabled(S::Music, false);
    check(!s->subsystem_enabled(S::Music) && s->subsystem_enabled(S::Sfx),
          "apagar Música deja Efectos encendido — son buses distintos");

    // ---- Round-trip de la máscara (serializar la sesión) ------------------
    const uint32_t saved = s->subsystems_enabled_mask();
    s->set_subsystems_enabled_mask((1u << ayther::kSubsystemCount) - 1u);
    check(s->subsystem_enabled(S::Sprites), "reponer la máscara completa enciende todo");
    s->set_subsystems_enabled_mask(saved);
    check(!s->subsystem_enabled(S::Sprites) && !s->subsystem_enabled(S::Music) &&
          s->subsystem_enabled(S::Tiles),
          "…y volver a la guardada restaura EXACTAMENTE lo que estaba");
    check(s->subsystems_enabled_mask() == saved,
          "…con la misma máscara, ida y vuelta");

    // Un bit de más en la entrada no se guarda: si se guardaran los 32, un
    // round-trip devolvería basura y comparar contra «todo encendido» fallaría
    // para siempre.
    s->set_subsystems_enabled_mask(0xFFFFFFFFu);
    check(s->subsystems_enabled_mask() == (1u << ayther::kSubsystemCount) - 1u,
          "la máscara descarta los bits que no son de ningún subsistema");

    // ---- En vivo: el cambio no reinicia nada ------------------------------
    // Correr frames con un subsistema apagado y volver a encenderlo tiene que
    // seguir produciendo frames: el routing es un gate por produce, no un
    // re-armado de estado.
    s->set_subsystems_enabled_mask((1u << ayther::kSubsystemCount) - 1u);
    for (int i = 0; i < 4; ++i) s->step();
    s->set_subsystem_enabled(S::Sprites, false);
    const ayther::FrameView* a = nullptr;
    for (int i = 0; i < 4; ++i) { const auto& fv = s->step(); if (fv.fb_pixels) a = &fv; }
    check(a != nullptr, "con un subsistema apagado la sesión sigue produciendo frames");
    s->set_subsystem_enabled(S::Sprites, true);
    const ayther::FrameView* b = nullptr;
    for (int i = 0; i < 4; ++i) { const auto& fv = s->step(); if (fv.fb_pixels) b = &fv; }
    check(b != nullptr, "…y volver a encenderlo tampoco pide reiniciar la sesión");

    // ---- Buses de audio (#290) --------------------------------------------
    //
    // La distinción que hay que no perder, y que es fácil de confundir con lo
    // de arriba:
    //
    //   apagar el SUBSISTEMA Música = «no sustituyas la música» → suena la del
    //   juego. Silenciar el BUS Música = «no quiero música» → no suena ninguna.
    //
    // Son controles distintos porque son intenciones distintas.
    using Bus = ayther::AudioBus;
    check(s->bus_volume(Bus::Music) == 1.0f && !s->bus_muted(Bus::Music),
          "DEFAULT de los buses: volumen 1.0 y sin silenciar");

    s->set_bus_volume(Bus::Music, 0.5f);
    check(s->bus_volume(Bus::Music) == 0.5f, "bajar el bus de Música lo baja");
    check(s->bus_volume(Bus::Sfx) == 1.0f && s->bus_volume(Bus::Voice) == 1.0f,
          "…y no toca a Efectos ni a Voces");

    // El tope es el mismo que el de la ganancia autorada de una Secuencia
    // (0..2): dos escalas distintas para lo mismo confunden al que mira los dos
    // números juntos.
    s->set_bus_volume(Bus::Music, 9.0f);
    check(s->bus_volume(Bus::Music) == 2.0f, "el volumen se acota a 2.0");
    s->set_bus_volume(Bus::Music, -1.0f);
    check(s->bus_volume(Bus::Music) == 0.0f, "…y no baja de 0");
    s->set_bus_volume(Bus::Music, 1.0f);

    // Bajar el bus mientras suena tiene que alcanzar a lo que YA está sonando.
    // Con la ganancia aplicada sólo al crear el stream, arrastrar el slider no
    // se oiría hasta el próximo disparo — y en una música en loop eso es nunca.
    // Acá se verifica el contrato de la API (el efecto audible lo mide
    // audio_output_smoke, que sí compara RMS).
    s->set_bus_volume(Bus::Music, 0.25f);
    check(s->bus_volume(Bus::Music) == 0.25f,
          "bajar el bus con audio sonando no rompe ni pierde el valor");
    s->set_bus_volume(Bus::Music, 1.0f);

    s->set_bus_muted(Bus::Sfx, true);
    check(s->bus_muted(Bus::Sfx), "silenciar Efectos lo silencia");
    check(!s->bus_muted(Bus::Music) && !s->bus_muted(Bus::Voice),
          "…y deja a los otros sonando (buses independientes)");
    s->set_bus_muted(Bus::Sfx, false);

    // El silencio de un bus es INDEPENDIENTE del routing por subsistema: son
    // ejes distintos y cruzarlos haría que apagar un toggle moviera el otro.
    s->set_subsystem_enabled(S::Music, false);
    check(!s->bus_muted(Bus::Music),
          "apagar el SUBSISTEMA Música no silencia el BUS Música (ejes distintos)");
    s->set_subsystem_enabled(S::Music, true);

    // ---- Disponibilidad: los tres estados ---------------------------------
    // Sin pack no se puede afirmar que un subsistema falte. Es la distinción
    // que evita mostrar «no disponible» sobre algo que nadie midió.
    check(s->subsystem_availability(S::Sprites) == ayther::SubsystemAvailability::Unknown,
          "SIN PACK la disponibilidad es DESCONOCIDA, no «ausente»");

    // ---- Perfiles: un pack LEGACY no se apaga solo (#293) ------------------
    //
    // Este check nació de una regresión que encontró este mismo oráculo: el
    // perfil «completo» se derivaba de `[systems]`, que un pack viejo no
    // declara, y cargarlo apagaba los ocho subsistemas. El pack abría, no
    // fallaba nada, y el juego se veía sin remasterizar.
    //
    // Es la distinción de #294 —ausente ≠ vacío— aplicada al arranque, y por
    // eso se mide acá y no sólo en el parser: lo que importa no es qué dice el
    // manifest sino con qué máscara queda la SESIÓN.
    if (s->has_pack()) {
        check(s->subsystems_enabled_mask() != 0,
              "PACK LEGACY: cargarlo NO deja los subsistemas apagados");
        check(s->profile_count() >= 2,
              "…y aun sin declarar perfiles ofrece «original» + «completo»");
        check(s->profile_id(0) == "original",
              "…con «original» primero, que ningún pack declara");
    }

    // ---- #522: la MEJORA POR SOFTWARE también respeta el routing ----------
    //
    // «Mejorar por software» (#493) no es un subsistema —#292 lista lo que el
    // pack REEMPLAZA y la mejora no reemplaza: procesa el original— pero
    // tampoco puede quedar afuera del gate. Con el perfil «original» del pack,
    // que apaga los ocho, era lo ÚNICO HD que seguía dibujándose: el jugador
    // pedía el juego como era y veía sus sprites suavizados.
    //
    // Se mide sobre el INVENTARIO (`SceneElement.fx_enhance`), que es donde el
    // motor decide — el píxel es cosa del compose y necesita GPU.
    {
        s->set_subsystems_enabled_mask((1u << ayther::kSubsystemCount) - 1u);
        s->set_hd_enabled(true);
        for (int i = 0; i < 4; ++i) s->step();

        std::vector<ayther::SceneElement> inv;
        s->scene_inventory(inv);
        // Un elemento de PLANO y uno de SPRITE del frame vivo (la ROM
        // sintética dibuja los dos), para marcarlos como mejorados.
        uint64_t plane_hash = 0, sprite_hash = 0;
        for (const auto& e : inv) {
            if (!e.hash || e.claimed) continue;
            if (e.layer <= 2 && !plane_hash)  plane_hash  = e.hash;
            if (e.layer == 3 && !sprite_hash) sprite_hash = e.hash;
        }
        check(plane_hash != 0,
              "el inventario trae al menos un elemento de PLANO para mejorar");

        auto enhanced_now = [&](uint64_t want_hash) {
            std::vector<ayther::SceneElement> v;
            s->scene_inventory(v);
            for (const auto& e : v)
                if (e.hash == want_hash && e.fx_enhance) return true;
            return false;
        };

        if (plane_hash) {
            const ayther::AytherSession::EnhancedElement els[] = {
                { plane_hash, /*layer=*/1, /*k=*/255 },
                { plane_hash, /*layer=*/0, /*k=*/255 },
            };
            s->set_enhanced_elements(els, 2);
            check(enhanced_now(plane_hash),
                  "MEJORA: con todo encendido, el elemento marcado sale mejorado");

            // El perfil «original» del pack = los ocho apagados.
            s->set_subsystems_enabled_mask(0);
            check(!enhanced_now(plane_hash),
                  "…con TODOS los subsistemas apagados (perfil «original») NO se mejora");

            // Familia de plano: alcanza con UNO de los tres para que siga.
            s->set_subsystem_enabled(S::Tiles, true);
            check(enhanced_now(plane_hash),
                  "…y vuelve con sólo Tiles encendido (la mejora sigue a su familia)");

            // Apagar la familia AJENA no la toca — el defecto que importa.
            s->set_subsystems_enabled_mask((1u << ayther::kSubsystemCount) - 1u);
            s->set_subsystem_enabled(S::Sprites, false);
            s->set_subsystem_enabled(S::Metasprites, false);
            check(enhanced_now(plane_hash),
                  "…apagar los sprites NO apaga la mejora de un PLANO");

            // Y la llave de luz maestra la apaga aunque el routing esté entero
            // (toggle Assets en Original, export/snapshot sin HD).
            s->set_subsystems_enabled_mask((1u << ayther::kSubsystemCount) - 1u);
            s->set_hd_enabled(false);
            check(!enhanced_now(plane_hash),
                  "…y con el HD apagado tampoco se mejora (Assets en Original)");
            s->set_hd_enabled(true);
            s->set_enhanced_elements(nullptr, 0);
        }
        s->set_subsystems_enabled_mask((1u << ayther::kSubsystemCount) - 1u);
    }

    // ---- Validación de packs (#295) ---------------------------------------
    //
    // Lo que hay que probar no es que valide un pack bueno, sino que un pack
    // MALO no tire nada: la promesa de #295 es «un pack incompatible no puede
    // causar un cierre inesperado», y eso sólo se ve pasándole basura.
    {
        const auto tmp = std::filesystem::temp_directory_path();
        const auto basura = tmp / "ayther_pack_basura.ay";
        { std::FILE* f = ayther::file_open(basura.string().c_str(), "wb");
          if (f) { std::fputs("esto no es un pack", f); std::fclose(f); } }

        const auto r1 = s->validate_pack(basura.string());
        bool has_error = false;
        for (const auto& f : r1) if (f.error) has_error = true;
        check(!r1.empty() && has_error,
              "un archivo que NO es un pack sale como ERROR, no como crash");

        const auto r2 = s->validate_pack((tmp / "no_existe_jamas.ay").string());
        check(!r2.empty(), "…y uno inexistente también da informe (no silencio)");

        // Cada hallazgo trae código Y mensaje: el código es para decidir sin
        // parsear castellano, el mensaje para mostrar.
        bool completos = true;
        for (const auto& f : r1) if (f.code.empty() || f.message.empty()) completos = false;
        check(completos, "cada hallazgo trae código estable y mensaje para el usuario");

        std::error_code ec;
        std::filesystem::remove(basura, ec);
    }

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

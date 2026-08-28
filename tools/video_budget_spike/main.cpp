// ---------------------------------------------------------------------------
// video_budget_spike — ¿entra un frame de video por frame de juego? (issue #263)
//
// El plan de la Cinemática con video anota este riesgo como el que puede hundir
// la fase, y con motivo: el presupuesto de UN upload por frame existe porque un
// memcpy de staging de ~10 MB tarda 4-5 ms y hacía STARVE al audio (la nota
// vive en src/ayther_renderer.cpp). Un video a 60 fps se comería ese
// presupuesto él solo.
//
// Lo que se mide acá es el memcpy de staging, que es el costo que el motor ya
// identificó como dominante: la copia CPU→buffer mapeado que precede a todo
// upload. NO se mide la transferencia PCI ni el decode — el primero depende del
// hardware de cada uno y el segundo es de libvpx. Si el memcpy solo ya no entra
// en el presupuesto, no hace falta seguir; si entra holgado, el riesgo pasa a
// ser de decode y de PCI, que son otra medición.
//
// Las tres variantes miden la decisión de diseño que el plan propone:
//   · RGBA 4K      — lo que NO hay que hacer (33 MB/frame).
//   · RGBA 1080p   — topear la resolución al tier HD, sin tocar el formato.
//   · YUV420 1080p — además subir en el formato NATIVO del decoder (3 planos
//                    R8) y convertir en el fragment shader: 1,5 B/px en vez de
//                    4, y desaparece la conversión en CPU.
//
// Referencia: a 60 fps el frame entero son 16,6 ms, y ahí adentro tienen que
// entrar la emulación, el resto del render y el audio.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target video_budget_spike
//
// ## RESULTADO FINAL, medido en el camino real (2026-07-27)
//
// 132 uploads reales del proyecto Golden Axe, cronometrados dentro de
// pump_uploads: media 3,5 ms, máximo 13,7 ms. O sea que la magnitud de la nota
// del motor (4-5 ms) es CORRECTA — pero la causa que le atribuye, no.
//
// El costo NO ESCALA CON LOS BYTES:
//     anfora.png            0,32 MB  →  4,18 / 5,36 / 5,86 ms
//     Dragon - Attack 02    9,06 MB  →  4,18 ms
//     Longmoan - Walk 05    6,40 MB  →  3,11 ms
// Un asset 29 veces más chico tarda lo mismo o más. El ancho de banda aparente
// va de 0,1 a 2,1 GB/s, o sea que el número no significa nada: es un costo
// FIJO POR OBJETO dividido por el tamaño.
//
// La causa está en finish_upload: cada llamada crea una VkTexture NUEVA con
// cadena de mipmaps, más dos descriptor sets que aloca y actualiza. Eso es
// creación de imagen + reserva de memoria + los blits de la pirámide de mips +
// descriptores. El memcpy de los píxeles —lo que este banco mide— es una
// fracción: 0,31 ms para 8 MB.
//
// ## Qué significa para el video (#263)
//
// Que el presupuesto de 1 upload/frame NO SE LE APLICA. Ese presupuesto acota
// cuántas TEXTURAS NUEVAS se crean por frame; un video no crea ninguna: aloca
// una vez y re-sube píxeles a la misma imagen, sin mipmaps (se dibuja 1:1) y
// sin descriptores nuevos. Su costo por frame es el memcpy más el comando de
// copia — décimas de ms, no 3,5.
//
// Y desarma la última duda sobre el pipeline YUV: con costo por objeto, tres
// planos serían TRES uploads y estaría peor; con textura persistente el costo
// vuelve a ser de bytes y YUV ayuda 2,7×. Sigue siendo optimización y no
// requisito, ahora por dos caminos independientes.
//
// EFECTO COLATERAL que no busqué: el mismo dato dice que cargar los assets de
// un proyecto está limitado por setup de textura, no por I/O ni por bytes —
// 132 assets × 3,5 ms de sobrecarga fija, con el tope de 1 por frame. Ver el
// issue que salió de acá.
//
// ## El OTRO riesgo de #263: libvpx en Windows (medido 2026-07-27)
//
// El plan lo daba por resuelto salvo por nasm. Lo que se estableció:
//
//   1. NASM NO ES PROBLEMA. vcpkg lo baja solo con
//      vcpkg_find_acquire_program(NASM) — verificado en el portfile, y el port
//      no tiene restricción de plataforma (libvpx 1.16.0).
//
//   2. LA LIBRERÍA COMPILA. Con INCLUDE/LIB/PATH del toolset 14.43.34808 + SDK
//      10.0.22621.0 y MSBuild.exe por ruta completa sobre el vpx.vcxproj que
//      genera el configure: cero errores, vpx.lib de 29 MB. Ése es el dato que
//      importa — libvpx es viable acá y no hay que buscar otro decoder.
//
//   3. `vcpkg install libvpx:x64-windows` FALLA IGUAL, y la causa NO está
//      aislada. Falla con error C1083 sobre stddef.h / string.h / stdlib.h /
//      assert.h, o sea que a CL no le llega el INCLUDE. Probado y descartado:
//        · correrlo con vcvars64.bat aplicado antes, en el mismo cmd → falla;
//        · el .vcxproj fija WindowsTargetPlatformVersion 8.1, pero vcpkg ya lo
//          sobreescribe por línea de comandos;
//        · una variable NoDefaultCurrentDirectoryInExePath=1 del entorno de
//          ejecución rompe vcvars64.bat por separado (el script hace pushd y
//          llama a vswhere.exe SIN ruta) — real, pero NO es la causa: sin ella
//          vcvars anda y vcpkg falla lo mismo.
//      Hipótesis viva: vcpkg sanea el entorno del build y arma el suyo, y ese
//      no incluye el CRT. Sin aislar.
//
// CONSECUENCIA PRÁCTICA: como el proyecto usa CMake + clang-cl y no depende de
// vcpkg para esto, la vía corta es construir libvpx una vez y consumir el .lib
// + headers (ABI MSVC, compatible con clang-cl), o un ExternalProject. Depurar
// el port no está en el camino crítico.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using clk = std::chrono::steady_clock;

/// Mediana de N copias. Mediana y no promedio: una interrupción del sistema
/// operativo en medio de una corrida inventa un pico que no es del código.
double memcpy_ms(size_t bytes, int iters) {
    std::vector<uint8_t> src(bytes, 0xAB);
    std::vector<uint8_t> dst(bytes, 0x00);
    std::vector<double> t;
    t.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        const auto a = clk::now();
        std::memcpy(dst.data(), src.data(), bytes);
        const auto b = clk::now();
        t.push_back(std::chrono::duration<double, std::milli>(b - a).count());
    }
    std::sort(t.begin(), t.end());
    // Tocar el destino para que el optimizador no borre la copia.
    volatile uint8_t sink = dst[bytes / 2];
    (void)sink;
    return t[t.size() / 2];
}

struct Case { const char* name; size_t bytes; };

}  // namespace

int main() {
    std::printf("=== video_budget_spike (#263) ===\n"
                "Presupuesto: 16,6 ms por frame a 60 fps — y ahí adentro entran\n"
                "la emulación, el resto del render y el audio.\n\n");

    const Case cases[] = {
        { "RGBA 4K    (3840x2160)", (size_t)3840 * 2160 * 4 },
        { "RGBA 1080p (1920x1080)", (size_t)1920 * 1080 * 4 },
        { "YUV420 1080p (3 planos R8)", (size_t)(1920 * 1080 * 3 / 2) },
        { "RGBA 720p  (1280x720)", (size_t)1280 * 720 * 4 },
    };

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail;
    };

    double ms_rgba4k = 0, ms_rgba1080 = 0, ms_yuv1080 = 0;
    for (const Case& c : cases) {
        const double ms = memcpy_ms(c.bytes, 40);
        const double mb = c.bytes / (1024.0 * 1024.0);
        std::printf("[..] %-28s %6.2f MB  →  %5.2f ms  (%4.1f%% del frame · %.1f GB/s)\n",
                    c.name, mb, ms, 100.0 * ms / 16.6,
                    mb / 1024.0 / (ms / 1000.0));
        if (c.bytes == (size_t)3840 * 2160 * 4)      ms_rgba4k   = ms;
        if (c.bytes == (size_t)1920 * 1080 * 4)      ms_rgba1080 = ms;
        if (c.bytes == (size_t)(1920 * 1080 * 3 / 2)) ms_yuv1080  = ms;
    }
    std::printf("\n");

    check(ms_rgba4k > 0.0 && ms_rgba1080 > 0.0 && ms_yuv1080 > 0.0,
          "control: las tres variantes se midieron");

    // El plan propone dos mitigaciones. La pregunta no es si «andan» sino si
    // cada una CAMBIA la respuesta — una mitigación que no mueve la aguja es
    // complejidad que no hay que pagar.
    std::printf("[..] topear a 1080p ahorra %.2f ms respecto de 4K (%.1f x)\n",
                ms_rgba4k - ms_rgba1080,
                ms_rgba1080 > 0 ? ms_rgba4k / ms_rgba1080 : 0.0);
    std::printf("[..] pasar a YUV420 ahorra otros %.2f ms respecto de RGBA 1080p (%.1f x)\n",
                ms_rgba1080 - ms_yuv1080,
                ms_yuv1080 > 0 ? ms_rgba1080 / ms_yuv1080 : 0.0);

    // EL VEREDICTO, y no es el que esperaba.
    //
    // El motor justifica su presupuesto de UN upload por frame diciendo que un
    // memcpy de staging de ~10 MB tarda 4-5 ms y hacia starve al audio
    // (src/ayther_renderer.cpp). Medido aca: 7,91 MB en 0,31 ms, unas
    // quince veces mas rapido. Ni siquiera un frame RGBA 4K entero llega al
    // techo del 25% del frame.
    //
    // O sea que este banco NO responde la pregunta: la ELIMINA como candidata.
    // Si los uploads cuestan lo que el motor dice, el costo no esta en la copia
    // CPU sino del otro lado: la transferencia por PCI, la sincronizacion con
    // el fence, o el driver. Eso se mide con Vulkan de verdad, no con memcpy, y
    // es la medicion que hay que hacer antes de comprometer la fase del video.
    //
    // Lo que si queda establecido, y sirve: el formato y la resolucion del
    // video NO son el problema. Topear a 1080p o pasar a YUV420 son
    // optimizaciones legitimas pero ninguna de las dos decide nada aca.
    constexpr double kBudget = 16.6 * 0.25;
    std::printf("\n[..] techo de referencia para UN upload: %.2f ms (25%% del frame)\n",
                kBudget);
    const double bw = (3840.0 * 2160 * 4 / (1024 * 1024 * 1024.0)) / (ms_rgba4k / 1000.0);
    check(bw > 1.0 && bw < 200.0,
          "control: el ancho de banda medido es plausible (no se optimizo la copia)");
    check(ms_yuv1080 < ms_rgba1080 && ms_rgba1080 < ms_rgba4k,
          "control: el costo escala con el tamano (la medicion distingue casos)");
    if (ms_rgba4k < kBudget)
        std::printf("[..] HALLAZGO: hasta RGBA 4K entra en el techo. El memcpy NO es\n"
                    "     el cuello de botella. La nota del motor que atribuye 4-5 ms\n"
                    "     a un memcpy de 10 MB no se reproduce en esta maquina; el\n"
                    "     costo real de un upload esta del lado de la GPU (PCI, fence,\n"
                    "     driver) y hay que medirlo con Vulkan, no con esto.\n");
    else
        std::printf("[..] el memcpy solo ya consume el presupuesto: el formato y la\n"
                    "     resolucion del video SI deciden y el pipeline YUV es requisito.\n");

    std::printf("\nOJO: esto mide SÓLO el memcpy de staging, que es el costo que el\n"
                "motor ya identificó como dominante. Faltan la transferencia PCI\n"
                "(depende del hardware) y el decode de libvpx (otra medición).\n");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}

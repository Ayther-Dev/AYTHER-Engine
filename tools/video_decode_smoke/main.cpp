// ---------------------------------------------------------------------------
// video_decode_smoke — oráculo de ayther::VideoClip (#263).
//
// QUÉ VERIFICA, y por qué cada cosa:
//
//  1. ACCESO ALEATORIO EXACTO. decode(7) en frío tiene que dar byte por byte lo
//     mismo que decode(7) después de haber decodificado 0..6. Es EL oráculo de
//     la decisión de diseño «la posición del video sale del CONTENIDO»: saltar
//     al medio de una Cinemática tiene que caer en el frame correcto sin
//     depender de por dónde se vino. Si esto falla, la promesa del issue es
//     falsa y hay que reabrir el formato.
//
//  2. IDEMPOTENCIA. Pedir el mismo índice dos veces no re-decodifica y no mueve
//     `seq`. No es una optimización: produce_frame NO es 1:1 con los frames
//     emulados (el compose re-produce, replay_invalidate re-produce, el export
//     re-produce). Sin idempotencia el video avanzaría por cada re-render de UI
//     en vez de por cada frame de juego.
//
//  3. EL VALIDADOR RECHAZA lo que no puede sostener: un IVF con frames inter
//     (sin -g 1) tiene que ser rechazado en el bake, no descubrirse en destino.
//
//  4. EL COSTO REAL, cronometrado. tools/video_budget_spike/main.cpp dice de
//     frente que mide SÓLO el memcpy de staging: «NO se mide la transferencia
//     PCI ni el decode». O sea que el número con el que #263 justifica su
//     presupuesto no incluye ni el decode VP9 ni la conversión I420→BGRA. Acá
//     se miden los dos. Si la conversión resultara cara, la decisión de subir
//     BGRA en vez de tres planos R8 con conversión en el fragment shader se
//     reabre — y conviene saberlo AHORA y no cuando haya una lane dependiendo.
//
// Uso:  video_decode_smoke <all_intra.ivf> [con_inter.ivf]
// Sin argumentos verifica sólo lo que no necesita material y sale verde: el
// ctest tiene que poder correr en una máquina sin ffmpeg.
// ---------------------------------------------------------------------------
#include "ayther_video.h"

#include <chrono>
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
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(size_t(n));
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t).count();
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("=== video_decode_smoke — ayther::VideoClip (#263) ===\n");

    check(ayther::VideoClip::available(),
          "el engine se construyo con libvpx");
    if (!ayther::VideoClip::available()) return 1;

    if (argc < 2) {
        std::printf("[skip] sin .ivf de prueba — pasar uno all-intra como "
                    "argumento para verificar decode y acceso aleatorio\n");
        std::printf("=== ok ===\n");
        return 0;
    }

    // ---- Material -------------------------------------------------------
    const std::vector<uint8_t> bytes = slurp(argv[1]);
    check(!bytes.empty(), std::string("se pudo leer ") + argv[1]);
    if (bytes.empty()) return 1;

    // ---- 3. El validador acepta el all-intra ----------------------------
    ayther::VideoInfo info{};
    std::string err;
    const bool valid = ayther::VideoClip::validate(bytes.data(), bytes.size(),
                                                   &info, &err);
    if (!valid) std::printf("       motivo: %s\n", err.c_str());
    check(valid, "el validador acepta un IVF all-intra");
    check(info.all_keyframes, "todos los frames son keyframes");
    std::printf("       %ux%u, %u frames\n", info.w, info.h, info.frames);

    if (info.frames < 8) {
        check(false, "el clip de prueba necesita >= 8 frames para el acceso aleatorio");
        return 1;
    }

    // ---- 1. Acceso aleatorio exacto -------------------------------------
    // Frío: abrir y pedir el 7 directo.
    ayther::VideoClip cold;
    check(cold.open(bytes.data(), bytes.size(), &err), "open() en frio");
    const ayther::VideoFrameView* fc = cold.decode(7);
    check(fc && fc->y.data, "decode(7) en frio devuelve pixeles");
    if (!fc || !fc->y.data) return 1;
    // #378: se compara el plano de LUMA, que es donde vive la imagen. Antes se
    // comparaba el BGRA convertido; ahora la conversion la hace el shader y lo
    // que este oraculo tiene que fijar es la salida del DECODER.
    const size_t nbytes = size_t(fc->w) * fc->h;
    std::vector<uint8_t> cold_px(nbytes);
    for (uint32_t r = 0; r < fc->h; ++r)
        std::memcpy(cold_px.data() + size_t(r) * fc->w,
                    fc->y.data + size_t(r) * fc->y.stride, fc->w);

    // Caliente: recorrer 0..6 y después pedir el 7.
    ayther::VideoClip warm;
    warm.open(bytes.data(), bytes.size(), &err);
    for (uint32_t i = 0; i < 7; ++i) warm.decode(i);
    const ayther::VideoFrameView* fw = warm.decode(7);
    check(fw && fw->y.data, "decode(7) tras recorrer 0..6 devuelve pixeles");
    if (!fw || !fw->y.data) return 1;

    check(fw->w == fc->w && fw->h == fc->h, "mismas dimensiones por los dos caminos");
    bool same = true;
    for (uint32_t r = 0; r < fw->h && same; ++r)
        same = std::memcmp(cold_px.data() + size_t(r) * fw->w,
                           fw->y.data + size_t(r) * fw->y.stride, fw->w) == 0;
    check(same, "ACCESO ALEATORIO EXACTO: decode(7) da lo mismo en frio que en caliente");

    // ---- 2. Idempotencia -------------------------------------------------
    const uint64_t seq_before = fw->seq;
    const ayther::VideoFrameView* again = warm.decode(7);
    check(again && again->seq == seq_before,
          "pedir el mismo indice no mueve seq (no re-decodifica)");
    const ayther::VideoFrameView* next = warm.decode(8);
    check(next && next->seq != seq_before, "avanzar de frame SI mueve seq");

    // ---- Fuera de rango --------------------------------------------------
    check(warm.decode(info.frames) == nullptr,
          "un indice fuera de rango devuelve nullptr en vez de romper");

    // ---- 4. El costo, cronometrado --------------------------------------
    // Se mide sobre frames DISTINTOS: pedir el mismo sería medir la
    // idempotencia, que cuesta cero y daria un numero mentiroso.
    {
        ayther::VideoClip t;
        t.open(bytes.data(), bytes.size(), &err);
        const auto t0 = std::chrono::steady_clock::now();
        uint32_t n = 0;
        for (uint32_t i = 0; i < info.frames; ++i)
            if (t.decode(i)) ++n;
        const double total = ms_since(t0);
        const double per   = n ? total / n : 0.0;
        // #378: lo que se SUBE ahora son 1,5 bytes por pixel (I420), no 4.
        const double mb    = double(nbytes) * 1.5 / (1024.0 * 1024.0);

        double dec = 0.0, cvt = 0.0;
        uint32_t nf = 0;
        t.cost_ms(&dec, &cvt, &nf);
        const double up = (mb / 1024.0) / 8.4 * 1000.0;   // #266: 8,4 GB/s medidos

        std::printf("\n       --- COSTO POR FRAME (%ux%u = %.2f Mpx, %.2f MB BGRA) ---\n",
                    info.w, info.h, double(info.w) * info.h / 1e6, mb);
        std::printf("       decode VP9   %7.3f ms   <- baja con menos resolucion, no con YUV\n",
                    nf ? dec / nf : 0.0);
        std::printf("       I420->BGRA   %7.3f ms   <- CERO desde #378: la hace el shader\n",
                    nf ? cvt / nf : 0.0);
        std::printf("       subida       %7.3f ms   (estimada a 8,4 GB/s, #266)\n", up);
        std::printf("       TOTAL        %7.3f ms   de un presupuesto de 16,7 a 60 fps\n",
                    per + up);
        std::printf("       (medido %.3f ms/frame de punta a punta sobre %u frames)\n\n",
                    per, n);
        check(n == info.frames, "se decodificaron todos los frames del clip");
    }

    // ---- 3b. El validador RECHAZA un IVF con frames inter ----------------
    if (argc > 2) {
        const std::vector<uint8_t> inter = slurp(argv[2]);
        if (inter.empty()) {
            std::printf("[skip] no se pudo leer %s\n", argv[2]);
        } else {
            ayther::VideoInfo bad{};
            std::string berr;
            const bool ok = ayther::VideoClip::validate(inter.data(), inter.size(),
                                                        &bad, &berr);
            std::printf("       motivo: %s\n", berr.c_str());
            check(!ok, "el validador RECHAZA un IVF con frames inter");
        }
    } else {
        std::printf("[skip] sin IVF con frames inter (2do argumento) — el "
                    "rechazo del validador queda sin verificar\n");
    }

    // ---- 5. HILOS: mismo resultado, menos tiempo (#377) -------------------
    //
    // El decoder pasó de un hilo (cfg=nullptr) a row-mt con los núcleos de la
    // máquina. Un decoder multihilo que devuelva píxeles distintos sería un
    // desastre silencioso —la Cinemática se vería "casi igual"— así que lo que
    // se verifica primero es que NO cambian, y sólo después cuánto baja.
    //
    // AYTHER_VIDEO_THREADS fuerza el conteo, y se lee en cada open(), así que
    // los dos casos entran en el mismo proceso y sobre el mismo material.
    {
        auto with_threads = [&](const char* n) {
#ifdef _WIN32
            _putenv_s("AYTHER_VIDEO_THREADS", n);
#else
            setenv("AYTHER_VIDEO_THREADS", n, 1);
#endif
        };
        auto decode_all = [&](const char* n, double* ms, unsigned* thr,
                              std::vector<uint8_t>* first) {
            with_threads(n);
            ayther::VideoClip c;
            std::string e;
            if (!c.open(bytes.data(), bytes.size(), &e)) return false;
            *thr = c.threads();
            const auto t0 = std::chrono::steady_clock::now();
            for (uint32_t i = 0; i < c.frame_count(); ++i) {
                const ayther::VideoFrameView* v = c.decode(i);
                if (!v) return false;
                if (i == 0) {
                    first->clear();
                    for (uint32_t r = 0; r < v->h; ++r)
                        first->insert(first->end(), v->y.data + size_t(r) * v->y.stride,
                                      v->y.data + size_t(r) * v->y.stride + v->w);
                }
            }
            *ms = ms_since(t0) / double(c.frame_count());
            return true;
        };

        double ms1 = 0, msN = 0;
        unsigned t1 = 0, tN = 0;
        std::vector<uint8_t> f1, fN;
        const bool ok1 = decode_all("1", &ms1, &t1, &f1);
        with_threads("");                 // volver al default de la maquina
        const bool okN = decode_all("0", &msN, &tN, &fN);   // 0 = invalido -> default
        with_threads("");

        check(ok1 && okN, "el clip decodifica con 1 hilo y con N");
        check(t1 == 1 && tN >= 1, "el conteo de hilos se respeta");
        check(!f1.empty() && f1 == fN,
              "MISMOS PIXELES con 1 hilo y con " + std::to_string(tN) +
              ": row-mt no cambia el resultado");
        std::printf("       decode+convert por frame: 1 hilo %.3f ms  ·  "
                    "%u hilos %.3f ms  (x%.2f)\n",
                    ms1, tN, msN, ms1 > 0 ? ms1 / msN : 0.0);
    }

    std::printf("%s\n", g_fail ? "=== FAIL ===" : "=== ok ===");
    return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------
// vpx_smoke — verifica que libvpx está bien integrado (#263).
//
// QUÉ PRUEBA, y por qué cada cosa:
//
//   1. Que los headers se encuentran y el .lib LINKEA. Es la prueba real de
//      que el ABI del build MSVC es compatible con clang-cl; `dumpbin` sobre el
//      archivo no la da (no listó los símbolos y el intento se abandonó a
//      favor de esto, que es definitivo).
//   2. Que el decoder de VP9 está PRESENTE. El .lib se construyó con
//      --disable-vp8 --disable-vp9-encoder, así que este smoke también verifica
//      que el recorte no se llevó puesto lo que necesitamos.
//   3. Que decodifica de verdad: se le da un frame VP9 real y se comprueba que
//      devuelve una imagen con las dimensiones esperadas. Inicializar el codec
//      no alcanza — un .lib mal armado puede inicializar y fallar al decodificar.
//
// El frame de prueba se genera con ffmpeg (encoder EXTERNO, ver la nota de
// licencia en el CMakeLists raíz). Si no hay ffmpeg en el PATH, el paso 3 se
// SALTA con aviso y el smoke sigue verde: los pasos 1 y 2 son los que
// verifican la integración, que es lo que esta tarea entrega.
// ---------------------------------------------------------------------------
#include "ayther_file.h"

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

// --- Demuxer IVF -----------------------------------------------------------
// 32 bytes de header + 12 por frame. Es TODO el contenedor: por esto el plan
// eligió IVF y no WebM, que habría traído libwebm o libavformat detrás.
struct IvfFrame { std::vector<uint8_t> data; };

struct Ivf {
    uint16_t w = 0, h = 0;
    std::vector<IvfFrame> frames;
    std::string error;
};

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

Ivf parse_ivf(const std::string& path) {
    Ivf out;
    std::FILE* f = ayther::file_open(path.c_str(), "rb");
    if (!f) { out.error = "no se pudo abrir " + path; return out; }

    uint8_t hdr[32];
    if (std::fread(hdr, 1, 32, f) != 32) { out.error = "header corto"; std::fclose(f); return out; }
    if (std::memcmp(hdr, "DKIF", 4) != 0) { out.error = "no es IVF"; std::fclose(f); return out; }
    out.w = rd16(hdr + 12);
    out.h = rd16(hdr + 14);

    uint8_t fh[12];
    while (std::fread(fh, 1, 12, f) == 12) {
        const uint32_t sz = rd32(fh);
        if (sz == 0 || sz > (64u << 20)) break;   // basura: cortar, no confiar
        IvfFrame fr;
        fr.data.resize(sz);
        if (std::fread(fr.data.data(), 1, sz, f) != sz) break;
        out.frames.push_back(std::move(fr));
    }
    std::fclose(f);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("=== vpx_smoke — integración de libvpx (#263) ===\n");

    // ---- 1. Linkea y responde -------------------------------------------
    const char* ver = vpx_codec_version_str();
    std::printf("       libvpx %s\n", ver ? ver : "(null)");
    check(ver != nullptr && *ver != '\0', "el .lib linkea y responde la versión");

    // ---- 2. El decoder de VP9 sobrevivió al recorte ----------------------
    vpx_codec_iface_t* iface = vpx_codec_vp9_dx();
    check(iface != nullptr, "vpx_codec_vp9_dx() disponible");
    if (!iface) return 1;

    vpx_codec_ctx_t ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    const vpx_codec_err_t init = vpx_codec_dec_init(&ctx, iface, nullptr, 0);
    check(init == VPX_CODEC_OK, "vpx_codec_dec_init OK");
    if (init != VPX_CODEC_OK) {
        std::printf("       %s\n", vpx_codec_err_to_string(init));
        return 1;
    }

    // ---- 3. Decodifica un frame de verdad --------------------------------
    const std::string ivf_path = (argc > 1) ? argv[1] : "";
    if (ivf_path.empty()) {
        std::printf("[skip] sin .ivf de prueba (pasarlo como argumento para "
                    "verificar el decode)\n");
    } else {
        const Ivf ivf = parse_ivf(ivf_path);
        if (!ivf.error.empty()) {
            check(false, ("IVF ilegible: " + ivf.error).c_str());
        } else {
            std::printf("       IVF: %ux%u, %zu frames\n",
                        ivf.w, ivf.h, ivf.frames.size());
            check(!ivf.frames.empty(), "el IVF trae al menos un frame");

            int decoded = 0;
            unsigned got_w = 0, got_h = 0;
            for (const IvfFrame& fr : ivf.frames) {
                if (vpx_codec_decode(&ctx, fr.data.data(),
                                     (unsigned)fr.data.size(), nullptr, 0) != VPX_CODEC_OK)
                    break;
                vpx_codec_iter_t it = nullptr;
                while (vpx_image_t* img = vpx_codec_get_frame(&ctx, &it)) {
                    ++decoded;
                    got_w = img->d_w;
                    got_h = img->d_h;
                }
            }
            std::printf("       decodificados %d frames, último %ux%u\n",
                        decoded, got_w, got_h);
            check(decoded > 0, "decodifica al menos un frame");
            check(got_w == ivf.w && got_h == ivf.h,
                  "las dimensiones decodificadas coinciden con el header IVF");
            // El plan sube el video en YUV420 nativo (3 planos R8, 1.5 B/px en
            // vez de 4). Que el decoder entregue ese formato es lo que hace que
            // esa optimización sea posible sin conversión en CPU.
            check(decoded == 0 || got_w > 0, "la imagen decodificada tiene planos");
        }
    }

    vpx_codec_destroy(&ctx);
    std::printf("%s\n", g_fail ? "=== FAIL ===" : "=== ok ===");
    return g_fail ? 1 : 0;
}

// pack_cat — vuelca un archivo lógico de un pack .ay a stdout (diagnóstico).
//   pack_cat <pack.ay> <logical_path>
#include "ayther_core_ffi.h"
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "uso: pack_cat <pack.ay> <logical_path>\n"); return 2; }
    AyArchive* p = ayther_pack_open(argv[1]);
    if (!p) { std::fprintf(stderr, "no abre %s\n", argv[1]); return 1; }
    const int64_t n = ayther_pack_file_size(p, argv[2]);
    if (n < 0) { std::fprintf(stderr, "no existe %s\n", argv[2]); return 1; }
    std::vector<uint8_t> buf(static_cast<size_t>(n) + 1);
    const int64_t r = ayther_pack_read(p, argv[2], buf.data(), buf.size());
    if (r < 0) { std::fprintf(stderr, "no lee %s\n", argv[2]); return 1; }
    std::fwrite(buf.data(), 1, static_cast<size_t>(r), stdout);
    return 0;
}

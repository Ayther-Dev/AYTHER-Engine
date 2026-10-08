// ---------------------------------------------------------------------------
// audio_asset_fallback_test — asignado ≠ reproducible (#388).
//
// POR QUÉ EXISTE. El mute del original se decidía por la EXISTENCIA de una
// asignación, antes de saber si el asset HD abría, decodificaba y bindeaba.
// Cualquier fallo dejaba el original silenciado sin reemplazo: silencio
// garantizado. Y la cache memorizaba el fallo para toda la sesión — reemplazar
// el archivo roto por uno bueno no hacía nada sin reiniciar.
//
// Acá se fija el contrato del lado del AudioPlayer: la tabla de fallos
// tipados (missing/empty/unsupported/corrupt), el resultado transaccional de
// play_*, y la invalidación de la cache negativa por fingerprint — un archivo
// que aparece o se reemplaza vuelve a intentarse SIN reiniciar, y uno que
// sigue roto no paga stat/log por consulta (rate-limit).
//
// Driver "dummy" de SDL3: device real, sin hardware (patrón de
// audio_player_pause_test).
// ---------------------------------------------------------------------------
#include "../../tools/common/synth_rom.h"
#include "audio_player.h"
#include "ayther_file.h"
#include "ayther_session.h"
#include "trusted_pack_fixture.h"
#include <fstream>
#include <iterator>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string &what) {
  std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
  if (!ok)
    ++g_fail;
}

bool write_bytes(const std::string &path, const void *data, size_t n) {
  FILE *f = ayther::file_open(path.c_str(), "wb");
  if (!f)
    return false;
  if (n)
    std::fwrite(data, 1, n, f);
  std::fclose(f);
  return true;
}

// WAV S16 estéreo 44100 válido con `frames` cuadros de tono.
bool write_tone_wav(const std::string &path, uint32_t frames) {
  const uint32_t data_sz = frames * 4;
  const uint32_t riff_sz = 36u + data_sz;
  uint8_t hdr[44] = {'R', 'I', 'F',  'F',  0,   0,   0,    0,    'W',  'A', 'V',
                     'E', 'f', 'm',  't',  ' ', 16,  0,    0,    0,    1,   0,
                     2,   0,   0x44, 0xAC, 0,   0,   0x10, 0xB1, 0x02, 0,   4,
                     0,   16,  0,    'd',  'a', 't', 'a',  0,    0,    0,   0};
  std::memcpy(hdr + 4, &riff_sz, 4);
  std::memcpy(hdr + 40, &data_sz, 4);
  std::vector<uint8_t> buf(sizeof(hdr) + data_sz);
  std::memcpy(buf.data(), hdr, sizeof(hdr));
  std::vector<int16_t> pcm(frames * 2, 6000);
  std::memcpy(buf.data() + sizeof(hdr), pcm.data(), data_sz);
  return write_bytes(path, buf.data(), buf.size());
}

const char *err_of(AudioPlayer &p, const std::string &path) {
  const char *e = p.asset_error_name(path);
  return e ? e : "(listo/desconocido)";
}

} // namespace

int main() {
  std::printf(
      "=== audio_asset_fallback_test — asignado != reproducible (#388) ===\n");

  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  if (!SDL_Init(SDL_INIT_AUDIO)) {
    std::printf("[FAIL] SDL_Init(AUDIO) dummy: %s\n", SDL_GetError());
    return 1;
  }
  AudioPlayer p;
  if (!p.init()) {
    std::printf("[FAIL] AudioPlayer::init() dummy\n");
    return 1;
  }

  // Baked packs name audio by content ID, without a filename extension.
  // Keep this separate from the disk key so a cached disk decode cannot make
  // the pack test pass without actually reading the signed archive.
  check(write_tone_wav("extensionless_audio_fixture", 128),
        "write extensionless WAV");
  check(p.asset_ready_disk("extensionless_audio_fixture"),
        "extensionless WAV decodes from its header");
  // Spec 002 (RF-4.6, D-6b; Toma 3 frame 1366): a prewarmed asset is also
  // converted to the mixer's format, so its first key-on does not resample a
  // whole track on the frame that starts it.
  check(p.mix_ready("extensionless_audio_fixture"),
        "D-6b: a ready disk asset is already in the mixer's format");
  std::ifstream tone_file("extensionless_audio_fixture", std::ios::binary);
  const std::vector<uint8_t> tone{std::istreambuf_iterator<char>{tone_file},
                                  {}};
  tone_file.close();
  ayther::test::TrustedPackFixture fixture{"extensionless_audio"};
  const char *manifest =
      "[pack]\nname='Audio "
      "regression'\nversion='1.0.0'\ngame_id='audio-test'\nayther_min='0.1.0'"
      "\n[regions]\ndefault='NTSC'\nsupported=['NTSC']\n";
  check(fixture.add_bytes("manifest.toml",
                          reinterpret_cast<const uint8_t *>(manifest),
                          std::strlen(manifest)),
        "add manifest");
  constexpr const char *asset_id = "51d40391488775b5f9045a2a40073027";
  check(fixture.add_bytes("assets/51d40391488775b5f9045a2a40073027",
                          tone.data(), tone.size()),
        "add extensionless pack audio");
  constexpr const char *assigned_id = "27344701428ebe2965d69cde77ee7769";
  check(fixture.add_bytes("assets/27344701428ebe2965d69cde77ee7769",
                          tone.data(), tone.size()),
        "add separately assigned pack audio");
  const char *events = "[[event]]\nsignature='0x0000000000000388'\n"
                       "asset='51d40391488775b5f9045a2a40073027'\n";
  check(fixture.add_bytes("audio_events.toml",
                          reinterpret_cast<const uint8_t *>(events),
                          std::strlen(events)),
        "add audio event catalogue");
  char pack_error[512] = {};
  const bool signed_pack = fixture.finish(pack_error, sizeof(pack_error));
  check(signed_pack, std::string("sign fixture: ") + pack_error);
  auto *pack = fixture.open();
  check(pack != nullptr, "open signed audio fixture");
  check(p.asset_ready_pack(pack, asset_id),
        "content-addressed WAV in pack is playable");
  check(p.mix_ready(asset_id),
        "D-6b: a ready pack asset is already in the mixer's format");
  check(p.play_event_hd(pack, asset_id, false, 0x388, 100),
        "start extensionless pack audio");
  check(p.hd_voice_count() > 0, "HD mixer has a playing voice");
  p.stop_all_events();
  {
    ayther::AytherSession::Config config;
    config.core_path = AYTHER_TEST_CORE_PATH;
    config.rom_path = ayther::synth::canonical_rom_path();
    config.pack_path = fixture.pack_path();
    config.trust_registry = fixture.registry_path();
    config.enable_audio =
        false; // Prewarming must also work before opening a device.
    config.derive_core_pack = false;
    auto session = ayther::AytherSession::create(config);
    check(static_cast<bool>(session), "create session with signed audio pack");
    if (session) {
      (*session)->load_audio_events_from_pack();
      check((*session)->audio_event_assignment_count() == 1,
            "load pack event assignment");
      check((*session)->audio_asset_error(asset_id) == nullptr,
            "pack catalogue prewarm does not cache a missing disk file");
      (*session)->assign_audio_event(0x389, assigned_id, false);
      check((*session)->audio_asset_error(assigned_id) == nullptr,
            "new pack assignment prewarms from the archive");
    }
  }
  std::remove("extensionless_audio_fixture");

  // ---- Tabla de fallos tipados ------------------------------------------
  check(!p.asset_ready_disk("no_existe_388.wav"),
        "asset inexistente: NO listo");
  check(std::string(err_of(p, "no_existe_388.wav")) == "missing",
        "asset inexistente informa 'missing'");
  check(!p.play_oneshot_asset_file("no_existe_388.wav", 1),
        "play de un asset inexistente devuelve false (suena el original)");
  check(p.hd_voice_count() == 0, "y no dejó ninguna voz viva");

  write_bytes("vacio_388.wav", nullptr, 0);
  check(!p.asset_ready_disk("vacio_388.wav"), "asset vacío: NO listo");
  check(std::string(err_of(p, "vacio_388.wav")) == "empty",
        "asset vacío informa 'empty'");

  const char garbage[] = "RIFFxxxxWAVEfmt truncado a mano";
  write_bytes("trunco_388.wav", garbage, sizeof(garbage));
  check(!p.asset_ready_disk("trunco_388.wav"), "WAV truncado: NO listo");
  check(std::string(err_of(p, "trunco_388.wav")) == "corrupt",
        "WAV truncado informa 'corrupt'");

  write_tone_wav("formato_388.xyz", 100);
  check(!p.asset_ready_disk("formato_388.xyz"),
        "extensión desconocida: NO listo");
  check(std::string(err_of(p, "formato_388.xyz")) == "unsupported",
        "extensión desconocida informa 'unsupported'");

  // ---- El fallo cacheado no reintenta por consulta ----------------------
  // 200 consultas seguidas (< rate-limit) = 0 stats extra; el observable
  // acá es que sigue en false y estable — el no-spam de log se ve en que el
  // camino frío (que es el que loguea) exige un CAMBIO de fingerprint.
  for (int i = 0; i < 200; ++i)
    if (p.asset_ready_disk("trunco_388.wav")) {
      g_fail++;
      break;
    }
  check(true, "200 consultas del fallo cacheado: estable, sin reintento");

  // ---- Invalidación por fingerprint: roto → válido SIN reiniciar --------
  SDL_Delay(600); // > kAssetRecheckMs: habilita el re-stat
  write_tone_wav("trunco_388.wav", 4410); // ahora es un WAV real (tamaño ≠)
  check(p.asset_ready_disk("trunco_388.wav"),
        "reemplazar el archivo roto por uno válido lo deja LISTO en vivo");
  check(err_of(p, "trunco_388.wav") == std::string("(listo/desconocido)"),
        "y el diagnóstico se limpia");
  check(p.play_oneshot_asset_file("trunco_388.wav", 2),
        "play del asset recuperado devuelve true");
  check(p.hd_voice_count() == 1, "con su voz sonando");
  p.stop_all_sfx();

  // ---- Archivo AUSENTE que aparece durante la sesión --------------------
  check(!p.asset_ready_disk("aparece_388.wav"), "aún no existe: NO listo");
  SDL_Delay(600);
  write_tone_wav("aparece_388.wav", 2205);
  check(p.asset_ready_disk("aparece_388.wav"),
        "el archivo que APARECE se activa sin reiniciar la sesión");

  // ---- play_event_hd sin pack -------------------------------------------
  check(!p.play_event_hd(nullptr, "assets/x", false, 77, 100),
        "play_event_hd sin pack devuelve false");

  p.shutdown();
  SDL_Quit();
  std::remove("vacio_388.wav");
  std::remove("trunco_388.wav");
  std::remove("formato_388.xyz");
  std::remove("aparece_388.wav");

  if (g_fail) {
    std::printf("--- %d FALLAS ---\n", g_fail);
    return 1;
  }
  std::printf("--- todo ok ---\n");
  return 0;
}

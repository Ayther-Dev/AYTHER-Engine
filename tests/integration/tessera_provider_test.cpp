#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ayther/ayther_session.h>
#include <doctest/doctest.h>

TEST_CASE(
    "Native provider selection is explicit and never opens a legacy core") {
  ayther::AytherSession::Config config;
  CHECK(config.provider == ayther::EmulationProvider::libretro);
  config.provider = ayther::EmulationProvider::tessera_native;
  config.core_path = "this-core-must-never-be-opened.dll";
  config.enable_audio = false;
  config.derive_core_pack = false;
  ayther::engine::NativeResult result;
  auto session = ayther::AytherSession::create(config, result);
  CHECK_FALSE(static_cast<bool>(session));
  CHECK(result.code < 0);
  CHECK(session.error.message.find("Tessera") != std::string::npos);
  config.provider = static_cast<ayther::EmulationProvider>(99);
  session = ayther::AytherSession::create(config, result);
  CHECK_FALSE(static_cast<bool>(session));
  CHECK(result.code == -1);
}

TEST_CASE("Native presentation carries a separate format and explicit original "
          "fallback") {
  ayther::FrameView view;
  CHECK(view.pixel_encoding == ayther::FramePixelEncoding::libretro);
  CHECK(view.native_result.code == 0);
  static_assert(std::is_same_v<decltype(std::declval<ayther::AytherSession &>()
                                            .native_emulation()),
                               ayther::engine::NativeEmulation *>);
}

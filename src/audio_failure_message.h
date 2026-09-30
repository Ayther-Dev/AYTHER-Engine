#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace ayther {

// Keep player-facing explanations separate from technical asset IDs and paths.
struct AudioFailureCounts {
  // missing, empty, unsupported, corrupt, unknown
  std::array<size_t, 5> values{};

  void note(const char *reason) {
    const std::string_view code = reason ? reason : "";
    const size_t index = code == "missing"       ? 0
                         : code == "empty"       ? 1
                         : code == "unsupported" ? 2
                         : code == "corrupt"     ? 3
                                                 : 4;
    ++values[index];
  }
};

inline std::string audio_failure_message(const std::string &title,
                                         const std::string &pack_name,
                                         const AudioFailureCounts &failures) {
  size_t total = 0;
  for (const auto count : failures.values)
    total += count;
  if (!total)
    return {};
  std::string message =
      title + ".\nNo se pudieron reproducir " + std::to_string(total) +
      (total == 1 ? " archivo de audio" : " archivos de audio");
  if (!pack_name.empty())
    message += " del pack «" + pack_name + "»";
  message += ".\nCausa: ";
  constexpr std::array<const char *, 5> descriptions{
      "no se encontraron o no se pudieron abrir", "están vacíos",
      "tienen un formato no compatible (se admiten WAV, OGG y FLAC)",
      "no se pudieron leer o decodificar; pueden estar dañados",
      "fallaron al reproducirse; no se pudo determinar la causa"};
  constexpr std::array<const char *, 5> singular{
      "no se encontró o no se pudo abrir", "está vacío",
      "tiene un formato no compatible (se admiten WAV, OGG y FLAC)",
      "no se pudo leer o decodificar; puede estar dañado",
      "falló al reproducirse; no se pudo determinar la causa"};
  bool first = true;
  for (size_t index = 0; index < failures.values.size(); ++index) {
    if (!failures.values[index])
      continue;
    if (!first)
      message += "; ";
    message +=
        std::to_string(failures.values[index]) +
        (failures.values[index] == 1 ? " archivo " : " archivos ") +
        (failures.values[index] == 1 ? singular[index] : descriptions[index]);
    first = false;
  }
  message += ".\nInstalá una versión corregida del pack. Si lo creaste en Lab, "
             "revisá los archivos de audio y volvé a exportarlo. "
             "Para continuar, podés desactivar el modo HD. "
             "Si tampoco escuchás el juego sin HD, revisá el volumen y la "
             "salida de audio.";
  return message;
}

} // namespace ayther

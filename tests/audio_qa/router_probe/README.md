# Observación del resampler del router — QA-020d3

RF-6, RF-10 y RF-14. Ensayo aislado del StreamResampler y de la frontera de consumo/ceros de Engine rc.9. La configuración comprueba hashes de los dos archivos de implementación y del header, extrae literalmente sus secciones y genera variantes de referencia y observada dentro del build. No se modifican las fuentes originales ni se reescribe el filtro en el ensayo.

La sección de sesión conserva sus decisiones de reset por atraso, inicialización a cero, pull y contador de déficit. El harness aporta únicamente los miembros y entradas que usa esa sección; no simula el resto de AytherSession. La variante observada registra fase anterior/posterior, paso efectivo, tamaño de historia, índices realmente leídos, valores producidos, recortes y resets. Se conservan los rangos de ceros del bloque solicitado que el resampler no produjo. La suma posterior de SoundFont y el staging quedan fuera de esta comprobación y corresponden a QA-020d4.

Se utilizan entradas sintéticas a 53267 Hz, salida inicial a 44100 Hz y cambio a 48000 Hz conservando fase. Siete solicitudes producen 328 frames de salida: 204 calculados por el filtro y 124 ceros del límite de consumo. Un exceso real de cola provoca el reset de la rama original, descartando [142, 416) del dominio absoluto de entrada, incluida historia retenida. La siguiente entrada comienza en 416; no se atribuye continuidad al material descartado.

Desde Engine, con Visual Studio 2026 v145:

```powershell
cmake -S tests/audio_qa/router_probe -B build/qa-020-router `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=C:/Users/david/Workspaces/ayther/.qa-reference/audio-qa-initial-20260927/engine-source'
cmake --build build/qa-020-router --config Release --parallel 4
ctest --test-dir build/qa-020-router -C Release --output-on-failure
```

Los tres casos exigen igualdad byte a byte con la referencia. `lost` omite el registro del reset con cola; `phase` altera un valor de fase copiado en el registro. Ninguno altera el PCM. Ambos deben devolver 2, `router_trace_incomplete`, conservando diagnóstico y archivos. Los campos calculados a partir de un registro incompleto no se presentan como posiciones verificadas.

Límites del ensayo: 512 frames de entrada, 328 frames de salida, siete bloques, hasta 1024 registros y tres segundos por proceso. Los callbacks solo copian structs en un array fijo; el agotamiento invalida la traza. La fase se conserva como double con precisión de ida y vuelta en TOML, igual que en la implementación original. No es todavía contrato público, captura SDL, replay de tomas ni cumplimiento del presupuesto de rendimiento del producto.

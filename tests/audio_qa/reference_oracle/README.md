# Adaptador de decisión de referencia — QA-022

RF-5 y RF-15. Invoca directamente live_resume_decide del header fijado de Engine rc.9. No copia su algoritmo ni introduce un motivo deducido. El caso viene de los archivos state.json e inputs.json definidos en QA-021; CTest verifica sus hashes y pasa argumentos separados, sin concatenación de shell. El límite uint64 se conserva como texto decimal JSON para evitar redondearlo a través de un número de coma flotante.

La instancia de bucle tiene inicio 30, fin 180 y cut UINT64_MAX; se solicita reanudar en cuadro 120 a 60 FPS con duración conocida 3 s. La rama original devuelve restart y offset 1,5 s. Es una decisión lógica; no acredita todavía recreación efectiva del reproductor ni el motivo interno. Esos campos se conservan como no observados para QA-023.

Desde Engine:

```powershell
cmake -S tests/audio_qa/reference_oracle -B build/qa-022-reference `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=C:/Users/david/Workspaces/ayther/.qa-reference/audio-qa-initial-20260927/engine-source' `
  '-DQA_ORACLE_FIXTURE=C:/Users/david/Workspaces/ayther/specs/001-AYTHER-bug-audio-qa/evidence/qa-021-oracle-definition'
cmake --build build/qa-022-reference --config Release --parallel 4
ctest --test-dir build/qa-022-reference -C Release --output-on-failure
```

Dos tests: decisión del fixture y rechazo de una entrada numérica con sufijo antes de crear resultado. Límites: una instancia, una operación, siete valores escalares y tres segundos por invocación. El resultado se escribe en un directorio nuevo; no reemplaza un archivo existente. No requiere SDL, core, ROM, red ni replay. La matriz completa y la congelación del oráculo siguen pendientes.

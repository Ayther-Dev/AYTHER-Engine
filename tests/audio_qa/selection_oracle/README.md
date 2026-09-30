# Oráculo de selección — QA-024

RF-2, RF-5 y RF-15. Compila el índice AudioMatchIndex y el método resolve_event_sig extraído literalmente de la sesión fijada rc.9. El harness aporta el catálogo y el índice que consulta ese método. La versión observada copia candidaturas y resultados dentro de las ramas originales; la referencia no contiene hooks. Ambas reciben el mismo estado y evento sintético.

El catálogo contiene una asignación exacta 99 y tres reglas de instrumento/tono: 10/63, 20/64 y 15/64, todas del instrumento 7. match consulta tono 64 y selecciona 15 por el desempate real; miss consulta 62 y conserva tres descartes y ausencia de coincidencia; exact consulta firma 99 y no visita candidaturas amplias. lost elimina la candidatura 20 solo del registro, devuelve 2 y conserva la misma decisión que match.

Se escriben estado, entrada, decisión y hechos separados. El sentinel de salida no modificado se conserva como cadena decimal, sin equipararlo a una asignación seleccionada. El evento procede del fixture, no de un detector o ROM ejecutados. La asignación exacta se identifica como selección directa, no se inventa un recorrido del índice. No se inicia audio en este ensayo.

Desde Engine:

```powershell
cmake -S tests/audio_qa/selection_oracle -B build/qa-024-selection `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=C:/Users/david/Workspaces/ayther/.qa-reference/audio-qa-initial-20260927/engine-source'
cmake --build build/qa-024-selection --config Release --parallel 4
ctest --test-dir build/qa-024-selection -C Release --output-on-failure
```

Límites: un evento por proceso, tres reglas amplias y una exacta, 32 hechos y tres segundos por proceso. Los hooks solo copian structs; desbordamiento o pérdida invalida la traza. La escritura ocurre después de la decisión, en un directorio nuevo. Los hashes de las fuentes originales se exigen al configurar. No se modifica producción ni se reescribe el algoritmo de selección.

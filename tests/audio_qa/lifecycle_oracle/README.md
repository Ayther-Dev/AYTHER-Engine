# Oráculos del ciclo de vida — QA-026

RF-6, RF-8 y RF-15. El mixer de rc.9 se compila intacto como referencia y en una copia con hooks para cursor, cruce de bucle y eliminación por ventana. No se recalculan sus decisiones en el adaptador. Una entrada PCM sintética de ocho frames tiene región [2,6), offset 4 y ganancia 0,5; se suma al original constante 100.

loop conserva seis cruces 6→2 en muestras 2,6,10,14,18,22. window mantiene la voz en el cuadro 2 y la elimina en el 3 por end_frame=2, en el límite 20 de mezcla. Su posición final es 4; no hay aportes posteriores. lost omite solo el primer cruce, mantiene audio idéntico y devuelve 2. Los extremos de cursor del terminal indican posición final retenida, no una nueva reproducción activa.

Desde Engine:

```powershell
cmake -S tests/audio_qa/lifecycle_oracle -B build/qa-026-lifecycle `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=C:/Users/david/Workspaces/ayther/.qa-reference/audio-qa-initial-20260927/engine-source'
cmake --build build/qa-026-lifecycle --config Release --parallel 4
ctest --test-dir build/qa-026-lifecycle -C Release --output-on-failure
```

Límites: cuatro cuadros, hasta 72 frames de mezcla, dos ocurrencias, 32 eventos, 256 aportes y tres segundos por proceso. Hooks sin reserva ni disco; saturación/pérdida invalida el registro. El test compara estado y PCM con la referencia. Los resultados pertenecen al timeline principal del mixer, no a una captura SDL ni a una toma real.

QA-027 (RF-7, RF-8, RF-15) añade replacement y replacement_lost. Dos arranques con la misma clave se solapan durante 56 frames; el segundo inicia el fade de la voz anterior. Se conservan identidades, cursor, ganancia efectiva por aporte y finales naturales en 64 y 72, sin juzgar el síntoma. El orden de visita por voces se conserva aunque sus límites de muestra no sean crecientes. El control negativo omite un terminal y devuelve 2 sin cambiar audio ni decisiones.

QA-028 (RF-7, RF-10, RF-15) añade natural y natural_lost: original constante 100, asset de ocho frames, ganancia 0,5, inicio 4 y mezcla de 16 frames. El intervalo [4,12) contiene original+HD; los restantes contienen solo original. El terminal natural conserva posición 8 y frontera 12. La ausencia deliberada de ese terminal invalida la traza con PCM idéntico.

QA-029 (RF-5, RF-8, RF-15) añade simultaneous y simultaneous_lost. Dos voces comparten end_frame=1 y cut_frame=1; solo la primera tiene fade autorado. El tick de cuadro 2 visita las voces 2→1: corta la segunda e inicia fade en la primera, conservando antes el contexto de todos los límites concurrentes. El fade termina cuatro muestras después. El orden efectivo no se reconstruye por proximidad de timestamps. Hay un buffer adicional de 32 contextos de tick; el control negativo pierde un corte y falla sin modificar PCM.

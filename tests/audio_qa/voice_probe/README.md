# Observación por ocurrencia del mixer — QA-020d2

Ensayo aislado de RF-6, RF-7 y RF-10 sobre HdMixer de Engine rc.9. No implementa aún la API QA ni demuestra la relación completa con cuadros de Runtime o salida SDL.

La configuración exige el SHA-256 del header de referencia. Genera una copia privada en el build con una identidad observacional por voz y un callback después de cada aporte real. El contador agregado started y las decisiones de mezcla siguen intactos. Se guardan cursor anterior, índice realmente leído, cursor posterior, ganancia efectiva, fade restante y valores de mezcla antes/después. El callback copia a un array de 4096 registros; no reserva memoria ni hace I/O. La instrumentación por muestra es solo para este ensayo acotado, no una política de rendimiento del producto.

Se compilan dos ejecutables con la misma secuencia de entradas. El de referencia incluye el header original; el observado incluye la copia generada. CTest exige 4096 bytes idénticos de PCM S16 estéreo a 44100 Hz y dos resultados diferenciados: traza completa y pérdida diagnosticada. El control negativo omite el aporte de la ocurrencia 1 en la muestra 80 y debe retornar 2, conservando audio y registros.

El escenario contiene cuatro ocurrencias: bucle con región [3, 11), voz muda que consume catorce frames, retrigger de la primera clave mientras la anterior entra en fade y fade de ocho frames por fin de ventana. Las fronteras son [13, 768), [0, 14), [210, 220) y [300, 308), respectivamente. A partir de 768 solo queda PCM original tras cut_all. La ubicación en este ensayo pertenece al timeline principal de mezcla, no al dominio de salida convertido de SDL.

Desde Engine:

```powershell
cmake -S tests/audio_qa/voice_probe -B build/qa-020-voice `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=C:/Users/david/Workspaces/ayther/.qa-reference/audio-qa-initial-20260927/engine-source'
cmake --build build/qa-020-voice --config Release --parallel 4
ctest --test-dir build/qa-020-voice -C Release --output-on-failure
```

No modifica fuentes de referencia, dependencias, ABI ni código de producción. Límites: 1024 frames de salida, cuatro ocurrencias, 4096 registros y tres segundos por proceso. Los resultados se escriben en un directorio nuevo por prueba. La identidad por ocurrencia existe solo en la copia experimental. Exportación pública, causas de todas las finalizaciones, staging/router, persistencia y campaña real permanecen en las tareas posteriores.

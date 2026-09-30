# Oráculo de continuidad y disparo repetido — QA-025

RF-5, RF-6 y RF-15. Se extraen de rc.9 la puerta de flanco del evento live, LiveInstance, hd_fired y la rama de gameplay de play_oneshot_asset_file. Las decisiones y la llamada a HdMixer::start permanecen originales. El mixer de referencia se compila sin cambios; la copia observada añade identidad y registro de muestras. No se sustituye la puerta por una decisión de anclaje supuesta.

Límite del adaptador: evento ya seleccionado con asset listo, sin ventana, audio/transport activos y sin bypass. Un proveedor de datos entrega PCM ya decodificado e inmutable: 128 frames S16 estéreo/44100, valor de cada sample 1000+37×índice. No se ejecuta la decodificación ni el detector. La rama de preview y la apertura de ventanas rechazan su uso en este fixture en lugar de simularlas.

keep mantiene el evento durante cuatro cuadros de 16 frames: un solo disparo y cursor continuo 0→64. repeat usa actividad [sí,sí,no,sí]: el flanco del cuadro 3 crea una nueva ocurrencia con la misma clave, posición 0→16; la anterior continúa 48→64 bajo el fade original. lost pierde exclusivamente el registro de la puerta del cuadro 3 y devuelve 2, conservando estado y audio.

Desde Engine:

```powershell
cmake -S tests/audio_qa/continuity_oracle -B build/qa-025-continuity `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=C:/Users/david/Workspaces/ayther/.qa-reference/audio-qa-initial-20260927/engine-source'
cmake --build build/qa-025-continuity --config Release --parallel 4
ctest --test-dir build/qa-025-continuity -C Release --output-on-failure
```

Límites: cuatro cuadros, 64 frames de mezcla, dos ocurrencias, ocho decisiones, 128 registros de muestras y tres segundos por proceso. Se guarda el orden real de aporte, no un orden inventado por identidad. Los hooks no asignan ni escriben; límites o pérdidas hacen incompleta la traza. Se comparan estado/entradas y PCM con la referencia, además de verificar posiciones y ganancias. Esto no acredita una sesión completa, la salida SDL ni la lógica de secuencias con ventana.

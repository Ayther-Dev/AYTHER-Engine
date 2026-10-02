# Ensayos de captura común — QA-015 a QA-019

RF-6 y RF-10, plan §5.2 y pruebas T6–T7. Este ensayo de Engine suministra el bloque `known-pcm-v1` de QA-014 a AudioPlayer::buffer_emulator y ejecuta flush_emulator. Captura el postmix del mismo dispositivo lógico mediante SDL_SetAudioPostmixCallback. No recompone una mezcla ni sustituye AudioPlayer por un doble.

AudioPlayer es interno de Engine y no se instala en su SDK. Por eso el ensayo pertenece a Engine; Runtime no incorpora esas cabeceras. En el build nativo usa las fuentes/biblioteca de ese build. En el ensayo de referencia independiente usa explícitamente las cabeceras del commit rc.9 y la biblioteca del SDK publicado correspondiente. No se mezclan los cambios locales del motor con esa referencia.

## Condiciones y resultados

Entrada: 1024 sample_frames S16LE, estéreo, 44100 Hz, SHA-256 `5d764a90b6ae1b6606aa95bbd343c54fd1a7b3aa7f7f20e116afc53423081602`. La copia en data coincide con QA-014. Solo se entrega un batch; no se cargan assets, ROM, pack ni tomas. Se usan opciones predeterminadas explícitas y se mantiene activo el DRC. No se desactivan, desvinculan o eliminan streams auxiliares; no se les suministran datos en este caso.

CTest fija un dispositivo dummy de SDL a 44100 Hz, dos canales y bloques solicitados de 512 frames. Es un entorno controlado para observar la salida digital, no una reproducción de las condiciones acústicas de Play CE. El ejecutable exige el driver dummy y valida el formato efectivo de cada callback; un formato inesperado hace fallar el ensayo.

El callback solo copia PCM y registra rangos consecutivos en almacenamiento preasignado: hasta 32768 frames y 256 bloques. No asigna memoria, escribe archivos, modifica PCM ni espera por el consumidor. El ensayo termina de recoger bloques completos al alcanzar al menos 16384 frames. La espera de progreso tiene un límite de tres segundos; desinstalar el callback sincroniza su finalización antes de leer o destruir los datos. Los archivos se escriben después, fuera del hilo de audio. Estos límites son del ensayo, no sustituyen los presupuestos del comprobador.

Se conservan `capture.f32le`, `capture.toml`, stdout y stderr en un directorio nuevo por prueba. El manifiesto enumera cada bloque real, su rango semiabierto en sample_frames, el rango del timeline principal antes/después del flush, el DRC observado y los hashes. El origen de salida es el primer callback retenido, no el comienzo histórico del dispositivo o una posición de juego. No se generan cuadros adicionales.

La correspondencia se acredita con una única entrada controlada por la ruta real y una comprobación de contenido: se busca la ventana con menor error RMS frente al fixture normalizado, con umbral 0,025. El control negativo entrega ceros por esa misma ruta y exige rechazo (`known_input_not_found`). Esta coincidencia tolera la transformación efectiva del DRC y **no acredita una alineación exacta por muestra de cada efecto**: `match_sample_begin` es un ancla diagnóstica, mientras que los rangos de callback son exactos. No se utiliza tiempo de pared para asignar posiciones. No se acepta un archivo vacío, incompleto o de tamaño inconsistente.

## Ejecutar con la referencia Windows fijada

Desde Engine, con las rutas de QA-010 ya preparadas:

```powershell
$reference = 'C:/Users/david/Workspaces/ayther/.qa-reference/audio-qa-initial-20260927'
cmake -S tests/audio_qa -B build/qa-015 -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake' `
  '-DVCPKG_INSTALLED_DIR=C:/Users/david/Workspaces/ayther/AYTHER-Runtime/out/build/rc9/vcpkg_installed' `
  -DVCPKG_MANIFEST_INSTALL=OFF `
  "-DAyther_DIR=$reference/engine-package/ayther-engine-vpx-v0.1.0-rc.9-windows-x86_64/lib/cmake/Ayther" `
  "-DQA_ENGINE_REFERENCE_SOURCE=$reference/engine-source" `
  "-DQA_RUNTIME_BIN=$reference/runtime-package/bin"
cmake --build build/qa-015 --config Release
ctest --test-dir build/qa-015 -C Release -R '^audio_qa_main_capture_' --output-on-failure
```

En Windows, QA_RUNTIME_BIN copia las dependencias de la distribución identificada al directorio del ensayo. La evidencia debe verificar el SHA-256 del SDL3.dll efectivamente situado junto al ejecutable; no basta su versión. La configuración no descarga dependencias. Las rutas externas de este comando corresponden a la referencia local y deben mantenerse asociadas a su manifiesto.

El subdirectorio también se registra en el CTest nativo de Engine cuando se habilita el motor. Con ese build previamente configurado, compilar el target `audio_qa_main_capture` y seleccionar `^audio_qa_main_capture_`. Esa ejecución usa la identidad del build nativo, no se etiqueta automáticamente como rc.9 publicada. Las pruebas tienen timeout y su fallo bloquea CTest; no se convierten en skip.

No es la interfaz QA de Runtime ni un formato definitivo de evidencia. Un fallo de escritura deja diagnóstico y puede dejar archivos parciales; solo una prueba aprobada con hashes y rangos verificados se conserva como resultado del ensayo. El cierre tras una ventana fija no prueba el drenaje de audio de una toma. La puerta QA-020 y la fidelidad T12 siguen pendientes.

## Casos complementarios y límites

| Caso positivo / control negativo | Comprobación |
| --- | --- |
| auxiliary / auxiliary_missing | Señal F32 de síntesis de 1024 frames, localizada exactamente una vez en el postmix; el control omite su envío. |
| prime / prime_missing | Inserción real de 512 frames de silencio mediante prime_synth, entre dos marcas distintas; el control omite esa operación y no recibe una ubicación inventada. |
| conversion / conversion_missing | Preview S16 convertido a F32 a 44100 Hz; igualdad exacta de 2048 valores con la conversión normalizada, no con el contenido principal transformado por DRC. |
| closing / closing_discard | Preview ya producido y encolado con dispositivo inicialmente pausado; al cierre se drenan exactamente sus 1024 frames, sin más entradas ni cuadros. El control descarta esa cola antes del drenaje y debe fallar. |

Los casos de cierre fijan el objetivo de captura en 1024 frames; los demás mantienen 16384. Los controles negativos pasan CTest solo cuando el ejecutable devuelve el rechazo específico esperado (3 para contenido ausente, 4 para la señal exacta auxiliar ausente). Los archivos originales de cada ejecución permiten revisar el diagnóstico.

QA-016–QA-019 prueban señales y operaciones concretas. La conversión numérica no demuestra resampling a otra frecuencia ni fase de DRC; el drenaje de preview no demuestra cierre de todas las colas. No se acredita alineación global mediante un RMS, tamaño de archivo o tiempo de pared. Los manifiestos conservan exact_effect_alignment_verified=false hasta que exista evidencia para esa afirmación general.

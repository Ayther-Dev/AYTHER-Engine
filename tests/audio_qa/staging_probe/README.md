# Cuadros, staging y entrega — QA-020d4

RF-6, RF-7 y RF-10. Ensayo integrado en el CMake de conversion_probe, que reutiliza el SDK rc.9 y su copia observacional de SDL. Compara la biblioteca publicada de AudioPlayer con métodos extraídos literalmente de la referencia y compilados bajo otro nombre de clase, con observadores de estado. La separación de nombres evita sustituir símbolos del SDK. No cambia el layout, las reglas, los algoritmos o las condiciones de los métodos.

La configuración exige los hashes de audio_player.cpp y audio_player.h. Extrae inicialización, staging, flush y limpieza; no necesita decodificadores porque las entradas son PCM sintético. La copia observada registra mark_frame_boundary, cada batch, suma/reemplazo de router, estado antes y después del mute/mixer y descarte. Los callbacks de SDL conservan los envíos exitosos, la conversión y el postmix real.

Dos cuadros controlados aportan tres batches: hash 11 en [0, 32), hash 12 en [32, 48) y hash 22 en [48, 96). Los siete casos son batches, mute, add, replace, discard, suppress y lost. Se verifica la tabla completa de estados, no solo la igualdad del resultado. El control lost omite el estado post_mix y debe retornar 2 (staging_trace_incomplete), aunque su audio siga intacto.

Cada proceso conserva input.s16le, mix.f32le y staging.toml. El manifiesto incluye PCM de los estados intermedios, batches, identidad del cuadro controlado, marca local, timeline principal, vínculo del envío con el estado observado y tramos de conversión con fase Q32. Los archivos de entrada son S16LE estéreo a 44100 Hz y los de salida F32LE estéreo a 44100 Hz. Todos los índices cuentan sample_frames; los rangos son semiabiertos. El campo states_before de cada envío identifica cuántos estados habían ocurrido al aceptar SDL esos bytes.

La primera entrega es el priming real de 3072 frames, que no pertenece al timeline del mixer. La segunda coincide con el estado post_mix. El EOS drena lo ya producido; no se ejecutan cuadros ni se genera PCM nuevo durante el cierre. El registro de conversión se compara exactamente con la salida real del postmix.

Desde Engine, después de configurar conversion_probe con QA_ENGINE_REFERENCE_SOURCE, Ayther_DIR y CMAKE_PREFIX_PATH como se indica en su guía:

```powershell
cmake --build build/qa-020-conversion --config Release --parallel 4
ctest --test-dir build/qa-020-conversion -C Release -R audio_qa_staging --output-on-failure
```

Límites: dos envíos efectivos, hasta cuatro envíos almacenables, 32 estados con un máximo de 128 frames y cuatro batches cada uno, 32 tramos convertidos, 4096 frames de entrada/salida y tres segundos de espera. Los callbacks no reservan memoria ni escriben en disco. Superar un límite invalida el ensayo. Las pruebas no desactivan DRC ni modifican los paquetes publicados.

Esto acredita la frontera controlada de AudioPlayer, no cuadros provenientes de una ROM ni reproducción de tomas. El router recibe una señal de prueba; la observación interna de su resampler está en QA-020d3. No hay voz HD activa en estos casos; sus aportes se comprobaron en QA-020b/d2. Integrar esos hechos en un recorrido público de una sesión real sigue siendo parte del desarrollo pendiente.

## Unión con una voz HD — QA-020d6

Se añaden voice, voice_suppress y voice_lost; los siete casos anteriores conservan su alcance. Los nuevos usan play_event_hd y el pack sintético de conversion_probe, con su confianza explícita solo para la invocación. En el cuadro controlado 1 se observan 48 aportes del mixer, desde timeline 48 hasta 96, correspondientes a salida [3120, 3168). La supresión del original conserva el HD. El control pierde exclusivamente el registro de timeline 60 y devuelve 2 con audio idéntico.

La clase y el mixer observados tienen nombres propios para aislar los símbolos del SDK. Los métodos de inicio, carga WAV y conversión se extraen de la referencia. Se incluyen declaraciones de stb_vorbis y dr_flac; los decoders siguen procediendo del SDK. QA_DECODER_INCLUDE fija un prefijo local existente para dr_flac.h, validado por hash; su predeterminado es build/windows-native/vcpkg_installed/x64-windows/include. No descarga ni instala dependencias.

Se registran como máximo 96 aportes, sin memoria dinámica ni I/O en el hook. La preparación del asset usa un conversor temporal y queda delimitada antes de los envíos de gameplay; las posiciones de pista son frames del PCM convertido. Los diez casos de staging y el conjunto de 34 pruebas conservan PCM idéntico a sus referencias. Esta unión no sustituye los futuros contratos públicos ni las pruebas de tomas reales.

## Motivo y efecto de reanudación — QA-023

resume_voice y resume_lost consumen el estado y entrada de QA-021 por hash, invocan live_resume_decide y aplican su offset mediante play_event_hd con end/cut originales. La copia observada registra una etiqueta inmediatamente antes del return Restart, sin modificar el cálculo. Los 48 aportes empiezan en el cursor 614 de la pista convertida y aparecen en [3120, 3168) de salida. El cuadro lógico 120 de la decisión y el cuadro controlado 1 del staging son dominios distintos del ensayo.

resume_lost pierde solo el motivo y retorna 2 aunque audio, voz y posiciones coincidan. Ahora son doce casos de staging y 36 pruebas en el conjunto. Se conservan los límites anteriores y la comparación con SDK. Los originales, decoders y paquetes no se modifican.

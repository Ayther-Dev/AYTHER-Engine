# Ensayo de fase del conversor — QA-020a

RF-6, RF-10 y RF-15. Ensayo de viabilidad separado de la API de producto. Prueba si se puede observar la fase real del conversor SDL sin cambiar su resultado, antes de integrar esa observación con AudioPlayer y el postmix. No es un replay de Runtime ni supera por sí mismo QA-020.

La configuración comprueba los hashes de SDL_audiocvt.c y SDL_audioresample.c de las fuentes locales SDL 3.4.8 identificadas, copia el árbol a su directorio de build y añade un observador después de cada conversión exitosa. El original, la caché de dependencias y la DLL publicada no se modifican. La copia alterada queda identificada como tal y mantiene la licencia de SDL. No se cambia el algoritmo, ratio, número de frames solicitados, ganancia o gestión de colas.

El observador registra los valores utilizados por la operación: formato, frames consumidos/producidos, paso Q32, fase anterior/posterior, padding y ganancia. Copia el PCM prestado en un almacenamiento de tamaño fijo y descarta sus punteros al salir del callback. Se registra antes de crear streams y se retira después de destruirlos; esta condición de vida es obligatoria para este contrato experimental sin sincronización propia. No se escribe en disco dentro del callback.

Hay dos ejecutables idénticos salvo la exigencia del observador. Uno carga la DLL publicada y verifica que carece del símbolo experimental; otro carga la copia instrumentada y lo exige. Ambos convierten el fixture QA-014 con idénticas operaciones, ganancia 0,75 y solicitudes de tamaños variables. Los casos prueban S16/F32 a 44100 Hz, 44100→48000 Hz y ratio inicial 1,000083327293396 seguido de 1,0075 después del primer consumo. Se comprueba continuidad y conservación de fase, correspondencia del PCM observado con el devuelto y comparación byte a byte frente a la referencia publicada. La fase posterior se observa directamente; la ecuación solo verifica coherencia, no sustituye ese dato.

Límites: una entrada de 1024 frames, hasta 4096 frames de salida, 128 operaciones registradas, tres formatos/ratios controlados, sin dispositivo ni reloj de pared. El agotamiento del espacio falla. El EOF se conserva con FlushAudioStream y no se añade otro contenido de entrada. No se acredita todavía el cierre de la sesión ni el efecto de cada voz dentro del filtro, los resets entre pistas, la mezcla de streams o todos los formatos posibles. La comparación en estos casos no equivale a T12 ni a identidad binaria entre SDL publicada y la recompilada.

## Reproducir en Windows

Desde Engine:

```powershell
cmake -S tests/audio_qa/conversion_probe -B build/qa-020-conversion `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_SDL_SOURCE=C:/Users/david/Workspaces/ayther/AYTHER-Runtime/.deps/vcpkg-clean/buildtrees/sdl3/src/ease-3.4.8-4fc18f166d.clean' `
  '-DQA_RUNTIME_BIN=C:/Users/david/Workspaces/ayther/.qa-reference/audio-qa-initial-20260927/runtime-package/bin'
cmake --build build/qa-020-conversion --config Release --parallel 4
ctest --test-dir build/qa-020-conversion -C Release --output-on-failure
```

No descarga paquetes ni actualiza locks. Cada ejecución conserva PCM, metadatos, hashes y logs en un directorio nuevo. La falta de observador, una DLL distinta de la referencia fijada, discontinuidad, desbordamiento o diferencia de PCM hacen fallar el ensayo, sin convertirlos en resultados omitidos.

Alternativa descartada para este ensayo: derivar la fase solo de GetAudioStreamQueued, Available o los bytes solicitados por el callback público. Esos datos no observan la fase fraccionaria ni cada consumo interno. Cambiar o desactivar el DRC tampoco probaría la transformación investigada. Esta decisión experimental aún no adopta una DLL modificada como dependencia del producto.

Los controles `drop` y `phase` alteran exclusivamente la copia de observaciones dentro del ensayo (pérdida del primer tramo o un bit de fase), nunca el PCM de SDL. Deben fallar con el diagnóstico de integridad correspondiente.

## Mezcla común — QA-020b

Al configurar QA_ENGINE_REFERENCE_SOURCE y Ayther_DIR de la referencia rc.9, se añade audio_qa_common_mix. Consume también los paquetes instalados indicados mediante CMAKE_PREFIX_PATH (el prefijo vcpkg de Runtime utilizado por QA-015). Compilar y ejecutar CTest con los comandos anteriores incluye cinco casos adicionales: all, gain, muted, main_only y missing_contribution.

El SDK publicado ejecuta AudioPlayer; la DLL privada observa envíos y conversiones. Se identifican cinco familias: principal, synth, preview PCM, preview de asset y sustituciones por hash. La principal tiene además una voz HD real. El registro de cada envío exitoso se hace bajo pausa inicial; la captura se publica con un flag atómico al reanudar. Durante la observación solo el hilo de audio modifica los buffers, y el lector espera a su finalización, pausa el dispositivo y desinstala el postmix antes de leerlos.

Se guardan mix.f32le (postmix real), contributions.f32le (copias diagnósticas) y mix.toml con rangos, fases y ganancias. Los rangos de contribución se localizan por el orden de consumo dentro del callback lógico. Una suma acotada a [-1,1] tras cada aporte comprueba la correspondencia sin modificar el audio real. El control de pérdida omite una contribución del registro y falla. El caso de mute sigue exigiendo consumo y posiciones aunque el PCM final sea cero. main_only conserva explícitamente las auxiliares inactivas.

Límites: 8192 frames finales, cinco rutas, 256 tramos y 40960 frames de contribuciones. La espera tiene un máximo de tres segundos. La principal, sin EOS, deja 7 frames pendientes de resampling en este escenario: no se presenta como cierre completo. La relación interna entre la voz HD y la principal, las rutas del router y la campaña real requieren ensayos posteriores.

El pack de data contiene únicamente el fixture PCM sintético; el registro de confianza se utiliza solo en esta invocación. generate_fixture.py lo regenera con Python y cryptography 50.0.1 usando una clave pública de pruebas determinista, que nunca debe incorporarse a una confianza de producto. CTest verifica hashes del pack y del registro; no instala herramientas para regenerarlos.

## Drenaje con generación congelada — QA-020c1

Los casos drain y drain_loss fijan el límite de salida disponible después de declarar EOS en las cinco colas existentes, con el dispositivo pausado. No hacen nuevas llamadas de alimentación ni otro avance del mixer. Un contador atómico observa cualquier nuevo envío después de congelar la generación y exige cero. El caso positivo conserva los 4096 frames principales, incluidos los siete retenidos antes de EOS; las otras colas también se drenan. El límite de captura pasa a ser el máximo de esos límites ya producidos, dentro del buffer de 8192. Si el último callback supera el límite, solo se conserva su prefijo aplicable.

El control borra la cola principal después de fijar ese límite. Conserva audio y diagnóstico, pero retorna 4 (drain_incomplete). El caso positivo exige consumo completo y disponibilidad final cero por stream; la voz HD puede seguir activa porque no se sintetiza su porción futura. No es todavía el cierre de Runtime ni prueba resets con fase activa.

## Descarte con fase activa — QA-020c2

El caso reset consume 97 frames convertidos, limpia el stream y vuelve a alimentarlo con los valores del fixture en orden intercalado inverso. El observador conserva cola y fase antes/después de ClearAudioStream. Los tramos de entrada usan origen absoluto distinto tras el descarte, mientras la salida conservada sigue siendo contigua. La salida se compara con la DLL publicada. Los controles reset_drop y reset_phase alteran solo el registro y deben fallar.

El análisis aislado de observer_impl.h requiere precargar también audio/SDL_audioqueue.h; la unidad SDL original ya incluye esa declaración. El contrato sigue siendo experimental y su registro/retiro está limitado a períodos sin streams en uso.

## Inserción tras resampling — QA-020c3

Los casos prime_resample, prime_missing y prime_loss abren primero un dispositivo dummy a 48000 Hz. AudioPlayer utiliza su inicialización normal y el ensayo verifica el formato efectivo; la variable de entorno de frecuencia por sí sola no cambió el predeterminado del backend. Se insertan 512 frames reales de prime_synth entre dos señales de 1024 frames a 44100 Hz y se drenan las cinco familias sin producir audio nuevo.

El registro de envíos comprueba los bytes reales de cada chunk, con un máximo de 16 chunks de hasta 65536 bytes. La copia SDL expone el soporte del filtro compilado y cada conversión registra esos valores junto con la fase real. El silencio de entrada [1024, 1536) se relaciona con el soporte de salida [1109, 1678); el tramo puro [1120, 1666) debe ser cero. Otros streams continúan contribuyendo al postmix común. La señal real se conserva; la suma de contribuciones solo verifica su correspondencia.

prime_missing omite la inserción y prime_loss pierde un registro de conversión sin alterar PCM. Ambos conservan diagnóstico y retornan 5 (prime_alignment_incomplete). Las fronteras candidatas en resultados incompletos no certifican silencio. La observación no asigna a una pérdida ceros ficticios ni cambia la fase del conversor. QA-020 sigue pendiente de la auditoría de rutas internas.

## Cambios durante consumo — QA-020d5

Los casos gain_switch, mute_switch, transport_cut, previews_cut y natural_end observan las cinco familias antes y después de la frontera de salida 512. El harness pausa en ese límite, aplica la API real y reanuda; este control pertenece exclusivamente al ensayo. No se alimentan más datos. ClearAudioStream y DestroyAudioStream notifican cola y fase reales, y el registro distingue su instante del último aporte convertido de cada ruta. Las rutas cortadas dejan su cola descartada explícita; las restantes deben terminar su consumo.

gain_switch_loss omite el registro de la frontera sin cambiar el audio y retorna 6 (transition_trace_incomplete). CTest incluye estos seis casos; el conjunto de conversión, mezcla y staging tiene 31 pruebas. Se conservan los mismos límites de memoria y espera. El callback de fin se registra antes de crear streams y se retira después de destruirlos; los punteros prestados se eliminan al copiar observaciones. No es una API pública ni una medición de latencia física.

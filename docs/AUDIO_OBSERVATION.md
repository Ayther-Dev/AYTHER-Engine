# Observación de audio QA

Contrato Engine observation API 1.0. Implementación parcial del plan de QA;
todavía no anuncia replay, captura digital ni traza completa como capacidades.

El host configura Config.audio_observer antes de crear AytherSession. El contexto
pertenece al host y permanece vivo hasta que la destrucción de la sesión haya
detenido sus productores. Las vistas de audio_observer.hpp vencen al retornar
cada callback; se copian a almacenamiento acotado. Un callback no modifica
Engine, no espera, no escribe a disco y no serializa. Con callbacks nulos la
sesión sigue la ruta ordinaria.

## Catálogo de asignaciones — QA-045; RF-2 y RF-14

La observación parte del parser real de audio_events.toml en el core Rust.
events_from_toml y la entrada C anterior comparten las mismas reglas con la
variante observable. Esta conserva el ordinal antes de filtrar y el valor
canónico de firma que calculó el parser. No hay otro parser ni nuevas reglas
de aceptación. Firmas cero, assets vacíos, normalización y orden conservan el
comportamiento previo, aunque alguna entrada resulte problemática para el caso.

La extensión C ayther_audio_events_parse_observed es aditiva: no cambia el
layout ni las firmas de la ABI existente. Su vista 1.0 ocupa 72 bytes en los
targets de 64 bits admitidos, comprobados desde Rust y C++. Los textos llevan
longitud, no terminador. La función devuelve el total aceptado, también cuando
cap sea menor; los callbacks no se recortan a cap. Para dimensionar y cargar,
la sesión usa la llamada ordinaria sin observación y observa una sola pasada
de llenado, incluso cuando el recuento anterior sea cero.

| Hecho | Significado |
| --- | --- |
| pack_catalog_begin | Origen pack/provided_text y cantidad declarada antes del filtrado. Su FactId identifica esta lectura del catálogo. |
| pack_assignment_declared | Ordinal, firma textual/canónica, asset declarado y tamaños. El antecedente es el inicio del catálogo. |
| pack_assignment_parse_result | Aceptación o rechazo desde la rama real del parser, etapa catalog_parser y motivo. El antecedente es la declaración correspondiente. |
| pack_catalog_end | Declaraciones observadas, aceptaciones del parser, duplicados e integridad del inventario. |
| pack_catalog_unavailable | No se obtuvo un inventario interpretable; conserva origen y motivo. No significa inventario vacío. |

La identidad de entrada es catálogo + ordinal, incluso para firmas iguales.
duplicate_of_ordinal señala la primera declaración con la misma firma canónica;
incluye alias como 0x1 y 0X01 y puede vincular entradas luego rechazadas por otro
motivo. Una primera declaración tiene no_aplicable; una firma inválida o una
limitación de observación tiene desconocido con razón. El duplicado no se borra
ni se convierte en otra carga exitosa. La asignación finalmente conservada por
el mapa de sesión sigue su política anterior.

La cantidad aceptada por el parser no es cantidad de claves únicas, assets
decodificados, selecciones ni reproducciones.

## Carga efectiva — QA-046; RF-2, RF-11 y RF-14

La misma carga de sesión emite hechos después de actualizar el mapa y después
de su preparación habitual de assets. No vuelve a interpretar el catálogo ni
revalida archivos para observarlos.

| Hecho | Significado |
| --- | --- |
| pack_assignment_loaded | Incorporación efectiva al mapa, etapa session_assignment_map, firma, asset efectivo, canales, parámetros parsed_duration/parsed_span/parsed_looping recibidos del parser y motivo inserted/replaced_existing_signature. Enlaza el resultado del parser; una sustitución enlaza también la incorporación anterior de esa firma. |
| pack_assignment_asset_result | Etapa asset_prewarm, intento actual o preparación omitida, origen pack/disk cuando se eligió en este intento, presencia y estado de la caché, bytes PCM y motivo observado. Enlaza la incorporación correspondiente. |
| pack_assignment_load_end | Cantidad de actualizaciones efectuadas, claves distintas finalmente cargadas e integridad de la observación de carga. Enlaza el catálogo o su diagnóstico de indisponibilidad. |

El ordinal de origen se conserva aunque haya filas rechazadas antes. Los tamaños
del asset declarado y efectivo permiten reconocer el recorte de la interfaz C
existente. Una entrada rechazada por el parser no produce incorporación al mapa.
Los campos parsed_* describen la entrada de esta actualización; no afirman que
se hayan borrado parámetros previos de una firma duplicada ni describen una voz.
Un asset ausente o corrupto puede pertenecer a una asignación incorporada: no se
presenta como reproducción correcta ni como rechazo del mapa.

La preparación conserva su política previa: los assets ya encontrados no se
preparan nuevamente, y un nombre vacío no inicia preparación. Esas ramas quedan
registradas como already_prewarmed y empty_asset. La consulta de caché solo lee
su estado actual; no llama al decodificador, no consulta archivos, no cambia
temporizadores ni provoca reintentos. Sin entrada en caché, disponibilidad y
bytes PCM son desconocidos, con razón not_cached. Si no se elige origen en este
intento, source es desconocido; no se inventa a partir del nombre del asset.

Cero aceptaciones con rechazos individuales deja un mapa vacío observado; un
TOML inválido conserva su diagnóstico y una observación incompleta. Los errores
de preparación no vuelven incompleto un registro que pudo observarlos íntegro.
La observación no corrige las decisiones de carga, selección ni reproducción.
load_observation_complete acredita la emisión de hechos de esta etapa, no una
carga completa del inventario. El comprobador deberá usar las discrepancias
registradas para declarar incompleta una carga cero o parcial conforme a RF-2;
esa clasificación del resultado global no corresponde al adaptador de Engine.

## Entrada al detector — QA-047; RF-5 y RF-14

Antes de cada llamada real a process_frame_ex en la sesión se observa el lote
entregado. Se identifican por separado el detector live y el de analysis. La
observación no vuelve a consultar al core, no decodifica escrituras ni ejecuta
el detector o búsquedas de candidaturas. Los argumentos de procesamiento se
mantienen sin cambios.

| Hecho | Significado |
| --- | --- |
| detector_input_batch | Identidad del lote, detector, cuadro recibido por el detector, procedencia de escrituras y cantidades de escrituras y eventos PCM entregados. |
| detector_input | Identidad propia y causa del lote; tipo chip_write/pcm_event, origen, ordinal source_index y todos los valores semánticos de la entrada. |

El cuadro de FactView conserva los 64 bits de la posición de sesión.
detector_frame conserva aparte el valor de 32 bits que recibe el detector
actual, incluida una conversión por desbordamiento si existiera. No se presenta
ese valor convertido como la posición completa. El host aún deberá relacionar
la sesión y su ejecución con la toma en el puente Runtime.

Las escrituras conservan chip, address, data y cycle. Este último es un ciclo
de CPU dentro del cuadro, no una identidad estable ni una muestra de salida.
Su origen abi_snapshot/legacy_core procede de la rama realmente usada por el
observador del emulador. Los PCM ya desempaquetados conservan tipo numérico y
nombre, canal, envolvente, panorama, comienzo, bucle e incremento de lectura;
su origen es core_event_queue. Los valores numéricos desconocidos se conservan.

source_index conserva el orden de cada array. La secuencia del productor
detector_input ordena la emisión de hechos; no inventa causalidad entre FM/PSG
y PCM. El lote declara cross_stream_order=independent, conforme al contrato
del detector. Cada entrada enlaza su lote; su pertenencia no afirma que haya
generado un evento detectado, una candidatura o un disparo.

Un lote vacío acredita los argumentos vacíos de esa llamada, sin demostrar
por sí solo que la fuente haya estado activa. QA-048 completará intervalos y
contadores de actividad; QA-049 aportará las salidas del detector y sus vínculos.
Los eventos del core filtrados antes de llegar al detector no se presentan
como entradas procesadas. Esta etapa no anuncia detector_ingress completo
para la aceptación de Runtime.

El adaptador utiliza arrays locales fijos de hasta once campos y una causa,
sin reservar ni retener memoria por entrada. El trabajo es O(escrituras + PCM)
y no limita ni recorta los arrays que Engine procesa. Una prueba emite 4097
entradas usando un receptor acotado. Sin callback no recorre las entradas ni
consume identidades. El agotamiento de identidades marca la emisión incompleta;
no altera el procesamiento del detector. La pérdida del receptor se registra
fuera del callback y no puede deducirse del retorno del productor.

## Metadatos de fuente — QA-048a; RF-5, RF-11 y RF-14

La lectura de eventos mantiene la política previa, pero permite conservar un
AudioPollObservation: rama alcanzada, intento y estado del sondeo, cantidad
efectivamente devuelta, e intento, estado y snapshot de estadísticas. La ruta
ordinaria y poll_audio_events_observed_v1 comparten exactamente una operación
de consumo. No se sondea de nuevo para obtener el diagnóstico. Las estadísticas
son las que ya se consultaban antes del sondeo, no un recuento posterior.

Se distinguen ausencia de API, salida inválida, capacidad/callback ausente,
layout incompatible, error del sondeo y sondeo correcto, incluido cero. Un
error de estadísticas conserva su código y no impide un sondeo que la política
anterior permitía; un layout incompatible sigue impidiendo ese sondeo. El
contador dropped_events conserva el valor saturado del transporte, incluido
UINT32_MAX; no se transforma en cero ni se calcula una diferencia implícita.

Una consulta de suscripciones de solo lectura conserva resultado, máscaras
soportada/activa/solicitada y cuadro de activación. No aplica una máscara ni
habilita fuentes. AudioWritesView conserva el resultado de la lectura ABI
intentada, incluso cuando después se usa el fallback legacy; si no hubo intento
queda ausente. Una lectura fallida seguida de fallback no se etiqueta como
lectura ABI correcta. La información de overflow del snapshot permanece
disponible para su correlación en la etapa siguiente.

QA-048b conecta estos metadatos a intervalos por cuadro; las pruebas de QA-048c
cubren los estados, cambios y límites. Las consultas correctas requieren comprobar
tamaño y versión de sus metadatos antes de interpretar el estado en el registro. Las
estructuras internas de lectura no amplían el inventario de cabeceras del SDK
ni anuncian capacidades nuevas de Runtime.

## Intervalos de fuente y detector — QA-048b–c; RF-5, RF-11 y RF-14

Cada produce_frame observado emite dos audio_source_interval antes de alimentar
el detector: chip_writes y typed_audio_events. El intervalo es [begin_frame,
end_frame), con end_frame=begin_frame+1. Un extremo no representable se conserva
como desconocido por interval_overflow. Los cuadros repetidos o retrocesos no
se fusionan: cada lectura conserva identidades nuevas.

Se registran los estados active, disabled, unavailable, interrupted y unknown,
con motivo. Una fuente activa requiere suscripción efectiva e información
interpretable de su lectura; la ausencia de una capacidad no se convierte en
un cero verificado. Los estados describen cada ruta técnica; no deciden por sí
solos si esa ruta es necesaria para el hardware de la ejecución. Esa relación
corresponde al contexto de ejecución y al comprobador.

Para escrituras se conservan origen, cantidad reportada, datos recibidos,
entradas preparadas, resultado de lectura ABI, validez del snapshot, generación,
overflow y máscara activa. Un snapshot actual con cero escrituras, suscripción
activa y sin overflow acredita cero incluso si el vector vacío evitó leer una
región, siempre que no haya lectura fallida ni entradas preparadas que contradigan
ese cero. Un fallback posterior a error ABI conserva la interrupción incluso con
contador cero. El overflow
acredita pérdida, pero su cantidad queda desconocida porque el core expone un
bit. Sin overflow en un snapshot válido se registra pérdida cero.

Para eventos tipificados se registran resultado concreto del sondeo, estados
de consulta, versiones/tamaños reportados, cantidad recibida, PCM preparados y
los contadores de descarte de las ramas reales: fuente distinta de PCM, esquema
incompatible y tipo no admitido. No se repite el filtrado para contarlos. Una
lectura fallida deja received desconocido aunque el buffer preparado esté vacío.
La suma de preparados y filtrados se contrasta con el sondeo; una discrepancia
queda como typed_count_mismatch.

La pérdida del transporte conserva el total previo al sondeo y, cuando puede
demostrarse, la diferencia desde el sondeo anterior. Se exige continuidad de
cuadros, contador interpretable y no saturado. Sin referencia inicial, un total
cero acredita pérdida cero; un total mayor queda desconocido para ese intervalo.
Retroceso del contador, saturación, salto/repetición de cuadro o estadísticas
inválidas impiden atribuir una diferencia al cuadro. El dato total no se borra
ni se atribuyen pérdidas históricas a un intervalo arbitrario. El snapshot de
estadísticas no representa un recuento posterior al sondeo.

Después del procesamiento, audio_detector_interval registra detector live o
analysis, disponibilidad, call_completed, sustitución habilitada o no, entradas
preparadas, entregadas y no entregadas. Enlaza los dos hechos de fuente y el
lote de entrada cuando existen. Entregadas significa argumentos de una llamada
real completada; no significa eventos reconocidos, candidaturas o selecciones.
La sustitución desactivada no se confunde con detector inactivo. El análisis
enlaza las lecturas que prepararon sus buffers sin volver a consumir la cola.

La consulta de suscripciones adicional es de solo lectura y solo se realiza
con observación activa. La política de sondeo, las suscripciones y las decisiones
de audio permanecen iguales. El seguimiento retiene únicamente una referencia
de cuadro/contador; los hechos usan arrays fijos, sin reservas ni persistencia
en callbacks. La integridad de emisión no acredita recepción, durabilidad ni
que todas las fuentes necesarias hayan estado activas. El puente Runtime y la
clasificación global del resultado siguen pendientes.

La prueba de integración cambia la suscripción del core sintético y verifica la
secuencia activa → lectura obsoleta con fallback → desactivada → activa. Las
pruebas de límites distinguen ausencia y versiones incompatibles, overflow de
escrituras, capacidad del sondeo, sumas de contadores de 32 bits sin wrap, pérdida
inicial desconocida, saturación y reinicio del contador, cuadros discontinuos o
repetidos y recuperación. El extremo UINT64_MAX permanece desconocido; no vuelve
a cero. La observación desactivada no consume identidades ni actualiza la referencia
de pérdida. La entrega nula al detector conserva las entradas no entregadas.
Estos ensayos de componente no acreditan la campaña Golden Axe ni T12 completo.

## Salidas del detector — QA-049; RF-5, RF-9 y RF-14

El adaptador interno DetectorTracker observa operaciones ya completadas y arrays
ya obtenidos por el consumidor. No incluye consultas al detector, procesamiento,
búsqueda, clasificación de fallo ni acceso a archivos. QA-049b conecta el adaptador a la sesión y QA-049c verifica las salidas reales
con observación activa, desactivada y un receptor que pierde hechos.

Cada detector live o analysis mantiene su propia referencia de estado. Una
operación emite detector_state_operation con identidad del productor detector,
cuadro de sesión de 64 bits, cuadro del detector de 32 bits cuando corresponde,
acción, parámetro y orden del estado mutable. Las acciones son creación, reset,
procesamiento, cambio PAL, máscara inicial activa, finish y limpieza de cerrados.
El procesamiento enlaza el lote de entrada y la operación previa; las demás
mutaciones enlazan el estado previo. Esto conserva transitivamente los ingresos
que participaron en el estado consultado, con espacio constante en el tracker.
No afirma que todos esos ingresos dispararon un evento ni escoge una escritura
concreta por parecido de firma, canal o tiempo.

Creación o reset observados establecen una nueva referencia conocida. Un
antecedente ausente, lote de entrada inexistente o parámetro requerido ausente
hace incompleta la historia. Una operación sin observador invalida esa referencia;
la reanudación no inventa las mutaciones intermedias. Un reset posterior observado
puede establecer estado conocido de nuevo, sin borrar el diagnóstico del tramo
anterior en el receptor. Un snapshot de salida no modifica la cadena del detector.

Las consultas emiten detector_output_batch y una fila por elemento del array:
detector_active_channel o detector_closed_event. Se registra uso (selección runtime,
aprendizaje live, resultado de análisis o inspección), capacidad, retorno, elementos
observados y total si la consulta previa lo expuso. El total de activos no se deduce
de otra consulta: puede quedar desconocido. read_complete comprueba la coherencia
entre los metadatos y el array entregado; no declara que una consulta limitada
represente todo el detector. La completitud del retorno del adaptador combina esa
coherencia, historia disponible y emisión; no prueba recepción ni persistencia.

Cada fila conserva índice de origen, firma, instrumento, chip, canal y pitch.
Los cerrados conservan además start_frame, end_frame y velocity exactamente como
los entregó el detector, sin reinterpretar la semántica de los extremos ni suponer
orden cronológico del array. Esos cuadros pertenecen al reloj del detector; el
cuadro del hecho es el de observación. Una fila activa es un snapshot, no un nuevo
key-on, y una fila cerrada leída después de finish no se etiqueta automáticamente
como un final natural ni como un cierre provocado por finish.

Las filas enlazan el lote de salida y este enlaza la última operación real.
Firmas repetidas reciben identidades de fila distintas. El resultado de activos
retiene hasta 64 enlaces para su consumidor; un array mayor sigue emitiéndose,
pero ese resultado marca incompletitud porque no conserva todos los enlaces.
Los cerrados se emiten sin retener historial ni imponer ese límite. El tracker
ocupa como máximo 64 bytes, comprobado al compilar; el trabajo es lineal respecto
al array observado y usa memoria constante. Las pruebas cubren 1025 cerrados y
el límite de 64/65 enlaces activos. Los callbacks siguen el contrato público de
vida útil y no retroalimentan decisiones de audio.

### Conexión a sesión — QA-049b

La sesión registra la creación de cada detector solo tras obtener su handle.
Registra después de la llamada real los reset, cambios PAL, máscara inicial,
procesamiento live/analysis, finish de análisis y limpieza de cerrados live.
Los lotes de entrada de QA-047 son los antecedentes del procesamiento, y los
trackers separados conservan la cadena de estado de cada detector. El indicador
interno de integridad acumula las emisiones incompletas sin controlar el audio.

Las salidas se observan en los arrays usados por la ejecución: cerrados antes del
aprendizaje live, activos antes de las decisiones de sustitución y resultados
finales de análisis. También se observa la consulta pública audio_live_active
cuando realmente se realiza, con uso inspection. No se agrega una consulta para
observar canales activos si la ejecución no la hacía. Los vínculos de las filas
activas de la consulta de selección quedan disponibles para las próximas etapas
de trazabilidad; las consultas de inspección no reemplazan esos vínculos.

El retorno de get se conserva en la misma llamada antes ignorada. La traza limita
su vista a los elementos devueltos y la capacidad existente; cualquier discrepancia
con el total consultado queda incompleta. No cambia el array ni el bucle consumidor.
Si el total cerrado es cero, el contador existente permite emitir un lote vacío
sin llamar a get. No hay un finish adicional ni cambios de política de limpieza.

La integración con core y ROM sintéticos verifica ambas creaciones, procesamiento,
limpieza, los cuatro usos de consulta, reset público, configuración de análisis,
máscara inicial y finish. Es una prueba de conexión y metadatos; no sustituye la
comparación de valores de QA-049c ni la campaña Golden Axe.

### Comparación de valores y causalidad — QA-049c

La prueba de sesión ejecuta tres veces ocho cuadros y analiza sus respectivas
tomas: observación desactivada, observación completa y receptor que descarta hechos
después de 32 callbacks. Compara campos de activos/cerrados y estado serializado
del juego. El receptor de prueba comprueba online el orden del productor y los
vínculos de cada operación con su estado previo e ingreso, del lote con la operación
y de cada fila con su lote e índice. Copia valores en arrays acotados de 256 filas
por uso; no reserva memoria ni realiza consultas dentro del callback.

La ROM sintética usa el programa canónico con título AYTHER QA DETECTOR 128.
Su CRC32 6ecdfdd0 produce en el core de pruebas una secuencia PSG que abre y cierra
el canal 1 dentro de cada cuadro y conserva otros canales activos. Se exigen
salidas no vacías en cada comparación de aprendizaje; un ensayo anterior con el
título canónico no ejercía cierres live y queda conservado como prueba fallida.
No se modifica el core ni se usan ROMs comerciales en esta validación.

Las filas activas coinciden con la consulta pública y con la vista de selección.
Los cerrados de análisis coinciden con el array devuelto por la sesión. Los cerrados
de aprendizaje se comparan además con otro detector real alimentado por las mismas
escrituras del FrameView; ese detector independiente existe únicamente en el test.
El core sintético no entrega PCM tipificado; esta comparación no acredita esa ruta
ni las políticas de sustitución del pack Golden Axe. El reset posterior deja la
consulta activa vacía y conserva la cadena de estado observada.

El ensayo completo observa 48 filas activas, 16 cerradas live y 10 cerradas de
análisis. Las tres ejecuciones producen iguales salidas y estado serializado.
La pérdida del receptor no retroalimenta el audio; esto no acredita que Runtime
detecte o conserve esa pérdida, cuya implementación sigue pendiente. Son pruebas
de componente e integración de Engine, no T12 completo ni aceptación del producto.

## Visitas del índice de instrumentos — QA-050a

AudioMatchIndex ofrece resolve_observed con Query (instrument, pitch) y un
callback síncrono void noexcept. Se invoca una vez por entrada del equal_range
real, antes del filtro de nota y del desempate. Conserva entradas duplicadas y
notas incompatibles; no visita otros instrumentos. Instrumento desconocido,
instrumento cero e índice vacío no producen visitas. El orden es el recorrido
existente del índice hash, sin ordenamiento adicional ni garantía entre builds.

CandidateView contiene firma, regla y nota de la entrada. Una visita no implica
selección ni aceptación. La vista dura hasta el retorno del callback; el receptor
debe copiarla si necesita conservarla. No puede reentrar, modificar índice o
salida, bloquear ni realizar E/S. La firma ordinaria resolve se conserva y usa
el mismo algoritmo con callback vacío; el objeto no adquiere estado adicional.

La observación agrega espacio constante por consulta y no limita el recorrido.
El test usa ocho posiciones de recepción y verifica que desbordarlas no recorta
las 66 visitas ni cambia el resultado. También verifica multiplicidad, ausencias,
nota incompatible, nota desconocida, salida nula y prioridad/desempate. Dieciséis
pruebas seleccionadas pasan, incluidas las regresiones de regla e identidad.
La conexión con hechos de sesión sigue pendiente en QA-050c; esta API por sí sola
no registra candidaturas en la traza ni completa QA-050 o T12.

## Formación de candidatos de secuencia — QA-050b

seq_anchor_frame_observed comparte el algoritmo con la firma ordinaria
seq_anchor_frame. Por cada par firma/secuencia realmente recorrido emite una
SeqAnchorCandidateView prestada con cuadro, firma, índice, clave y etapa:
disabled, internal, not_head, formed o accumulated. Las primeras tres describen
salidas tempranas del recorrido; formed y accumulated describen la incorporación
al candidato y conservan trigger y el contador real head_hits. Esos dos campos
no son observaciones de coincidencia en las etapas tempranas.

Las visitas siguen el orden de entrada y conservan firmas repetidas; no deduplican
ni ordenan para la traza. Una candidatura formada puede perder el quorum, ser
reclamada por otra secuencia o perder prioridad. Su visita no afirma que ancle.
El callback void noexcept es síncrono y no puede reentrar, modificar entradas o
estado, bloquear, hacer E/S ni conservar la referencia. La observación usa espacio
constante por visita; no agrega búsquedas ni reservas. Las reservas y límites
propios del algoritmo anterior siguen vigentes.

El receptor del test conserva 16 visitas; con 128 visitas, descartar las restantes
no modifica anclajes ni estado. Se verifican etapas tempranas, quorum insuficiente,
duplicados, reclamaciones, prioridad de continuidad, entradas vacías y ajuste del
tamaño del estado. La regresión previa de secuencias también pasa. La tabla de
replay todavía usa la firma ordinaria; su conexión observacional y la de sesión
se completan en QA-050d. Las decisiones finales corresponden a QA-051.

## Hechos de consultas de asignación — QA-050c1

El adaptador interno MatchObservation representa una sola consulta real. No recibe
catálogo, índice, detector ni puntero de resultado; no puede buscar ni seleccionar.
El consumidor informa una vez el resultado del sondeo exacto ya realizado y usa
el adaptador como callback de resolve_observed cuando corresponde recorrer el
índice de instrumentos. Los ocho usos identificados se distinguen: live_assignment,
live_sequence_interest, live_voice_route, replay_trigger, replay_voice_route,
asset_coverage, bare_frame_mute y export_audio.

Los hechos usan el productor selection (5) de la API de observación 1.0:

- assignment_query: firma, instrumento, nota, uso y source. Su causa es la fuente
  observada que aporte el consumidor. Fuente ausente o con identidad cero queda
  unknown con motivo y provenance_complete=false, sin inventar un antecedente.
- assignment_exact_probe: firma y found del sondeo real; causa assignment_query.
  found=false no declara una candidatura ni afirma ausencia de coincidencia por
  instrumento. found=true emite un candidato exacto.
- assignment_candidate: firma de la entrada, regla numérica/textual, nota cuando
  corresponde, origin y visit_index desde cero. Enlaza consulta y sondeo observado.
  El candidato exacto no tiene regla de nota: pitch=not_applicable. Las entradas
  de instrumento conservan la nota real, incluidos 255 y valores incompatibles.

Cada consulta obtiene identidad propia aunque repita firma y cuadro. Los candidatos
mantienen orden y multiplicidad de visita; ninguno significa selección. Ausencia
del sondeo deja probe_observed=false y la integridad local incompleta. complete()
describe los hechos emitidos y la procedencia aportada; no confirma recepción,
persistencia, cierre de consulta ni cobertura de sesión. QA-051 añadirá decisiones.

El adaptador ocupa como máximo 256 bytes y usa campos/causas en pila, sin historial
ni asignaciones de memoria. El contador de visitas no envuelve y el agotamiento
de identidades marca integridad incompleta. Desactivar el observador no consume
identidades. Un receptor acotado a 16 hechos descarta el excedente sin afectar las
67 visitas de instrumento ni su resultado, comprobado por el test del adaptador.
Las referencias prestadas deben copiarse antes del retorno, según el contrato
general.

### Conexión a sesión — QA-050c2

Las ocho llamadas reales de resolve_event_sig aportan un uso explícito. El
resolver conserva una sola consulta exacta y, si falla, un solo recorrido del
índice. El resultado del callback no participa en la selección. qa_match_complete
acumula problemas de emisión/procedencia local y no sustituye la integridad de
los hechos antecedentes ni la confirmación de recepción de Runtime.

Cada consulta añade source_kind, source_index, event_start, event_end y
evaluated_frame. Las consultas live enlazan la fila de detector activo realmente
consumida, conservada por índice en la vista de 64 filas. El router de voz reutiliza
el índice de la referencia ya encontrada; no vuelve a buscar la firma.

Las consultas sobre audio_events enlazan el lote cerrado de análisis y su índice
de fila. No enlazan una escritura de chip inferida ni una fila con igual firma
en otro lote. Conservar lote e índice evita mantener un historial de identidades
proporcional a la toma. El helper interno solo recibe referencias del vector de
eventos de sesión, como verifican sus llamadas; su índice se obtiene sin búsqueda.
Las filas fuera de la cantidad realmente devuelta no reciben procedencia ficticia.
Los extremos start/end mantienen la convención del detector; evaluated_frame
conserva el cuadro que evalúan mute/cobertura/replay/export, distinto del cuadro
de sesión en el encabezado del hecho. Campos sin aplicación quedan not_applicable.

La procedencia cerrada se invalida al iniciar otro análisis o vaciar eventos.
La vista live se renueva con cada consulta real y se vacía junto con live_active
en reset/cambio de modo. Una inspección pública no reemplaza la vista de selección.
No se agregan consultas al detector ni búsquedas de asignaciones para la traza.

El test con core/ROM sintéticos recorre ocho cuadros, análisis y replay, con
asignaciones vacías intencionales para aislar resolución de reproducción. Comprueba
84 consultas, 2 candidatos exactos y 44 por instrumento; 82 sondeos exactos
negativos, 24 consultas de replay y 28 de interés de secuencia. Verifica causas,
índice y firma frente a las salidas reales del detector. Compara estado del juego
con observación on/off y receptor que descarta después de 64 callbacks.

La prueba inicial cubre live_assignment, live_sequence_interest y replay_trigger;
los ocho sitios conservan su contexto por revisión estática. QA-050e amplía la
cobertura dinámica a router, prioridad frente a SF2, frames bare y exportación.
QA-050c queda integrada; QA-050d conecta candidaturas de anclaje de secuencia.

## Recorrido de la tabla de anclaje — QA-050d1

seq_anchor_table_observed usa la misma ordenación estable y los mismos accesos
a firmas/cuadros que seq_anchor_table. Antes de ejecutar un cuadro llama a
before_frame con SeqAnchorFrameView: cuadro, firmas, sustituciones, estado de
anclaje de entrada e índices de los eventos originales. Los índices proceden
del tramo real del vector ya ordenado; no se buscan por firma. Después, las
visitas de candidatura salen de seq_anchor_frame_observed en el mismo recorrido.

Ambos callbacks son void noexcept, síncronos y prestados; no pueden reentrar,
modificar entradas o estado, hacer E/S, bloquear ni retener vistas. Los spans
referencian los vectores ya usados por el algoritmo. La API ordinaria conserva
su firma y delega con callbacks vacíos sin imponer una nueva copia o movimiento
a los lectores de firmas/cuadros. La observación no añade reservas, ordenaciones,
consultas al detector ni accesos a dichos lectores.

La prueba compara el conteo exacto de lecturas, anclajes y entradas observadas:
para eventos sin ordenar y firmas repetidas, los índices son 1,2,0,3 y los cuadros
0,10. Verifica el estado anterior de ambos cuadros y el resultado esperado 0,10.
Con cero eventos no hay callbacks y se conservan las claves vacías. Un receptor
que no guarda vistas recibe 128 cuadros/visitas sin cambiar el resultado. También
se comprueba la compatibilidad del lector de la API ordinaria que permite copia
y rechaza movimiento.

Este paso expone el recorrido. QA-050d2 crea el adaptador y QA-050d3 conecta la
caché y ambos caminos live. No demuestra por sí solo la trazabilidad completa
de secuencias ni la equivalencia de audio T12.

## Hechos de anclaje y vistas consumidas — QA-050d2

SequenceObservation recibe un contexto live_pack, live_authored o replay_table.
En begin copia a hechos la vista prestada del cuadro, sin conservar sus spans.
El encabezado usa el cuadro de sesión; anchor_frame conserva el cuadro que el
algoritmo evalúa, aunque se esté construyendo una tabla completa de replay.

- sequence_query identifica el cálculo y registra cantidades de entradas,
  sustituciones y estados, fuente disponible y si el algoritmo reiniciará el
  estado porque su tamaño no coincide con la vista de sustituciones.
- sequence_input conserva firma, índice de entrada, fuente e índice original.
  Replay obtiene los índices del recorrido estable real. Live puede aportar
  detector_signature, resolved_assignment o sequence_rule_trigger. Una firma
  transformada exige el hecho de transformación para declarar procedencia
  completa; su ausencia es unknown. En una firma sin transformar esa relación
  es not_applicable. No se busca una firma igual para adivinar su origen.
- sequence_substitution copia clave, disparador, duración, span, enabled, looping
  y tamaños de miembros/cabeza. sequence_signature registra cada elemento con
  su índice y lista, incluidos duplicados.
- sequence_state_input copia cada estado aportado: next_free, win_start, win_end
  y open. Si el algoritmo lo descartará por discrepancia de tamaños, will_be_used
  es falso; se conservan los datos recibidos sin atribuirlos al estado efectivo.
- sequence_candidate_visit conserva etapa e índices de entrada y sustitución.
  Enlaza consulta, entrada, sustitución y estado realmente aplicable. trigger y
  head_hits son not_applicable en ramas que todavía no los calcularon. Una visita
  formed no constituye selección ni anclaje; las decisiones quedan para QA-051.

El índice de entrada sale del bucle real de seq_anchor_frame_observed; así dos
firmas iguales mantienen identidades distintas. Un callback de otro cuadro no
adquiere enlaces a las filas del cuadro anterior. Fuente ausente, transformación
faltante, tamaños incompatibles de procedencia o enlaces no disponibles marcan
la integridad local incompleta sin cambiar la política de anclaje.

El adaptador ocupa como máximo 256 bytes y no reserva memoria. Conserva tres
grupos de identidades emitidas, comprobando una por una que productor y secuencia
son contiguos antes de resolver un enlace por índice. No cruza huecos, cambio de
productor ni desbordamiento. Esta propiedad depende del contrato de un escritor
por productor y callbacks sin reentrada. No se retiene un historial proporcional
a la toma y no se limita el recorrido a la capacidad del receptor.

El test verifica vínculos exactos, duplicados, etapas tempranas, ausencia de
procedencia y callbacks fuera de cuadro. Con 1025 entradas se emiten 2053 hechos
y el anclaje conserva su resultado aunque el receptor solo guarde 64. Pruebas
de los grupos cubren UINT64_MAX, huecos, productor distinto e identidad ausente.
Observación desactivada no consume identidades. complete() no acredita recepción,
persistencia ni la integridad de los antecedentes. QA-050d3 conecta las rutas
reales y los hechos de transformación que requiere live.

## Conexión de anclajes de sesión — QA-050d3

Las dos rutas live llaman a seq_anchor_frame_observed dentro del recorrido que
ya decide los anclajes. La ruta live_pack registra la firma directa del detector
y, cuando la resolución por asignación incorpora otra firma, emite exactamente
en esa rama sequence_signature_transform. La ruta live_authored hace lo mismo
cuando una regla de secuencia incorpora su disparador. El hecho conserva firma
original y destino, tipo de transformación y los antecedentes disponibles; las
propiedades que no aplican se declaran not_applicable en vez de usar ceros como
si fueran valores observados.

La tabla de replay llama a seq_anchor_table_observed únicamente cuando la caché
ya debía regenerarse por cambio de cantidad de eventos o generación de reglas.
Consultar otra vez la misma clave reutiliza el resultado y no genera hechos de
tabla. La observación no invalida la caché, no repite búsquedas ni crea un segundo
recorrido para obtener procedencia.

La sesión reserva una vez, solo cuando existe observador, espacio para 4160
orígenes: 4096 disparadores de secuencia y 64 voces live. El tipo está limitado
al compilar a 512 KiB. Reutiliza el bloque por cuadro y no limita el vector de
firmas que consume el algoritmo. Si la reserva falla o la capacidad se supera,
el audio continúa y sequence_query declara origin_storage=allocation_failed o
limit_exceeded; la integridad local queda incompleta. La pérdida del receptor no
cambia el recorrido ni el estado de anclaje.

La prueba de sesión recorre ocho cuadros con un core y una ROM sintéticos. Observa
14 consultas live_pack, 16 live_authored, 8 cuadros de tabla, 26 visitas y 2
transformaciones. Contrasta cada entrada con la fila activa o el lote cerrado
real y exige que las firmas transformadas enlacen su hecho de transformación.
Una segunda consulta de la misma tabla conserva los anclajes y no incrementa sus
ocho consultas. Las ejecuciones con observación desactivada, activa y receptor
acotado producen los mismos anclajes y estado serializado del juego.

Las pruebas unitarias cubren el límite completo del almacenamiento y su exceso,
reinicio, fallo de reserva representado por el contexto y observación desactivada.
La validación seleccionada no acredita todavía cobertura dinámica de todos los
usos de resolución, T12 completo, campaña Golden Axe, PCM equivalente, puente de
Runtime, persistencia ni aceptación de producto. Es un build local con VPX
desactivado; no constituye release ni adopción por Runtime.

## Integración de candidaturas y fidelidad local — QA-050e

Una prueba de sesión recorre dinámicamente los ocho usos de resolución sobre el
mismo core y ROM sintéticos: live_assignment, live_sequence_interest,
live_voice_route, replay_trigger, replay_voice_route, asset_coverage,
bare_frame_mute y export_audio. Habilita el dispositivo SDL dummy para ejercer
el router, usa replay normal y con salto para cubrir voces y frames bare, y crea
un mixdown HD para recorrer la exportación. Una asignación de instrumento con
SoundFont ausente fuerza la consulta de prioridad asset/SF2 y su degradación;
no acredita una síntesis SoundFont exitosa.

La prueba fija respectivamente 32, 28, 2, 30, 10, 18, 42 y 10 consultas: 172
en total. También exige 172 sondeos exactos, de los cuales 2 encuentran candidato,
y 112 visitas del índice de instrumentos. En secuencias conserva 14 consultas
live_pack, 16 live_authored, 8 cuadros de tabla, 26 visitas y 2 transformaciones.
Los conteos exactos fallan si la observación agrega un recorrido o callback que
la política no ejecutó.

Se ejecuta la misma sesión con observación apagada, recepción completa y receptor
que descarta callbacks después de 64 entregas. Las tres producen el mismo estado
serializado, anclajes y bytes del WAV exportado. La comparación conjunta evita
usar el parecido del audio como único oráculo; las pruebas unitarias de reglas
conservan además prioridades, multiplicidad, descarte y desempate.

QA-050 queda completada para observación de candidaturas. Esta comprobación local
no acredita T12 completo, una toma real, campaña
Golden Axe, rendimiento B13, puente Runtime, persistencia ni aceptación. Es un
build local con VPX desactivado y no constituye release.

## Selección y descarte — QA-051; RF-5, RF-9 y RF-14

La resolución de asignaciones emite una decisión por cada candidatura visitada
desde las ramas del índice: pitch_rejected, winner_updated o lower_priority. El
hecho assignment_candidate_decision enlaza la consulta y la candidatura que
acaba de evaluar y conserva firma, mejor firma provisional y rango. El resultado
assignment_selection se emite una vez por consulta con selected o el motivo
terminal efectivo: invalid_instrument, empty_index, unknown_instrument o
no_applicable_candidate. La selección exacta se identifica como exact_lookup y
no inventa un recorrido amplio.

El anclaje de secuencias conserva quorum_rejected,
claimed_by_open_sequence y selected dentro del mismo recorrido que muta el
estado. sequence_candidate_decision identifica cuadro, entrada, sustitución,
firma y sustitución relacionada cuando existe. sequence_selection cierra cada
cuadro con no_candidate, no_selection o selected y la cantidad real de anclajes.
Los hechos comparten la consulta y la identidad de sustitución con las visitas;
no vuelven a buscar candidatos ni recalculan la prioridad.

Los callbacks son síncronos, noexcept y prestados. Los métodos ordinarios usan
receptores vacíos sobre el mismo algoritmo. Los adaptadores permanecen dentro de
256 bytes y los fallos de emisión solo marcan la traza incompleta. La prueba de
sesión verifica 112 decisiones de candidaturas y 172 cierres de asignación, con
58 ausencias de coincidencia. Para secuencias verifica 3 decisiones y 38 cierres.
Estado serializado, anclajes y WAV coinciden con observación apagada, activa y
con un receptor que descarta después de 64 callbacks.

## Inicio y mantenimiento — QA-052; RF-5, RF-6 y RF-9

Una reproducción HD directa conserva una identidad de ocurrencia desde su
primer flanco. La traza enlaza assignment_selection con
hd_playback_request, hd_playback_decision y hd_playback_effect. Los tres hechos
de reproducción comparten la ocurrencia y cada uno causa el siguiente. La
decisión start declara rising_edge y su efecto distingue started, start_failed
o deferred según la rama que realmente ejecutó la sesión.

Mientras la misma firma permanece activa, la instancia conserva esa ocurrencia
y emite maintain con active_without_rising_edge, seguido de retained. En esta
rama played es no aplicable porque no se intenta un nuevo inicio físico. La
prueba de sesión fuerza un inicio y trece mantenimientos y exige el encadenado
causal completo. La prueba del adaptador cubre además la ausencia de selección o
identidad y marca la observación incompleta sin cambiar la reproducción.

La comparación con observación apagada, activa y con un receptor que deja de
aceptar callbacks conserva estado serializado, anclajes y WAV. Esta etapa cubre
inicio y mantenimiento de one-shots directos. Reinicio, sustitución, posición,
bucle, cierre, mezcla completa y las rutas de secuencia corresponden a tareas
posteriores.

## Reinicio y sustitución — QA-053; RF-5, RF-6, RF-8 y RF-9

El mezclador conserva la identidad QA de cada voz separada de su clave de
negocio. Al iniciar una voz consulta las voces reales de esa clave antes de
aplicar su política de fade. Si el PCM compartido coincide, comunica restart;
si difiere, comunica replace. La búsqueda inversa identifica la voz más nueva
incluso cuando una ocurrencia anterior aún desvanece.

La sesión convierte ese resultado efectivo en la decisión de reproducción. El
hecho conserva previous_occurrence y la occurrence nueva, distintas y no nulas,
y declara same_key_same_asset o same_key_new_asset. La petición sigue causada
por la selección y el efecto queda causado por la decisión. Un inicio sin voz
previa mantiene previous_occurrence como no aplicable.

El contrato unitario recorre start, restart y replace dentro del mezclador. La
integración de sesión crea dos nuevos flancos sin cortar las voces físicas:
verifica un reinicio, una sustitución, nueve mantenimientos y doce cadenas
completas. Estado serializado, anclajes y WAV coinciden con observación apagada,
activa y con receptor acotado.

## Avance de posición — QA-054; RF-6 y RF-14

Cada voz directa que tiene identidad QA conserva también la petición de
reproducción que la originó. El mezclador publica `hd_voice_position_span` por
cada tramo lineal mezclado. El hecho identifica ocurrencia y clave, enlaza la
petición causal y expresa los intervalos semiabiertos de salida y fuente, el
límite de fuente y la frecuencia de 44100 muestras por segundo.

Un tramo agrupa todas las muestras contiguas que avanzan uno a uno. El límite
de un bloque de mezcla cierra el tramo; el siguiente bloque continúa desde la
posición alcanzada. Así se puede reconstruir posición inicial, avance y límite
sin emitir un callback por muestra. Un cruce de bucle cierra además el tramo
antes de cambiar la posición de fuente; el significado completo de ese cruce
corresponde a QA-055.

La posición de salida pertenece al reloj de muestras del mezclador. El cuadro
queda desconocido con motivo `mixer_sample_timeline_only`: esta etapa no inventa
una conversión cuadro–muestra. Las voces sin identidad QA siguen mezclándose y
no producen un tramo que pudiera atribuirse falsamente. La prueba unitaria
mezcla un asset de 16 muestras en dos bloques y obtiene exactamente dos tramos,
`[2,10) → [0,8)` y `[10,18) → [8,16)`, ambos con límite 16 y la misma causa.
La integración de sesión observa diez tramos y conserva la equivalencia de
estado, anclajes y WAV entre sus tres modos de observación.

## Cruce de región de bucle — QA-055; RF-6 y RF-8

Cuando una voz en bucle alcanza el extremo de su ciclo, el mezclador cierra el
tramo lineal anterior antes de cambiar su cursor y emite
`hd_voice_loop_crossing`. El hecho conserva la posición exacta en el reloj de
salida, las posiciones de fuente previa y posterior, los extremos de la región,
el límite completo del asset, la ocurrencia y la petición causal.

Este hecho describe el salto observado y no contiene un veredicto. La prueba
entra en una región `[2,6)` desde la posición 5 y mezcla tres muestras: conserva
los tramos `[0,1) → [5,6)` y `[1,3) → [2,4)`, con un único cruce en la posición
de salida 1 desde 6 hacia 2. El límite del asset permanece 12 y se distingue del
extremo 6 de la región.

## Fin natural y fin de ventana — QA-056; RF-8 y RF-9

La retirada de una voz con identidad QA emite `hd_voice_end` desde la rama que
la provoca. `natural_end` procede del agotamiento real del asset y conserva la
posición de salida posterior a su última muestra. `window_end` procede de superar
el cuadro final de una voz en bucle sin cola. `window_cut` queda separado para
el límite duro. El motivo no se deduce por cercanía entre cursores o relojes.

Cada final conserva posición y límite de fuente. Los finales decididos en el
mezclador conocen la posición de salida y declaran el cuadro desconocido; los
decididos al avanzar la ventana conocen el cuadro y declaran la posición de
salida desconocida. La prueba hace terminar naturalmente un asset iniciado en
la muestra 2: aunque su retirada se procesa en el bloque posterior, conserva la
posición final real 6. Otra voz queda en la fuente 7 de 8 y termina por ventana
en el cuadro 2; su cercanía al límite no cambia el motivo `window_end`.

## Cola y desvanecimiento — QA-057; RF-7 y RF-8

`hd_voice_lifetime_effect` registra las fases `begin`, `advance` y `end` de una
cola de ventana o un desvanecimiento autorado. La cola declara `window_tail`; el
fade declara `authored_fade`. Inicio y fin decididos por la ventana conservan su
cuadro; el avance conserva intervalo de salida y fuente. En un fade se conservan
además las muestras restantes antes y después del avance. Estos campos son no
aplicables para una cola, no ceros inferidos.

El final de voz conserva `tail_complete`, `tail_cut` o `fade_complete` según la
rama efectiva. La prueba recorre una cola desde la fuente 5 hasta 8 y un fade de
dos muestras. En ambos exige exactamente un inicio, un avance y un fin, además
del motivo terminal correspondiente y la petición causal original.

## Orden de decisiones compartidas — QA-058; RF-5, RF-9 y RF-14

Cuando un inicio reutiliza una clave que todavía tiene una voz, el mezclador
devuelve la ocurrencia previa y la identidad causal de su petición inicial. La
decisión `restart` o `replace` enlaza entonces dos antecedentes explícitos: la
petición nueva y aquella petición de la voz que determinó la rama compartida.
Los campos `previous_occurrence` y `previous_request` exponen la misma relación
para consulta.

Los mantenimientos intermedios no reemplazan el antecedente que creó físicamente
la voz. La integración fuerza inicio, nueve mantenimientos, reinicio y
sustitución. Para las dos decisiones compartidas exige el orden de las
peticiones, las ocurrencias anterior/nueva y ambos vínculos; las demás decisiones
conservan un único antecedente.

## Revisión de neutralidad — QA-059; RF-5, RF-14 y RF-15

La revisión de las rutas QA-045 a QA-058 confirma que los adaptadores reciben
resultados ya calculados o acompañan el mismo recorrido real. No ordenan
candidatos, no eligen desempates, no vuelven a consultar reglas y no cambian
mute, selección, inicio, bucle ni política de finalización. Los callbacks del
mezclador son opcionales y sus estados adicionales describen decisiones ya
tomadas. La sesión solo los conecta cuando existe observador.

La integración ejecuta el mismo core, ROM y entradas con observación apagada,
recepción completa y receptor que descarta después de 64 callbacks. Los tres
modos producen estado serializado, anclajes y WAV idénticos. Los conteos exactos
de consultas, candidaturas, selecciones y usos protegen iteración y reglas; las
pruebas unitarias de matching y anclaje conservan prioridades y desempates.

## Participantes del mezclador — QA-060; RF-6, RF-7 y RF-9

Cada tramo lineal que una voz HD aporta al bloque real del mezclador emite
`hd_mix_participant`. El hecho conserva la identidad de ocurrencia y la clave de
negocio por separado, enlaza la petición que creó físicamente la voz y declara
el rango semiabierto en `engine_main_mix` junto con las posiciones inicial y
final en `hd_asset_pcm`. Ambos relojes expresan sample frames a 44100 Hz y el
límite completo de la pista permanece disponible.

La emisión utiliza el mismo tramo que el mezclador acaba de recorrer; no vuelve
a mezclar, consultar ni estimar posiciones. Una voz que empieza dentro del
bloque conserva solo `[inicio, fin)` y dos voces del mismo PCM siguen siendo dos
participantes por sus ocurrencias. Los bucles dividen el aporte en intervalos
lineales para no ocultar el retroceso de pista. Ganancia, mute y audibilidad se
añaden en QA-062; su ausencia aquí no convierte actividad en contribución
audible.

La prueba controlada mezcla dos ocurrencias del mismo PCM: una aporta
`[0,10) → [4,14)` y otra, iniciada más tarde, `[3,10) → [8,15)`. Verifica sus
identidades, claves, causas, relojes, tasas y límites. La integración de sesión
exige una fila de participante por cada tramo de posición emitido y conserva la
equivalencia de estado, anclajes y WAV de QA-059.

## Audio original presente, suprimido o ausente — QA-061; RF-7 y RF-10

`original_audio_span` observa los lotes nativos que realmente entran al bloque
común antes de aplicar el mute y antes de sumar las voces HD. Cada hecho conserva
su intervalo exacto en `engine_main_mix`, hash de fuente cuando existe, tasa de
44100 sample frames por segundo, estado y motivo. `present/unmuted` significa que
la ruta original permanece en el bloque; no afirma que sus muestras sean
audibles. `suppressed` distingue el mute por hash de la supresión explícita del
intervalo.

Cuando el router sustituye por completo el PCM nativo, el intervalo real se
registra como `absent/no_native_batch` y el hash queda no aplicable. Si el router
se suma sobre el chip, se preservan los rangos nativos capturados antes de que la
lista operativa de mute se sustituya por el lote reservado del router. Así no se
confunde una contribución auxiliar con presencia de audio original.

La captura usa almacenamiento fijo para 256 lotes nativos por bloque. El lote
257 no se omite en silencio: los tramos conservados se marcan incompletos y se
emite un intervalo `unknown/capacity_exceeded` que localiza el bloque afectado.
Este límite no recorta ni modifica el PCM que procesa `AudioPlayer`.

La prueba integrada recorre presencia, mute por hash y ausencia con router, más
los límites 256/257. La sesión conecta el mismo observador y exige intervalos
completos durante su ejecución sintética. Las pruebas de mute, router, mezcla
unificada, captura común y salida continúan produciendo los mismos resultados.

## Ganancia y mute de participantes HD — QA-062; RF-6 y RF-7

Cada `hd_mix_participant` conserva la ganancia efectiva de la primera y de la
última muestra de su tramo, expresada como `linear_gain`. También indica si toda
la ganancia del tramo fue cero y si alguna muestra de la voz produjo una
contribución PCM no nula antes de sumarse y saturarse con las demás voces. Esta
última propiedad describe el aporte digital observado, no un juicio perceptual.

El mezclador obtiene estos campos durante el mismo bucle que calcula cada
muestra. No recorre el PCM por segunda vez y reutiliza exactamente el entero que
se suma a la salida. Una voz con ganancia cero sigue avanzando su cursor y
permanece activa, pero registra `muted_by_gain=true` y
`nonzero_contribution=false`.

La prueba mezcla dos voces concurrentes del mismo PCM: una con ganancia uno y
otra con ganancia cero. Ambas avanzan por sus rangos esperados y siguen activas;
la salida contiene una sola contribución de valor 100, sin duplicación por la
voz muda. La integración valida presencia y coherencia de los campos en la
sesión. Los cambios de ganancia dentro de un cuadro y su correspondencia exacta
con límites de muestra se completan en QA-063.

## Correspondencia de cuadros y muestras — QA-063; RF-6, RF-9 y RF-10

`audio_frame_sample_boundary` registra el punto exacto de
`engine_main_mix` en el que comienza cada cuadro ejecutado. Conserva el número
de cuadro, la posición absoluta, el desplazamiento dentro del bloque staged y
la tasa de 44100 sample frames por segundo. No convierte estas posiciones a
segundos ni usa el reloj de pared.

Cada lote de audio original conserva el cuadro y el límite vigentes cuando el
callback del core entregó sus muestras. Al vaciar un bloque que acumuló varios
cuadros, `original_audio_span` recupera el hecho de límite correspondiente y lo
incluye como causa explícita. La relación utiliza cuadro y posición absoluta;
no depende del orden de llegada entre productores ni del último cuadro del
bloque.

La prueba integrada produce dos cambios dentro del cuadro 77. El audio original
está presente en `[0,2)` y suprimido por hash en `[2,4)`. Ambos tramos se
relacionan con el límite del cuadro en la muestra 0, pero sus efectos conservan
límites enteros distintos en las muestras 0 y 2. La integración de sesión exige
cuadro conocido, causa de límite válida y rangos semiabiertos sin redondeo.

## Bloques reales de salida principal — QA-064; RF-6 y RF-10

El canal PCM del contrato publica cada búfer recibido por el callback postmix
del dispositivo lógico de la sesión. `PcmView` identifica el punto
`sdl_logical_device_postmix`, la línea `engine_main_output`, su rango consecutivo,
la tasa, el formato F32LE y los dos canales. Los bytes son la vista prestada del
búfer real durante el callback síncrono; el consumidor debe copiarlos antes de
retornar, como exige el contrato público.

`AudioPlayer` no vuelve a mezclar ni convierte el bloque para observarlo. Cuenta
los sample frames aceptados por el callback y rechaza como observación completa
un formato, longitud o tamaño fuera de contrato. La sesión conecta este canal
solo cuando el consumidor solicita PCM y lo separa de los productores de hechos.

El ensayo copia cada vista a almacenamiento acotado, verifica rangos contiguos y
busca exactamente una vez una señal F32 conocida suministrada al stream auxiliar.
La coincidencia es exacta, incluidos todos los bytes. La prueba de sesión exige
que el canal público reciba bloques reales. Los vínculos de contribuciones con
estos bloques corresponden a las tareas siguientes.

## Conexión con el receptor de captura — QA-065; RF-7, RF-10 y RF-14

La sesión instala el callback postmix solo si el observador público declara un
receptor PCM. Cada bloque real pasa directamente de `AudioPlayer` al adaptador
`MainOutputObservation` y de allí al receptor prestado. El ensayo usa un destino
fijo y copia durante el callback; no ejecuta un mezclador auxiliar, no consulta
streams para recomponer audio y no escribe en disco desde el hilo de audio.

La identidad `main_output` tiene una secuencia propia, independiente de los
hechos emitidos por el hilo de sesión. La prueba integrada exige al menos un
bloque, rangos consecutivos, metadatos válidos y bytes coherentes. El cierre
desinstala el callback antes de liberar el dispositivo y el contexto.

Este receptor acotado demuestra la frontera de conexión. Las colas generales,
su capacidad, saturación y registro de pérdidas corresponden a QA-070–QA-072;
QA-065 no presenta el búfer del ensayo como esa implementación posterior.

## Contribución auxiliar en la captura común — QA-066; RF-6, RF-7, RF-10 y RF-14

Cada entrega aceptada por el stream synth obtiene una identidad y un rango en
`synth_input`. Los callbacks públicos put/get de SDL delimitan, bajo el bloqueo
del propio stream, qué parte de esa entrega consume la siguiente solicitud del
dispositivo. El postmix desplaza esos tramos a `engine_main_output` y emite
`auxiliary_output_span` con dos causas: la entrega auxiliar y el bloque PCM que
contiene el intervalo.

La transferencia de metadatos usa un anillo SPSC fijo de 256 entregas y hasta 32
tramos por bloque. El hilo productor no espera al consumidor ni copia PCM. Un
exceso marca incompleta la correspondencia, sin modificar la llamada a SDL ni
el audio. Los silencios de cebado avanzan internamente en el mismo orden y su
hecho de salida se incorpora en QA-067.

El caso validado usa entrada y dispositivo F32 estéreo a 44100 Hz, por lo que el
paso es identidad. Una señal de 32 sample frames conserva `[0,32)` en entrada y
el intervalo exacto donde sus bytes aparecen una sola vez en el postmix. El
hecho de salida enlaza la identidad del bloque PCM que el receptor copió. Otra
tasa o disposición se marca incompleta hasta propagar la conversión en QA-069;
no se extrapola el paso unitario.

## Silencio insertado en la salida — QA-067; RF-6 y RF-10

`prime_synth` conserva una entrega auxiliar con `inserted_silence=true`, por lo
que el silencio introducido deliberadamente no se confunde con ausencia de
captura ni con un bloque descartado. Cuando SDL consume esa entrega, el tramo
`auxiliary_output_span` conserva el mismo indicador, sus límites en
`synth_input` y `engine_main_output`, y dos causas: la entrega que explica el
silencio y el bloque PCM real que lo contiene.

La asignación de identidades de hechos admite productores concurrentes sin
esperar, para que la entrega desde la sesión y el tramo emitido desde el hilo de
audio no compitan por una secuencia. La identidad no expresa orden entre esos
hilos; las causas explícitas mantienen la relación de procedencia.

La prueba de integración entrega una señal de 32 muestras, inserta 16 muestras
de silencio y entrega una segunda señal distinta. Conserva las tres entregas y
sus tres tramos. El silencio ocupa `[32,48)` en la línea de entrada, tiene un
tramo de salida de 16 muestras y la copia del postmix contiene ceros en ese
intervalo identificado. Un hueco de captura o una pérdida de metadatos no se
rellena con ceros y queda incompleto en QA-068.

## Descarte y fallo de entrega auxiliar — QA-068; RF-6, RF-10 y RF-11

Si una entrega synth no llega a la salida, el adaptador emite el hecho
`auxiliary_delivery_loss`. Conserva el intervalo semiabierto afectado en
`synth_input` y uno de dos motivos: `delivery_failed` cuando SDL rechaza la
entrega, o `discarded` cuando un corte elimina audio que seguía pendiente. El
hecho no lleva bytes PCM, no tiene causas inventadas y no se transforma en un
tramo `auxiliary_output_span` ni en silencio insertado.

Al descartar, el cursor de consumo avanza hasta el final de ese intervalo. Un
segundo corte sin una nueva entrega no vuelve a publicar la misma pérdida. El
corte de transporte usa esta misma operación, de modo que no queda una ruta
que borre synth pendiente sin registrarlo. Estas reglas no modifican la llamada
a SDL ni generan audio; solamente describen una pérdida ya producida.

## Mapeo auxiliar mediante conversión — QA-069; RF-6 y RF-10

`auxiliary_output_span` conserva por separado las tasas de `synth_input` y
`engine_main_output`, el paso y la fase inicial Q32 y el soporte izquierdo y
derecho del filtro. Sus límites de entrada describen las muestras cuyo soporte
participa en el rango de salida; por ello los rangos pueden tener longitudes
distintas y dos entregas vecinas pueden influir en las mismas muestras de
transición.

El seguimiento avanza la fase únicamente por los frames de synth disponibles
que SDL entrega al bloque postmix. Un stream sin datos no adelanta el reloj de
entrada. Los descartes reinician la fase junto con `SDL_ClearAudioStream`. El
modelo de soporte está fijado a SDL 3.4.8, la versión validada por el ensayo de
viabilidad; otra versión marca incompleta la correspondencia en vez de aplicar
silenciosamente límites no verificados.

La integración a 48000 Hz reproduce las fronteras medidas: para una entrega de
silencio `[1024,1536)` a 44100 Hz, el soporte de salida es `[1109,1678)`; la
entrega siguiente empieza a influir en `1666`. El caso 44100→44100 conserva el
paso unitario y los rangos sin soporte adicional.

## Cola acotada de hechos — QA-070; RF-5, RF-10 y RF-14

`BoundedFactQueue<Capacity>` ofrece un traspaso SPSC de capacidad fijada al
compilar. El productor copia el hecho y todas sus vistas anidadas a una ranura
preasignada antes de publicarla. Ese recorrido no reserva memoria, no espera al
consumidor y no realiza serialización ni E/S. El consumidor recibe una vista
válida únicamente durante su callback síncrono y libera la ranura al volver.

`try_push` distingue aceptación, cola llena y hecho que excede los límites del
contrato. Una cola llena no sobrescribe hechos pendientes ni altera su orden;
la pérdida queda disponible para que el productor la contabilice fuera del
anillo en QA-072. La cola no incluye bloques PCM, cuya capacidad y propiedad se
incorporan en QA-071, ni escribe evidencia durable, responsabilidad de las
etapas consumidoras posteriores.

La prueba llena exactamente dos ranuras, comprueba el rechazo inmediato del
tercer hecho, modifica el almacenamiento original y verifica que la copia
conserva textos, causas, estados y campos. Tras consumir una ranura, el tercer
hecho entra y sale después de los dos anteriores. Un texto superior al límite
se rechaza sin ocupar capacidad.

## Cola acotada de PCM — QA-071; RF-10 y RF-14

`BoundedPcmQueue<Capacity>` separa el transporte de bloques PCM del transporte
de hechos. Cada ranura posee hasta 256 KiB preasignados y copia bytes, formato,
canales, punto de captura, timeline, rango y causas antes de publicarse. La ruta
del productor usa el mismo contrato SPSC sin espera: no reserva, no serializa,
no escribe y no llama al consumidor.

Una saturación devuelve `full` y conserva intactos los bloques pendientes. Un
payload o conjunto de textos fuera de los límites devuelve `invalid` sin
consumir la ranura. La vista del consumidor caduca al volver de su callback.
Los contadores de pérdida permanecen deliberadamente fuera de este anillo y se
incorporan en QA-072.

La prueba llena dos ranuras con formatos distintos, muta la memoria original y
comprueba bytes, metadatos, identidad y orden 1–2–3 tras reutilizar una ranura.
También verifica el rechazo de un bloque de 256 KiB + 1 byte sin impedir que el
siguiente bloque válido se publique.

## Desbordamiento fuera de las colas — QA-072; RF-5, RF-10 y RF-14

`ObservationOverflowCounter` reside fuera de las ranuras de hechos y PCM. Cada
cola recibe opcionalmente su dirección y, al detectar `full`, incrementa el
contador y conserva el primer elemento afectado antes de devolver. No intenta
encolar un diagnóstico en el mismo almacenamiento saturado.

El primer rechazo de hechos conserva `FactId`. El primero de PCM conserva
timeline, tasa y rango semiabierto; el timeline posee almacenamiento fijo
propio y declara si pudo copiarse completo. El productor publica ese estado y
el contador mediante release; el supervisor puede obtener una instantánea con
acquire sin bloquear el recorrido de audio. El valor se satura en el máximo de
64 bits en vez de volver a cero.

La prueba usa colas de capacidad uno y provoca dos rechazos consecutivos en
cada tipo. Aunque la ranura sigue ocupada, la instantánea conserva total dos y
el primer hecho o tramo exacto. QA-073 conectará el consumidor trabajador; el
cierre y sus contadores finales corresponden a QA-074.

## Trabajador consumidor — QA-073; RF-5, RF-10 y RF-15

`ObservationConsumerWorker` ejecuta un callback de drenaje propiedad del host
en un hilo separado. El host lo arranca y lo une fuera de los callbacks de
tiempo real. El trabajador solo decide cuándo invocar el drenaje: Runtime podrá
serializar o escribir dentro de ese callback sin introducir esas dependencias
en Engine ni en los productores.

Cuando no hay elementos, el trabajador cede durante un intervalo breve. La
ruta productora no lo notifica, no toma sus locks y no lo espera: solo copia a
las colas preasignadas o devuelve `full`. Solicitar parada y unir el hilo son
operaciones de control y nunca deben ejecutarse desde el recorrido de audio.
El protocolo de drenaje final y sus contadores se completa en QA-074.

El ensayo bloquea deliberadamente el callback consumidor después de adquirir
el primer hecho. El mismo hilo productor todavía inserta el segundo y recibe
`full` para el tercero; luego libera al trabajador, que consume ambos en un
hilo distinto. Si la inserción llamara o esperara al consumidor, el ensayo no
podría alcanzar la operación que lo libera.

## Cierre y números finales — QA-074; RF-5, RF-10 y RF-14

Las colas registran la última identidad emitida antes de intentar copiar y la
última identidad cuyo callback consumidor terminó. `close()` publica el límite
del productor y desde entonces `try_push` devuelve `closed`; el consumidor aún
puede drenar las ranuras pendientes. `close_snapshot()` combina esos números
con el contador externo de desbordamiento.

Una instantánea es completa únicamente cuando el productor está cerrado, las
últimas identidades emitida y consumida coinciden y no hubo desbordamientos.
Esto distingue una cola todavía pendiente de una pérdida final y evita que el
cierre invente recepción. Una cola sin contador externo aún detecta una última
identidad no consumida, pero los consumidores de producto deben conectar el
contador para conservar pérdidas intermedias.

La prueba acredita cierre limpio de hechos y PCM, drenaje posterior al cierre y
rechazo de emisiones tardías. El control satura una cola con la secuencia 10,
rechaza la 11 y después drena la 10: la instantánea final conserva 11 frente a
10, un desbordamiento y resultado incompleto.

## Instantánea inicial de audio — QA-075; RF-3 y RF-14

`AytherSession::audio_initial_snapshot()` devuelve una copia propietaria del
estado HD observable en el límite actual de la sesión. Enumera las voces del
mezclador con identidad, clave, posición, límite, ventana, loop, ganancia y
fade; copia las ventanas de secuencia y de sustitución live; y describe PCM
pendiente en staging, batches, stream principal, synth y entrega auxiliar.

La lectura no avanza el core, no mezcla muestras, no consume streams ni expone
los vectores internos. Las consultas de colas SDL son de solo lectura. Un error
de consulta marca `complete=false` sin presentar la cola como vacía. La memoria
de la instantánea pertenece al llamador y permanece independiente de cambios
posteriores en la sesión.

La prueba construye una voz activa con posición intermedia y un bloque nativo
staged sobre el dispositivo dummy. Dos lecturas sucesivas conservan los mismos
valores mientras `voice_count` y `pending_frames` no cambian. También verifica
que una copia de ventanas sobrevive al vaciado de la colección original.

## Inicialización HD nueva — QA-076; RF-3, RF-14 y RF-15

`AytherSession::prepare_fresh_hd_audio()` establece la frontera usada cuando la
toma no aporta un estado HD completo y compatible. No reinicia ni avanza el
juego emulado y conserva el catálogo, las asignaciones y los observadores ya
instalados. El detector live vuelve a su estado inicial con la región temporal
efectiva; se eliminan ventanas, flancos, instancias, disparos y aprendizaje de
la ejecución anterior.

`AudioPlayer::prepare_fresh_session()` pausa temporalmente el dispositivo si
estaba activo, corta voces y previews, vacía staging y streams y restablece las
líneas de muestra, colas auxiliares, mutes, ganancias y telemetría. Un fallo de
SDL devuelve `false`, de modo que Runtime no tiene que presentar como vacía una
frontera que no pudo acreditar. Los assets decodificados permanecen en caché:
son material cargado, no una reproducción heredada.

La instantánea posterior declara `initialization=fresh`. La prueba comienza con
una voz, PCM nativo y synth pendientes, mutes y detector live activo; después
comprueba colecciones vacías, valores ordinarios, mismo cuadro e idempotencia.
La operación no interpreta ni restaura estados aportados; esa validación
corresponde a QA-077.

## Validación de estado HD aportado — QA-077; RF-3 y RF-14

`audio_hd_state.hpp` define el encabezado de estado HD 1.1. La revisión 1.1
añade la categoría/bus de cada voz para restaurar pausas sin recalcular rutas;
los estados 1.0 se rechazan sin alterar el estado activo. Su identidad opaca
debe nombrar el estado exacto del juego restaurado, no solo el título o la
ejecución. También conserva el cuadro de emulación y declara explícitamente la
presencia de detector, ventanas, voces, solicitudes y audio pendiente; una
sección puede estar vacía, pero no puede estar ausente de un estado completo.

`validate_audio_hd_state()` exige versión exacta, identidades no vacías y de
hasta 256 bytes, identidad y cuadro coincidentes, todas las secciones
obligatorias y ninguna sección desconocida. El resultado distingue cada clase
de incompatibilidad y conserva las máscaras de secciones faltantes o
desconocidas. Por ello un payload parcial no puede presentarse como restaurado.

El ensayo acepta un encabezado completo y rechaza de forma determinista versión
1.1, identidad ausente o distinta, cuadro distinto, una sección faltante, una
sección desconocida e identidad excesiva. No modifica la sesión ni aplica el
payload; detector y ventanas se restauran en QA-078.

## Restauración de detector y ventanas — QA-078; RF-3 y RF-14

`AudioHdDetectorWindowsState` conserva el estado opaco completo del detector,
las firmas activas del cuadro anterior, la máscara resultante, las ventanas de
secuencia, los próximos anclajes y el aprendizaje firma–instrumento. El payload
binario del detector tiene versión propia y conserva firmas de 64 bits sin
conversiones numéricas; su tamaño máximo es 1 MiB. Cada colección de la capa
de sesión admite hasta 4096 entradas.

`AytherSession::restore_audio_hd_detector_windows()` valida primero el
encabezado, el cuadro interno del detector, límites, máscaras, rangos y
duplicados. Construye detector y colecciones temporales y solo los intercambia
cuando todo el estado es válido. Un rechazo conserva intacto el estado anterior.
La restauración reemplaza el aprendizaje y los flancos: no los combina con una
ejecución previa, no procesa entradas y no avanza el cuadro emulado.

`audio_hd_detector_windows_state()` obtiene una copia propietaria para guardar
o verificar ese límite. La prueba restaura primero contaminación controlada y
después el estado esperado, comprueba detector activo, dos clases de ventana,
anclaje, aprendizaje y máscara exactos, mismo cuadro y origen `restored`.
También corrompe el payload y verifica rechazo atómico.

## Restauración de una voz HD — QA-079; RF-3, RF-6 y RF-14

`AudioHdVoicesState` conserva assets PCM mix-ready y voces que los referencian
mediante una identidad local al estado. Cada voz incluye ocurrencia y causa,
clave de negocio, cursor, posición de salida, ganancia, tipo, límites de
ventana, fade, tail, región de loop y retraso. El PCM compartido se guarda una
sola vez. El estado admite hasta 256 voces y 64 MiB de PCM en total.

`AytherSession::restore_audio_hd_voices()` valida primero el encabezado exacto
de QA-077. El mezclador construye assets y voces temporales, comprueba
identidades, tamaños, posiciones, ganancias, loops, fades y contadores, y solo
entonces sustituye las voces activas. No llama al mezclador, no entrega PCM y
no invoca observadores. Un rechazo conserva las voces anteriores.

El ensayo restaura una voz activa con PCM conocido y posición intermedia. La
instantánea y la copia completa recuperan todos sus valores, el contador de
muestras mezcladas permanece en el valor aportado, no aparece audio pendiente
y el cuadro no cambia. Un cursor situado exactamente después del asset se
rechaza y una nueva lectura confirma que la voz válida sigue intacta.

## Voces simultáneas con clave reutilizada — QA-080; RF-3, RF-6 y RF-14

La restauración identifica cada reproducción por `occurrence`, no por su clave
de negocio ni por el PCM. Por ello dos voces pueden compartir clave y asset y
mantener cursores, posiciones de salida, causas y ganancias independientes. El
asset permanece una sola vez en el estado propietario.

El control restaura dos ocurrencias con la misma clave y el mismo
`pcm_identity`, en posiciones 3 y 19 y límites de salida distintos. La
instantánea conserva dos voces y el estado reexportado contiene un asset y dos
referencias. Duplicar la ocurrencia se rechaza antes de modificar el mezclador.

## Solicitudes y audio pendiente compatible — QA-081; RF-3, RF-10 y RF-14

`AudioHdRequestsPendingState` conserva las instancias lógicas HD, los marcadores
de disparo de secuencia y evento y el audio principal que todavía permanece en
staging. El estado pendiente incluye PCM S16 estéreo, lotes con hash y rango,
mutes, procedencia del audio original, cursores de salida y los contadores
auxiliares necesarios para que la siguiente entrada continúe desde el mismo
límite. Las colecciones admiten hasta 4096 entradas, la procedencia conserva el
límite fijo de 256 lotes del observador y el PCM pendiente admite hasta 16 MiB.

`restore_audio_hd_requests_pending()` valida el encabezado exacto, rutas,
rangos, máscaras, ganancias, claves y lotes contiguos antes de modificar la
sesión. A continuación elimina el backlog heredado de los streams SDL y
reemplaza staging, relojes y solicitudes; no los combina con la ejecución que
ocupaba antes la sesión. Las ocurrencias aportadas reservan su rango para que
las reproducciones posteriores reciban identidades nuevas.

SDL no ofrece una lectura de sus colas sin consumirlas. Por ello una captura
con bytes ya entregados a un stream, una conversión auxiliar con fase distinta
de cero, una entrega auxiliar en curso o un stream HD heredado se marca
incompleta y no se acepta como estado compatible. Esta regla evita presentar
solo contadores como si fueran el audio real. La prueba restaura primero datos
ajenos y después el estado aportado, añade la primera entrada PCM y acredita
que el staging contiene únicamente el estado aportado y esa entrada. La
instantánea de sesión coincide en solicitudes, disparos y rangos pendientes;
un lote inválido se rechaza sin sustituir el estado válido.

## Límite de producción al terminar — QA-082; RF-3, RF-8 y RF-10

`AytherSession::freeze_audio_production()` fija una frontera inmutable después
del último cuadro solicitado. `AudioProductionLimit` conserva ese cuadro, el
primer sample frame que ya no pertenece a la producción principal, los frames
principales aún en staging, el cursor de salida entregada al congelar y el
límite de entrada auxiliar. El límite principal suma el timeline ya enviado y
el staging ya generado; no depende de cuánto haya drenado el dispositivo.

Después de congelar, `step()` no avanza el core. AudioPlayer rechaza nuevas
entradas nativas o auxiliares, silencios de cebado, voces y previews, y tampoco
permite restaurar otro estado pendiente. Las llamadas repetidas devuelven la
misma frontera aunque indiquen otro cuadro. El audio ya producido puede seguir
entregándose en la fase de drenaje posterior; esa entrega no amplía el límite.

La prueba fija `[0,1204)` a partir de 1200 frames enviados y cuatro en staging,
con cuadro final 44. Intenta todas las entradas productoras y confirma que el
estado pendiente no cambia. En sesión ejecuta un cuadro, congela y prueba que
otro `step()` devuelve el mismo cuadro y la misma frontera, sin generar muestras.

## Finalización de voces por fin de prueba — QA-083; RF-8, RF-10 y RF-14

`AytherSession::finalize_audio_hd_voices_for_test()` solo acepta el cierre tras
congelar la producción. Retira todas las voces HD todavía activas y emite un
`hd_voice_end` por ocurrencia con motivo `test_end`, su cursor de fuente y su
límite. El hecho usa el límite principal congelado como posición de salida; si
esa frontera quedó incompleta por desbordamiento, conserva el cuadro final y no
afirma una posición de salida exacta.

El cierre no mezcla PCM, no avanza cursores, no completa un bucle, fade o tail
y no fabrica un `natural_end`. Una llamada previa a la congelación se rechaza
sin retirar voces. Tras un cierre aceptado, repetirlo conserva la frontera y
devuelve cero voces finalizadas sin duplicar hechos.

La prueba restaura dos ocurrencias que comparten clave y PCM pero mantienen
posiciones 3 y 7. Verifica el rechazo previo, congela, recibe exactamente dos
hechos `test_end` con ambas posiciones y confirma que no quedan voces. Las
pruebas existentes de agotamiento real siguen exigiendo `natural_end`, por lo
que ambos motivos permanecen diferenciados.

## Drenaje del audio ya producido — QA-084; RF-10 y RF-14

`AytherSession::drain_frozen_audio()` entrega únicamente el bloque principal
que ya formaba parte de `main_sample_limit`. Exige producción congelada y cero
voces activas, de modo que el cierre de QA-083 debe ocurrir antes. El flush no
ceba silencio por stall, no mezcla voces, no ejecuta el core y no acepta nueva
entrada. El resultado conserva límite, frames entregados y frames restantes.

Si SDL rechaza la entrega, el staging permanece pendiente y `complete` es
falso. Una entrega completa avanza el timeline exactamente hasta el límite,
vacía el staging y puede repetirse sin duplicar muestras. El audio que SDL ya
poseía sigue drenando por su flujo ordinario; esta operación no sintetiza ni
extiende esa cola.

La prueba restaura 256 frames conocidos y una voz todavía activa. Comprueba
que el drenaje se rechaza hasta finalizar la voz, entrega el bloque completo y
encuentra sus muestras exactas en el callback postmix. Una segunda llamada no
entrega frames. La prueba de sesión conserva el mismo frame antes y después del
drenaje y de un `step()` posterior a la congelación.

## Límites y errores del catálogo

El adaptador no reserva memoria en callbacks. Conserva hasta 4096 firmas y
4096 enlaces de entradas aceptadas en almacenamiento fijo; su tamaño total
no supera 512 KiB, comprobado al compilar. Más firmas
distintas siguen produciendo declaraciones, pero su condición de duplicado
puede quedar desconocida y el inventario se marca incompleto. Se mantienen
las identidades de las firmas previamente observadas. Más de 4096 aceptaciones
siguen cargándose normalmente; los enlaces de carga posteriores quedan
desconocidos y la observación se marca incompleta. Se prueban ambos lados del
límite. Los textos de más de
4096 bytes conservan tamaño y razón text_limit, sin truncarse como si estuvieran
completos. Estos límites no recortan el catálogo procesado por Engine.

Una reserva fallida del adaptador conserva el diagnóstico y usa el parser
ordinario. La ausencia del pack o del archivo, archivo vacío, fallo de lectura,
TOML inválido y miembro event ausente/no array se distinguen de event=[].
Los rechazos individuales no impiden observar el resto. El estado de integridad
del catálogo describe lo que pudo emitir el productor; no confirma recepción
ni persistencia. Tampoco acredita la integridad de una ejecución completa.

Las pruebas usan un catálogo sintético, un pack firmado sintético, el core de
prueba del repositorio y una ROM sintética. Se comparan salidas con observación
activa/inactiva y la recarga conserva la secuencia del productor. No constituyen
la campaña Golden Axe ni la equivalencia completa T12.

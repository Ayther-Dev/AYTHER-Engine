# Consumo nativo de Tessera desde Engine

La integración opcional consume un paquete **instalado** `Tessera::core` mediante
su ABI C 1.0. `TesseraBackend` es privado; los encabezados públicos nativos de Engine no
incluyen Tessera, libretro ni detalles de su implementación. La dirección de
la dependencia es Engine → Tessera. El núcleo se sigue compilando por separado.

## Configuración y selección

Configurar Engine con `AYTHER_ENABLE_TESSERA=ON` y `Tessera_DIR` apuntando a
`<prefijo>/lib/cmake/Tessera`. Se requiere Windows x64, ABI MSVC, herramientas
14.51 o posteriores y CRT `/MD`. Engine y Tessera deben coincidir en la opción ASAN. Un paquete de diagnóstico requiere el mismo
compilador y versión; sus pruebas copian el runtime ASAN local junto al binario.
La opción desactivada mantiene el proveedor existente y stubs nativos que
responden `unsupported_capability`. No hay descargas implícitas de Tessera.

```cpp
ayther::AytherSession::Config configuration;
configuration.provider = ayther::EmulationProvider::tessera_native;
configuration.enable_audio = false;
configuration.derive_core_pack = false;
configuration.native_config = native_configuration;
ayther::engine::NativeResult preparation;
auto session = ayther::AytherSession::create(configuration, preparation);
if (!session) { /* Preserve preparation.code/phase/frame_id/transaction. */ }
```

`native_configuration` identifica el backend y contiene bytes de contenido,
configuración canónica y memoria persistente inicial. La configuración incluye
el build ID enumerado por `NativeEmulation::backends`; la detección por bytes y
su evidencia se consultan con `NativeEmulation::detect`. La sesión verifica la
selección y expone sistema, capacidades e identidades mediante `info()`.
Una selección inválida, contenido/configuración incompatibles o preparación
fallida devuelven error; no cambian al proveedor libretro.

El modo nativo se conduce exclusivamente mediante `native_emulation()`, con
estados completos de controles y contexto de época/frame. `step()` y
`set_input()` de la fachada legacy rechazan esa ruta: el resultado es
`unsupported_capability` en `FrameView::native_result`, sin avance. Los clientes
nativos deben conservar cada `NativeResult`: las consultas siguientes tienen
su propio resultado. El overload de creación permite conservar también el
contexto de un error sin sesión. `reset()` de la fachada delega al driver.

## Originales, capturas y límites de este incremento

Después de un avance nativo, `native_frame()` refresca los préstamos originales
sin copiar píxeles ni PCM. `pixel_encoding=rgba8` y `fb_format=-1` distinguen RGBA8
de los formatos libretro. `native_original` incluye PCM S16LE, frecuencia,
canales, muestras, ticks e identidad reales. FX-01 produce 16×16 RGBA8 y 800
pares estéreo a 48 kHz; no se envían al reproductor legacy de 44,1 kHz.
El respaldo original queda explícito como `pack_not_integrated` y no genera
sustituciones, paletas VDP ni eventos `ayther_interface_v1` ficticios.

Los préstamos vencen al siguiente avance, reset, restore o cierre exitosos.
Las operaciones fallidas conservan los originales; cuando no hay originales
actuales la fachada devuelve una vista vacía con el resultado correspondiente.
`NativeSnapshot` posee una referencia independiente y puede sobrevivir al cierre
o destrucción de la sesión. Retener otra referencia antes de transferirla a otro
lector; cada objeto envoltorio tiene un propietario. La memoria se libera por
la API del módulo que la asignó. Las colas y presupuestos no se amplían.

`CONTROL_PENDING`, `END_OF_TRACE`, confirmación y cancelación mantienen códigos,
frame/época, transacción y causa; no dependen de que haya espacio en la cola de
eventos. EOF pausa sin avanzar ni aplicar una pendiente. Los fallos de reset
conservan la sesión y las capturas históricas. Checkpoints aún responden
`unsupported_capability`; su integración se verifica en T024.

Este incremento acredita FX-01 desde la sesión real de Engine. La ROM Mega Drive,
FX-02 de audio activo, FX-03 de sustitución gráfica, composición con pack real y
presentación/audio de Runtime corresponden a las fases siguientes. Por ello
las opciones nativas de pack, parche, core options, observadores legacy, ROM por
ruta y dispositivo de audio se rechazan explícitamente en esta fase.

## Validación local y costo

Las pruebas se registran en el CTest existente. El checkout de Tessera se usa
sólo para headers de pruebas fijados por su lock y fixtures de la CLI, mediante
`AYTHER_TESSERA_TEST_SOURCE_DIR`; el producto enlaza el paquete instalado.
`tessera_session_test` crea `AytherSession` y usa su driver público; el oráculo
cerrado no llama a una segunda sesión C por detrás. La prueba de replay compara
12 procesos nuevos de 3.000 frames con la CLI autónoma y fórmulas independientes.

Los costes registrados separan avance, consultas/encoding/hash de originales,
observación y oráculo funcional, excluyendo preparación y escritura JSON.
Son una línea base sintética de T018, no la aceptación de rendimiento T032.
La integración CI reutiliza una sola entrada Windows existente y un SHA fijo de
Tessera. No se crea una matriz ni un PR por fase. La ejecución remota queda
pendiente del push coordinado; no se da por aprobada a partir del YAML.

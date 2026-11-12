# Consumo nativo de Tessera desde Engine

La integración opcional consume un paquete **instalado** `Tessera::core` mediante
su API C 1.1. `TesseraBackend` es privado; los encabezados públicos nativos de Engine no
incluyen Tessera, libretro ni detalles de su implementación. La dirección de
la dependencia es Engine → Tessera. El núcleo se sigue compilando por separado.

El sistema soportado es la **Mega Drive** (backend `megadrive` de Tessera). Con
él, una ROM corre por la misma tubería de frames que con un core libretro y da
las mismas identidades que gpgx: es la puerta G2 de la spec 008 (equivalencia de
observación, `v0.2.0-rc.8`). Los backends sintéticos (`synthetic.*`) siguen
existiendo para las pruebas del contrato.

## Configuración y selección

Configurar Engine con `AYTHER_ENABLE_TESSERA=ON` y `Tessera_DIR` apuntando a
`<prefijo>/lib/cmake/Tessera`. Se requiere Windows x64, ABI MSVC, herramientas
14.51 o posteriores y CRT `/MD`. Engine y Tessera deben coincidir en la opción ASAN. Un paquete de diagnóstico requiere el mismo
compilador y versión; sus pruebas copian el runtime ASAN local junto al binario.
La opción desactivada mantiene el proveedor existente y stubs nativos que
responden `unsupported_capability`. No hay descargas implícitas de Tessera.

### Versión exigida y lock

Engine exige un paquete que declare la **API de Tessera 1.1 o una 1.x posterior**
(`Tessera_ABI_MAJOR`/`Tessera_ABI_MINOR` de `TesseraConfig.cmake`); un paquete
1.0 se rechaza al configurar con `TESSERA_API_TOO_OLD`. La versión del paquete
(`0.x`) no se restringe, porque los tags de Tessera avanzan por separado de la
API. Además, `dependencies/tessera.lock.json` fija el Tessera exacto:

- `sourceCommit`, `api` y `package.version` siempre;
- `release.tag` y `artifact.sha256` cuando Tessera publica un tag con esa API.
  Mientras no exista, el lock es `unreleased` y fija un commit de `main`.
  Hoy fija `v0.2.0-rc.8` (API 1.1, núcleo `megadrive`, región de work RAM
  `0x1000` y la temporización de TP-209.2), el candidato con el que se aprobó
  G2. `v0.2.0-rc.1` falló en CI antes de empaquetar y no tiene release.

`native-release.yml` lee el commit del lock para compilar Tessera; `ci.yml` lo
fija como literal (`TESSERA_COMMIT`, para que el `ref` del checkout no derive de
un archivo del repositorio) y un paso falla si difiere del lock, de modo que el
lock y el workflow cambian juntos. La configuración compara el paquete instalado con el
lock (`source.revision`, `version`, API); si no coincide falla con
`TESSERA_LOCK_MISMATCH`. Para desarrollar contra otro Tessera, local y nunca en
CI: `-DAYTHER_TESSERA_ALLOW_UNLOCKED=ON`. Las pruebas `ayther.package.tessera_lock.*`
comprueban el lock y que el verificador rechaza un commit, versión, tag o API
distintos. Para subir de versión se cambia el lock en el mismo PR que el código
que lo necesita:

```powershell
cmake -DAYTHER_TESSERA_LOCK=dependencies/tessera.lock.json `
      -DAYTHER_TESSERA_PREFIX=<prefijo Tessera> -P tools/check_tessera_lock.cmake
```

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

### Sesión Mega Drive

Para la Mega Drive, la configuración lleva el sistema 1
(`TESSERA_SYSTEM_MEGA_DRIVE`), audio a 48 kHz y un mapa de dos mandos de doce
controles (campo 6). `tools/parity_bench/native_config.h` y
`tests/support/tessera_native_config.hpp` la construyen a partir de una ROM; la
ROM se pasa como bytes (`content`), nunca por ruta.

El modo nativo se conduce de dos formas. `native_emulation()` es el driver
tipado, con estados completos de controles y contexto de época/frame. Desde
TP-301.3 (spec 008), `set_input()` y `step()` de la sesión también avanzan la
máquina nativa por la tubería de frames (`produce_frame`), igual que con
libretro. `set_input()` guarda el mando RetroPad de cada puerto. En cada
`step()`, la sesión asigna todos los controles que declara el mapa de la
configuración (campo 6), con el mapeo de gpgx: A=Y, B=B, C=A, más las
direcciones y start. X, Y, Z y mode quedan sueltos. Si el avance falla,
`step()` devuelve el código en `FrameView::native_result`.

En la tubería, el frame de la máquina `megadrive` baja como RGB565, el formato
que da gpgx, para que el hasher de tiles, el juez de sprites y la
recomposición raster vean los mismos píxeles (TP-209.1). La observación (VRAM,
CRAM, registros, VSRAM, sprites, escrituras de audio) sale de `TesseraBackend`
como `ObservationSource`. La work RAM sale de la región `0x1000` de Tessera con
el layout de `SYSTEM_RAM` de libretro (word-swapped): la sesión guarda un espejo
que se relee tras cada avance, restore, reset o close, y `poke()` y los cheats
del Engine (`add_cheat`) escriben con `write_control`, así que la escritura es
estado de la máquina (los checkpoints la llevan). Los códigos de cheat del core
(`cheat_set`) son de libretro y no existen en esta vía.

### Qué funciona por la vía nativa

- **Packs:** `Config::pack_path`, `set_pack()` y `reload_pack()` cargan el pack
  igual que con libretro; `validate_pack()` declara la plataforma `megadrive` y
  el CRC32 de la ROM. Un pack horneado con gpgx se reproduce con Tessera y al
  revés con el mismo multiconjunto de sustituciones por frame
  (`parity_bench --cross-pack`, TP-210; `ayther.integration.parity_bench_cross_pack`).
- **Identidades:** sprites, tiles, celdas de plano, atribución y eventos de
  sonido salen iguales que con gpgx (`parity_bench dump --identities`, TP-209;
  contrato en [IDENTITY_SPECIFICATION.md](IDENTITY_SPECIFICATION.md)).
- **Fondos y Panorámicas:** el bg stitcher (`bg_capture`, `bg_bounds`,
  `bg_cells`, `export_background_plane`) y el anclaje de una Panorámica
  (`define_panorama`) dan lo mismo que con gpgx.
- **Lua:** `load_script()`; `ayther.ram.*` lee el espejo de la work RAM y
  `ayther.tiles.list()`/`ayther.sprites.list()` las ocurrencias del frame.
- **Detector de audio en vivo:** `audio_live_active()` da las mismas voces y
  firmas FM/PSG que con gpgx, frame a frame.

Las cuatro últimas las cubre `ayther.integration.tessera_native_subsystems`
(TP-301.5) con una ROM sintética escrita en la prueba (`tools/common/synth_rom.h`):
Tessera siempre y, si está el core del fork de `third_party/cores/core.lock` (o
`AYTHER_PARITY_GPGX_CORE`), la misma serie con gpgx.

### Qué no funciona todavía

Estas opciones se rechazan al crear la sesión con `unsupported_capability`, sin
cambiar de proveedor: `enable_audio` (no hay salida al dispositivo; el PCM
nativo está en `native_original`), `patch_path`, `core_options`, `rom_path` y
los observadores `audio_observer` y `render_observer`. Tampoco hay rewind ni
keyframes de grabación, así que `replay_seek` y el análisis de tomas siguen
siendo de libretro: son la spec 008, TP-302. `serialize()` delega en el
`checkpoint()` del driver y `serialize_size()` devuelve 0; `unserialize()` se
rechaza, porque restaurar exige `restore_checkpoint()` con la traza de entradas
que lo acompaña.

Los clientes nativos deben conservar cada `NativeResult`: las consultas
siguientes tienen su propio resultado. El overload de creación permite
conservar también el contexto de un error sin sesión. `reset()` de la fachada
delega al driver.

## Originales y capturas del driver tipado

Después de un avance con el driver tipado, `native_frame()` refresca los
préstamos originales sin copiar píxeles ni PCM. `pixel_encoding=rgba8` y
`fb_format=-1` distinguen RGBA8 de los formatos libretro. `native_original`
incluye PCM S16LE, frecuencia, canales, muestras, ticks e identidad reales; no
se envía al reproductor legacy de 44,1 kHz. Con pack y la política de
presentación confirmada, el frame se compone con las sustituciones del pack;
si no, `native_original_reason` dice por qué se muestra el original
(`no_pack`, `policy_unconfirmed`, `capture_lost`, `temporal_incomplete`, …).

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
conservan la sesión y las capturas históricas.

## Validación local y costo

Las pruebas se registran en el CTest existente. El checkout de Tessera se usa
sólo para headers de pruebas fijados por su lock y fixtures de la CLI, mediante
`AYTHER_TESSERA_TEST_SOURCE_DIR`; el producto enlaza el paquete instalado.
`tessera_session_test` crea `AytherSession` y usa su driver público; el oráculo
cerrado no llama a una segunda sesión C por detrás. La prueba de replay compara
12 procesos nuevos de 3.000 frames con la CLI autónoma y fórmulas independientes.

Las pruebas de la Mega Drive son `ayther.contract.tessera_observation_source`
(observación), `ayther.integration.tessera_native_md` (FX-02 por la tubería de
frames), `tessera_controls_test`, `tessera_pack_test` y
`tessera_presentation_test` (FX-03 con pack), `tessera_native_subsystems`
(fondos, Panorámicas, Lua y detector) y `parity_bench_cross_pack` (packs
cruzados con gpgx). Las fixtures nativas (`AYTHER_TESSERA_TEST_FIXTURE_DIR`) se
generan en el checkout de Tessera; CTest no las descarga.

Los costes registrados separan avance, consultas/encoding/hash de originales,
observación y oráculo funcional, excluyendo preparación y escritura JSON.
Son una línea base sintética de T018, no la aceptación de rendimiento T032.
La integración CI reutiliza una sola entrada Windows existente y un SHA fijo de
Tessera. No se crea una matriz ni un PR por fase. La ejecución remota queda
pendiente del push coordinado; no se da por aprobada a partir del YAML.

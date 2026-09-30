# Engine public API contract

**Status:** implemented for the installed C++ Engine surface, including typed
input, session, renderer, layers, pack inspection, Libretro core probing, build
capabilities, and the Vulkan render-image handoff.

The ownership decision, authorized consumers, and `0.1.x` guarantees are
recorded in [ADR 0003](adr/0003-runtime-engine-public-api-ownership.md).

## Public boundary

The primary Runtime-facing C++ entry point lives under
`include/ayther/engine/` and is included with installed paths such as:

```cpp
#include <ayther/engine/engine.hpp>
#include <ayther/engine/audio_observer.hpp>
#include <ayther/engine/capabilities.hpp>
#include <ayther/engine/core_probe.hpp>
#include <ayther/engine/input.hpp>
#include <ayther/engine/vulkan_interop.hpp>
```

`engine.hpp` is the umbrella include. It directly publishes the typed Engine
modules and the installed C++ session, renderer, and layer facades; consumers
that want the complete C++ API need no additional AYTHER header.

The existing flat `include/ayther/` C++ facades remain installable and are
reachable through the umbrella during the 0.1.x line so current consumers
retain source compatibility. New focused APIs belong under `ayther/engine/`;
the flat C ABI remains available separately through `ayther_core_ffi.h`.

Public headers must be self-contained and compile as the first include in an
otherwise empty C++20 translation unit. They may include the C++ standard
library, documented third-party API headers, or other installed
`ayther/engine/` headers. They must not include paths containing `src/`,
`libretro_host/`, `vulkan_backend/`, `private/`, `internal/`, or `detail/`. A
future `detail/` directory requires an explicit contract, installation rule,
and compatibility coverage before use.

## Observación de audio QA (API 1.0)

`AytherSession::audio_initial_snapshot()` devuelve una copia propietaria del
límite HD actual. `prepare_fresh_hd_audio()` establece una sesión HD nueva sin
avanzar ni reiniciar el juego emulado; conserva asignaciones cargadas y devuelve
fallo cuando el backend no puede acreditar que eliminó el audio encolado de la
ejecución anterior.

`audio_hd_state.hpp` versiona por separado el estado HD aportado. Su validador
compara la identidad opaca del estado de juego, el cuadro y la presencia de las
cinco secciones obligatorias antes de que una restauración pueda mutar Engine.
`audio_hd_detector_windows_state()` copia detector, flancos, ventanas, anclajes
y aprendizaje. `restore_audio_hd_detector_windows()` los reemplaza de forma
transaccional después de validar el encabezado y el payload completo, sin
avanzar el juego. El detector usa un payload opaco binario con versión y límite
de 1 MiB; las colecciones públicas están acotadas a 4096 entradas.

`audio_hd_voices_state()` copia voces y PCM compartido sin consumirlo.
`restore_audio_hd_voices()` valida y reemplaza de forma atómica hasta 256 voces
y 64 MiB de PCM, sin mezclar ni encolar muestras. Las identidades PCM son
locales al estado; la identidad de ocurrencia y la clave de negocio permanecen
separadas. Dos voces pueden reutilizar clave y PCM: `occurrence` sigue siendo
única y sus cursores y parámetros no se combinan.

`audio_hd_requests_pending_state()` copia solicitudes lógicas, disparos y el
staging principal sin consumirlo. `restore_audio_hd_requests_pending()` valida
y reemplaza esos datos y vacía primero cualquier backlog SDL heredado. El PCM
pendiente está limitado a 16 MiB y sus lotes deben cubrirlo de forma contigua.
La procedencia de hasta 256 lotes originales se restaura junto con staging para
que el primer flush no declare ausente audio que sí estaba pendiente.
Una cola SDL no vacía, una entrega auxiliar en curso o una fase de conversión
auxiliar no nula hacen que la copia sea incompleta: la API no inventa bytes que
SDL no permite inspeccionar sin consumo. La instantánea inicial expone también
las solicitudes y marcadores restaurados.

`freeze_audio_production()` fija de forma idempotente el último cuadro y el
límite semiabierto de muestras principales ya producidas. Tras esa llamada la
sesión no ejecuta nuevos pasos y AudioPlayer no acepta nuevas entradas, voces,
silencios de cebado ni restauraciones. La estructura pública distingue muestras
producidas, staging pendiente y salida ya entregada para que el drenaje no se
confunda con síntesis adicional.

`finalize_audio_hd_voices_for_test()` se acepta únicamente después de esa
congelación. Emite un final `test_end` por cada voz activa con su posición de
fuente y la frontera congelada, y después retira las voces sin mezclar muestras
ni completar bucles o colas. El resultado informa aceptación, cantidad y la
posición usada; una repetición aceptada informa cero y no duplica finales. Si
el límite de muestras es incompleto, los hechos usan el cuadro final en vez de
atribuir una posición de salida exacta.

`drain_frozen_audio()` requiere esa frontera y que no queden voces activas.
Entrega el staging principal ya incluido en el límite sin ejecutar pasos,
mezclar voces, cebar silencio ni producir muestras. El resultado informa los
frames entregados y restantes. Una entrega SDL fallida conserva el bloque para
diagnóstico o reintento; una repetición posterior a un drenaje completo entrega
cero frames y mantiene el mismo límite.

`audio_observer.hpp` publica vistas de hechos, causas, orden de estado compartido,
ocurrencias y PCM, además de un binding no propietario. Solo depende de C++20;
no exige Runtime, SDL, disco, UI, RTTI ni un formato de transporte. La pareja 1.0
es independiente de la release y de la ABI del core; se rechazan 1.1 y 2.0 hasta
que exista soporte explícito. Declarar estos tipos no anuncia capacidades de
instrumentación que todavía no estén implementadas.

Todos los spans y textos, incluidos los anidados en variantes, pertenecen al
productor y vencen al retornar el callback. El consumidor copia lo necesario a
almacenamiento acotado. Es propietario del contexto y lo mantiene vivo hasta
desconectar y detener todos los productores. Productores distintos pueden llamar
concurrentemente; la recepción proporciona una entrega acotada y sin bloqueo.
Se prohíben reentrada o mutación de Engine, I/O, serialización, esperas y reservas
sin límite en los callbacks. Una pérdida de copia se registra separadamente y
no puede modificar una decisión sonora. El binding solo cambia con productores
detenidos. Un callback nulo desactiva ese flujo.

Los identificadores de hecho son locales a la ejecución: productor y secuencia
no nulos. Runtime añade la identidad de ejecución al copiar. Una ocurrencia es
distinta de la clave de negocio; los rangos PCM son semiabiertos y sus unidades
son sample frames completos, con tasa y timeline explícitos. Orden de llegada y
orden causal no son equivalentes. Los campos desconocidos/no aplicables llevan
motivo; no se sustituyen por valores inventados.

Los límites declarados son 256 causas y órdenes de estado, 128 campos por hecho,
4096 bytes por texto, 64 KiB por hecho codificado y 256 KiB de PCM por vista,
hasta 192 kHz y ocho canales. Los límites del framing pueden exigir dividir el
PCM en bloques menores para incluir metadatos. Son cotas del contrato; este
binding no valida datos ni acredita los presupuestos de ejecución. Los
productores y el bridge deben verificarlas y diagnosticar pérdidas.

QA-041 verifica consumo sin dependencias y añade la cabecera al inventario de
instalación y a las pruebas del repositorio. QA-042 verifica vida útil y
no interferencia del binding; la integración de productores y el paquete nuevo
se verifican en las tareas posteriores del plan de audio QA.

QA-045 conecta el inventario de catálogo y las ramas del parser mediante
Config.audio_observer. Su esquema, límites y distinción respecto de carga y
reproducción se describen en [observación de audio](AUDIO_OBSERVATION.md).

## Language, exceptions, and RTTI

- C++20 is the minimum language version.
- Functions marked `noexcept` do not allocate, do not throw, and terminate no
  process on an environmental failure.
- Expected failures in fallible APIs use a typed result and stable error code.
  Exceptions never cross the Runtime-to-Engine boundary.
- Public behavior must not depend on RTTI. Public interfaces do not require
  consumers to enable RTTI, and concrete implementation types are not exposed
  for `dynamic_cast` or `typeid`.

## Ownership and lifetime

- Value types are owned by the caller after return.
- Owning objects are represented by move-only RAII handles. Their destructors
  release Engine-owned resources; copying ownership is forbidden.
- Borrowed references, spans, string views, and native handles name their owner
  and validity interval in the declaring header. A borrowed value never
  extends the owner's lifetime.
- Vulkan handles remain owned by the party named by `vulkan_interop.hpp`.
  Exporting or borrowing a handle never transfers ownership implicitly.

## Thread safety and synchronization

- Immutable value operations and the version/capability queries are safe for
  concurrent calls.
- Mutable Engine objects are single-owner and single-threaded unless their
  declaring header explicitly says otherwise.
- A Vulkan interop contract must state the queue, command-buffer, semaphore,
  image-layout, and destruction responsibilities for every borrowed or
  transferred handle. Runtime may not infer synchronization from a raw handle.

## Errors

`version()` and `probe_capabilities()` are total, side-effect-free `noexcept`
queries. They report the linked artifact, not current device availability, so
there is no environmental failure path. APIs that inspect a loader, GPU,
display, audio device, filesystem, configuration source, or emulator core are
fallible and must return either an unavailable capability or a typed error.
Diagnostics and logging are not substitutes for a returned error.

## Version and capability identity

`ayther::engine::version()` reports the numeric release embedded in the linked
Engine artifact: `0.1.0` for this line. The release-candidate distribution keeps
the identifier `v0.1.0-rc.5` in its immutable tag, archive names, provenance,
and release metadata. The `rc.5` suffix is distribution identity rather than a
fourth field of `Version`, so logs and UI format the linked numeric value through
the API instead of embedding another version literal.

`ayther::engine::core_abi_revision()` forwards the linked Core's non-zero ABI
revision. It is independent from SemVer and must be incremented whenever an
incompatible Engine/Core function signature or shared data layout changes.
Tests compare the public query with `AYTHER_CORE_C_ABI_REVISION` and compare the
linked numeric version with the canonical release macros and CMake project
version.

## Compatibility and deprecation

The release candidate guarantees source compatibility within the 0.1.x line.
It does not guarantee binary compatibility across compiler versions, C++
standard libraries, build modes, compiler flags, or platform toolchains.
Consumers must rebuild against the headers and library shipped together.

A public symbol is deprecated before removal, with documentation naming the
replacement and the first version in which removal is permitted. During 0.1.x,
removal or a source-incompatible semantic change is deferred to 0.2.0. Security
or correctness defects that cannot be preserved safely are documented as an
explicit compatibility exception.

The maintained consumers during `0.1.x` are AYTHER Runtime, AYTHER Play when it
uses the same reviewed modules, and this repository's package/conformance
tests. Lab, SDK tools, and third parties may not expand the boundary without a
new Engine review. AYTHER Engine maintainers own the declarations,
implementation, installation rules, documentation, and contract tests.

## Artifact version and capabilities

`version()` returns the version compiled into the linked Engine library. It is
implemented out of line, so Runtime compile definitions cannot change it.

`probe_capabilities()` returns build capabilities only. It reads no registry,
singleton, environment variable, configuration file, filesystem path, or
device state. It does not load or initialize Vulkan, create a window, open an
audio device, or start a thread. Therefore it is callable before any Engine
instance and behaves identically on a machine without a GPU or display.

For the current native artifact:

| Field | Meaning |
|---|---|
| `renderer` | Renderer implementation compiled into Engine (`vulkan` or `none`) |
| `hardware_acceleration` | The compiled renderer uses hardware acceleration when initialized; it is not a device-presence check |
| `external_image_import` | A public external-image import contract is compiled and available |
| `libretro_video` | The Engine artifact implements the libretro video callback path |
| `libretro_audio` | The Engine artifact implements the libretro audio callback paths |

Vulkan loader/device suitability belongs to Engine creation or a future typed
environment probe. Keeping it separate is what makes capability probing
deterministic and safe in headless processes.

## Libretro core probing

`probe_core(path)` loads one user-selected native library without initializing
SDL, loading a ROM, or calling `retro_init`. Success returns a move-only
`CoreProbe`; its destructor unloads the library. `CoreProbe::info()` returns a
`CoreInfo` whose name, version, extension list, API version, and path/extraction
flags were copied while the library was loaded. No `retro_system_info` pointer
or platform handle is public.

The factory returns `Result<CoreProbe>`. `ErrorCode::Io` means the platform
loader rejected the file, while `ErrorCode::BadFormat` means the library loaded
but omitted `retro_api_version` or `retro_get_system_info`. The owned diagnostic
contains the platform error or missing symbol names. A failed factory call
releases any handle acquired before the failure.

`CoreInfo::serialize()` and `CoreProbe::serialize()` return the same compact
JSON object. Strings supplied by the core escape quotes, reverse solidi, and
all JSON control characters. Protocol framing such as `AYTHER_STATUS`, event
names, and process exit codes remains a Runtime responsibility.

Loading a native library executes code under platform-loader rules and is not
a sandbox or trust decision. `CoreProbe` is single-owner; destruction or moves
must not race with access to its information.

## Renderer ownership boundary

`<ayther/ayther_renderer.h>` is the canonical public renderer surface and every
declared method is implemented by `Ayther::engine`. `AytherRenderer` records
offscreen work into a caller-provided command buffer; it does not create or
destroy the Vulkan instance, physical device, logical device, surface, graphics
queue, or swapchain, and it never presents. Those objects remain owned by the
host application through the complete renderer lifetime.

`init()` accepts the borrowed `VulkanContextView`, canvas dimensions, and the
installed shader directory. The renderer owns its offscreen targets, texture
caches, pipelines, comparison image, and readback resources. `shutdown()`
provides deterministic release before the host destroys the Vulkan context. If
it is omitted, the destructor performs the same cleanup using the retained
borrowed context; consequently that context must still be alive during renderer
destruction.

## Vulkan render-image handoff

`RenderImageView` is a trivially copyable, non-owning snapshot returned by
`AytherRenderer::render_image()` and `compare_render_image()`. It publishes the
borrowed `VkImage`, `VkImageView`, and `VkSampler`, together with the
image format, two-dimensional extent, handoff layout, barrier source stage and
access masks, and the exclusive owning queue-family index. The image, its
memory, its view, and its sampler remain owned by Engine. Runtime never calls a
Vulkan destruction or memory-release function for those handles.

The main render target's handles remain valid until renderer resize, shutdown,
or destruction. Comparison views additionally end on comparison release or any
recapture; callers must query a new view even when the implementation reuses the
same handles and size. A copied `RenderImageView` does not extend that interval.
Before an invalidating operation, Runtime must complete all GPU access and
discard or rebind descriptors that reference the old view.

The producer hands both render targets over in the `layout` recorded in the
view; currently this is `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` with fragment
shader/read access as the ready scope. Runtime may transition to transfer source
or another compatible use, but before the next Engine access it must restore
the published layout and keep exclusive ownership in `queue_family_index`.
Queue-family transfer is outside this contract and requires an additional API
that coordinates the release and acquire operations in both directions.

No semaphore, fence, event, command buffer, or queue handle is transferred by
`RenderImageView`. When Engine production and Runtime consumption are recorded
in the same command buffer, command order and the documented image barriers are
the synchronization contract. Across submissions, Runtime supplies and owns the
signal/wait chain in both directions. It must wait for Engine production before
reading the image and make its own completion visible before Engine reuses,
resizes, releases, or destroys it.

The view's image, image view, and sampler are required for a valid handoff.
`is_valid()` checks handle and metadata presence, not GPU completion. Destroying
the C++ value performs no Vulkan work.

## Dependency containment

The capabilities header exposes only fixed-width standard types. The core-probe
header exposes standard filesystem, ownership, and string types plus the
installed `ayther::Result` contract. Libretro, SDL, platform loader headers,
threads, and logging remain implementation dependencies and do not propagate
through those modules.

`vulkan_interop.hpp` deliberately exposes Vulkan native types. Therefore the
installed `Ayther::engine` target carries `Vulkan::Vulkan` as a public usage
requirement and `AytherConfig.cmake` resolves Vulkan before importing the Engine
target. Other third-party dependencies remain private unless a future public
declaration truly requires the consumer to see them.

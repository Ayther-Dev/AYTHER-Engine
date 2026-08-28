#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# drop_samples.py — automatiza la parte MECANICA del import del paquete DAW
# (Mezclar -> Exportar -> Paquete DAW) en FL Studio: arrastrar el .mid y cada
# samples/NN-*.wav a su canal S<NN> del Channel Rack.
#
# POR QUE ASI (no hay API de FL Studio para esto)
# -----------------------------------------------------------------------------
# La unica superficie de scripting publica de FL Studio (MIDI Controller
# Scripting, Python sandboxeado: modulos channels/plugins/general/...) NO tiene
# funciones para importar archivos, cargar un sample en un canal, ni asignar un
# generador. Tampoco hay SDK de "plugin de automatizacion" (el SDK de FL es
# para plugins de audio, no para esto). Generar el .flp directamente tampoco es
# viable con garantias: la libreria pyflp (no oficial, formato reverseingenie-
# rado) solo EDITA canales/notas que YA EXISTEN en un .flp cargado — no hay
# forma documentada de crear canales nuevos ni de agregar notas.
#
# La unica via que queda es manejar la ventana REAL de FL con automatizacion de
# UI de Windows: el import del .mid y la colocacion de notas los hace FL de
# verdad (cero riesgo de formato); este script solo automatiza los arrastres
# repetitivos. El drag-and-drop se simula con SetCursorPos + SendInput (mismo
# mecanismo que usa cualquier automatizador de Windows) porque un drag de OLE
# real (el que usa Explorer para soltar un archivo) necesita eventos de mouse
# de bajo nivel, no mensajes de ventana (PostMessage/click sintetico no alcanza).
#
# El Channel Rack de FL es una superficie dibujada a mano (no son controles
# nativos de Windows), asi que no hay forma de "encontrar el canal S03" via
# accesibilidad — el script pide CALIBRAR 2 puntos (fila del primer canal S01 y
# fila del ultimo S<N>) y por linealidad calcula las filas intermedias. Misma
# idea para la ventana de Explorer con los samples (ordenados NN-<nombre>.wav,
# el orden alfabetico YA coincide con el orden S01..SNN porque el paquete los
# nombra con prefijo numerico de 2 digitos).
#
# LIMITACIONES A PROPOSITO (no intenta cubrir todo)
# -----------------------------------------------------------------------------
# - Asume el Channel Rack en vista de LISTA (filas parejas, una debajo de otra)
#   y el Explorer en vista Detalles/Lista ordenado por nombre ascendente — el
#   usuario los deja asi ANTES de correr el script (instrucciones abajo).
# - El dialogo de opciones de import del .mid (checkbox "Create one channel per
#   track", tempo) lo confirma el USUARIO a mano — su layout puede variar entre
#   versiones/temas de FL y equivocarse ahi es peor que pedir un click humano.
# - No reasigna instrumentos ni ajusta root note: eso es una decision del
#   usuario (parte 4/5 del flujo), no algo mecanico para automatizar.
#
# USO
# -----------------------------------------------------------------------------
#   python drop_samples.py <carpeta_del_paquete> [--dry-run] [--skip-import]
#                          [--start N] [--limit N] [--list]
#
# Antes de correrlo:
#   1. Abri un proyecto NUEVO/VACIO en FL Studio.
#   2. Abri el Explorador de Windows en la carpeta samples/ del paquete, vista
#      Detalles, ordenado por Nombre ascendente (Ctrl+Shift+6 o boton derecho
#      > Ver > Detalles, y click en la columna "Nombre").
#   3. Corre el script y segui las instrucciones en pantalla (selecciona
#      ventanas enfocandolas + Enter en esta terminal; calibra posiciones
#      quedandote quieto con el mouse sobre el punto pedido).
#
# Sin dependencias de terceros — solo stdlib (ctypes para las llamadas Win32).
# Requiere Windows y Python 3.9+.
# -----------------------------------------------------------------------------

import argparse
import ctypes
import re
import sys
import time
import subprocess
from ctypes import wintypes
from pathlib import Path

if sys.platform != "win32":
    sys.exit("Este script solo corre en Windows (usa la API Win32 via ctypes).")

user32 = ctypes.windll.user32

# -----------------------------------------------------------------------------
# mapping.toml — parser minimo (formato propio, escrito por daw_export.cpp).
# No es un parser TOML general: solo extrae los campos de [[sound]] que hacen
# falta acá, evitando una dependencia extra (tomllib exige Python 3.11+).
# -----------------------------------------------------------------------------

class Sound:
    __slots__ = ("index", "name", "sample", "pitches", "chip")

    def __init__(self, index, name, sample, pitches, chip):
        self.index = index
        self.name = name
        self.sample = sample
        self.pitches = pitches
        self.chip = chip


def parse_sounds(mapping_path: Path):
    text = mapping_path.read_text(encoding="utf-8")
    # ^[[sound]]$ anclado a inicio de línea — el comentario de cabecera del
    # propio mapping.toml menciona "[[sound]]" en prosa ("Cada [[sound]]
    # correlaciona..."), así que un split ingenuo por substring se confunde
    # con eso y cuenta una sección de más.
    blocks = re.split(r"(?m)^\[\[sound\]\]\s*$", text)[1:]
    sounds = []
    for block in blocks:
        # Corta en el siguiente header de sección para no arrastrar [[channel_track]].
        body = re.split(r"(?m)^\[\[", block)[0]
        fields = {}
        for m in re.finditer(r'^\s*(\w+)\s*=\s*(.+?)\s*$', body, re.MULTILINE):
            fields[m.group(1)] = m.group(2)

        def as_str(key, default=""):
            v = fields.get(key, default)
            return v.strip('"') if v else default

        def as_int(key, default=0):
            v = fields.get(key)
            try:
                return int(v)
            except (TypeError, ValueError):
                return default

        def as_int_list(key):
            v = fields.get(key, "[]")
            return [int(x) for x in re.findall(r"-?\d+", v)]

        sounds.append(Sound(
            index=as_int("index"),
            name=as_str("name"),
            sample=as_str("sample"),
            pitches=as_int_list("pitches"),
            chip=as_str("chip"),
        ))
    sounds.sort(key=lambda s: s.index)
    return sounds


# -----------------------------------------------------------------------------
# Win32: ventanas, cursor, drag sintetico
# -----------------------------------------------------------------------------

def get_cursor_pos():
    pt = wintypes.POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    return pt.x, pt.y


def get_foreground_window_title(hwnd):
    length = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def is_window(hwnd):
    return bool(user32.IsWindow(hwnd))


def select_window(prompt: str):
    """Pide al usuario que enfoque la ventana deseada (click/Alt+Tab) y
    presione Enter ACA — evita adivinar títulos/clases de ventana (el Channel
    Rack de FL no tiene título propio detectable, y el título de Explorer
    depende de la carpeta). El usuario apunta; el script solo lee."""
    print(f"\n>> {prompt}")
    input("   Enfocá esa ventana y volvé acá — presioná Enter cuando esté activa: ")
    hwnd = user32.GetForegroundWindow()
    title = get_foreground_window_title(hwnd) or "(sin título)"
    print(f"   Usando ventana: \"{title}\"")
    return hwnd


def capture_point(prompt: str, countdown=3):
    """Captura la posición del cursor SIN requerir un click (un click podría
    disparar algo dentro de FL/Explorer) — el usuario solo se queda quieto."""
    print(f"\n>> Posicioná el mouse sobre: {prompt}")
    for n in range(countdown, 0, -1):
        print(f"   capturando en {n}...", end="\r")
        time.sleep(1)
    x, y = get_cursor_pos()
    print(f"   capturado en ({x}, {y})           ")
    return x, y


INPUT_MOUSE = 0
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(wintypes.ULONG))]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("mi", MOUSEINPUT)]


def _send_mouse_flag(flag):
    inp = INPUT(type=INPUT_MOUSE, mi=MOUSEINPUT(0, 0, 0, flag, 0, None))
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def move_to(x, y):
    user32.SetCursorPos(int(x), int(y))


def drag(src, dst, speed="normal"):
    """Simula un drag-and-drop de OLE real: SetCursorPos para el movimiento +
    SendInput para el estado del botón (un click sintetizado por PostMessage
    NO dispara el drag source de Explorer — necesita el hook de mouse de bajo
    nivel, que es lo que da SendInput)."""
    step_delay = {"slow": 0.03, "normal": 0.015, "fast": 0.006}[speed]
    steps = {"slow": 30, "normal": 18, "fast": 10}[speed]

    move_to(*src)
    time.sleep(0.1)
    _send_mouse_flag(MOUSEEVENTF_LEFTDOWN)
    time.sleep(0.08)
    # Jiggle minimo: arma la detección de drag de Windows (umbral ~4px).
    move_to(src[0] + 6, src[1] + 6)
    time.sleep(0.05)
    for i in range(1, steps + 1):
        t = i / steps
        move_to(src[0] + (dst[0] - src[0]) * t, src[1] + (dst[1] - src[1]) * t)
        time.sleep(step_delay)
    time.sleep(0.15)   # deja que el target registre el hover antes de soltar
    _send_mouse_flag(MOUSEEVENTF_LEFTUP)
    time.sleep(0.35)   # deja asentar el drop antes del próximo drag


def lerp_row(p_first, p_last, index_first, index_last, index):
    if index_last == index_first:
        return p_first
    t = (index - index_first) / (index_last - index_first)
    return (p_first[0] + (p_last[0] - p_first[0]) * t,
            p_first[1] + (p_last[1] - p_first[1]) * t)


# -----------------------------------------------------------------------------
# Flujo principal
# -----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Automatiza el drag del paquete DAW (mid + samples) a FL Studio.")
    ap.add_argument("paquete", type=Path, help="Carpeta del paquete exportado (mapping.toml adentro)")
    ap.add_argument("--dry-run", action="store_true", help="Calcula el plan y lo imprime; no mueve el mouse")
    ap.add_argument("--skip-import", action="store_true",
                     help="Los canales S01..SNN ya existen en FL (te saltea el drag del .mid)")
    ap.add_argument("--start", type=int, default=1, help="Empezar desde el sonido N (para reanudar)")
    ap.add_argument("--limit", type=int, default=None, help="Soltar como máximo N sonidos (para probar)")
    ap.add_argument("--speed", choices=["slow", "normal", "fast"], default="normal")
    ap.add_argument("--list", action="store_true", help="Solo lista los sonidos del mapping.toml y sale")
    args = ap.parse_args()

    mapping_path = args.paquete / "mapping.toml"
    if not mapping_path.exists():
        sys.exit(f"No encontré {mapping_path}")
    sounds = parse_sounds(mapping_path)
    if not sounds:
        sys.exit("mapping.toml no tiene [[sound]] — ¿es la carpeta correcta del paquete?")

    print(f"{len(sounds)} sonido(s) en el mapping:")
    for s in sounds:
        pitch_note = f" · notas {s.pitches}" if s.pitches else ""
        print(f"  S{s.index:02d}  {s.name:<28} [{s.chip}]  {s.sample}{pitch_note}")
    if args.list:
        return

    mid_files = list(args.paquete.glob("*.mid"))

    if not args.skip_import:
        print("\n=== Fase 1: import del MIDI ===")
        if not mid_files:
            sys.exit("No encontré ningún .mid en la carpeta del paquete.")
        fl_hwnd = select_window("la ventana de FL Studio (proyecto nuevo/vacío abierto)")
        drop_pt = capture_point("un área VACÍA del Channel Rack o Playlist de FL (el punto donde soltar el .mid)")

        print(f"\nAbriendo el explorador en {args.paquete} para arrastrar {mid_files[0].name}...")
        # /select, va PEGADO a la ruta (sin espacio) — explorer.exe parsea su
        # línea de comando de forma especial, no como argv estándar. Por eso
        # se arma como UN string (no una lista) para no meter un espacio.
        subprocess.Popen(f'explorer /select,"{mid_files[0].resolve()}"')
        mid_src_pt = capture_point(f"el ícono de {mid_files[0].name} en el Explorador que se abrió")

        if args.dry_run:
            print(f"[dry-run] arrastraría {mid_files[0].name} {mid_src_pt} -> FL {drop_pt}")
        else:
            if not is_window(fl_hwnd):
                sys.exit("La ventana de FL ya no existe.")
            drag(mid_src_pt, drop_pt, args.speed)
            print("Listo — soltado. FL debería mostrar el diálogo de import.")

        print("\n>> Confirmá VOS el diálogo de FL Studio: tildá 'Create one channel per")
        print("   track', aceptá el tempo, y esperá a que aparezcan los canales S01.. en")
        print("   el Channel Rack.")
        input("   Cuando termines, presioná Enter acá para seguir: ")

    print("\n=== Fase 2: samples -> canales ===")
    print("Asegurate de que el Explorador esté en la carpeta samples/, vista Detalles,")
    print("ordenado por Nombre ascendente (así la fila N = sonido N).")

    fl_hwnd = select_window("la ventana de FL Studio con los canales S01..S%02d ya creados" % sounds[-1].index)
    fl_first = capture_point(f"la fila del canal S{sounds[0].index:02d} ({sounds[0].name}) en el Channel Rack")
    fl_last = capture_point(f"la fila del canal S{sounds[-1].index:02d} ({sounds[-1].name}) en el Channel Rack")

    # Ojo: si un sample falló al renderizar (mapping.toml trae sample = ""),
    # ese archivo NO existe en samples/ — el Explorador NO le reserva una fila,
    # así que la posición en la lista de archivos NO es sounds[i].index, es la
    # posición dentro de los que SÍ tienen archivo (renderable).
    renderable = [s for s in sounds if s.sample]
    missing = [s for s in sounds if not s.sample]
    if missing:
        print("\n(!) Sin sample renderizado — no se van a arrastrar:")
        for s in missing:
            print(f"    S{s.index:02d} {s.name}")
    if not renderable:
        sys.exit("Ningún sonido tiene sample renderizado — nada para arrastrar.")

    samples_dir = args.paquete / "samples"
    subprocess.Popen(["explorer", str(samples_dir.resolve())])
    time.sleep(1.0)
    expl_hwnd = select_window("el Explorador con la carpeta samples/ (vista Detalles, orden por Nombre)")
    expl_first = capture_point(
        f"la PRIMERA fila de la lista de archivos ({renderable[0].sample.split('/')[-1]})")
    expl_last = capture_point(
        f"la ÚLTIMA fila de la lista de archivos ({renderable[-1].sample.split('/')[-1]})")

    todo = [s for s in renderable if args.start <= s.index]
    if args.limit is not None:
        todo = todo[:args.limit]

    print(f"\n{len(todo)} drag(s) a realizar" + (" (dry-run: solo se imprime el plan)" if args.dry_run else "") + ".")
    for s in todo:
        expl_pos = renderable.index(s) + 1   # posición REAL en la lista de archivos (1-based)
        src = lerp_row(expl_first, expl_last, 1, len(renderable), expl_pos)
        dst = lerp_row(fl_first, fl_last, sounds[0].index, sounds[-1].index, s.index)
        if args.dry_run:
            print(f"  [dry-run] S{s.index:02d} {s.name}: Explorer{tuple(round(v) for v in src)} "
                  f"-> FL{tuple(round(v) for v in dst)}")
            continue
        if not is_window(fl_hwnd) or not is_window(expl_hwnd):
            sys.exit(f"Una de las ventanas se cerró — corté en S{s.index:02d}. "
                     f"Reanudá con --skip-import --start {s.index}")
        print(f"  S{s.index:02d} {s.name} ...", end="", flush=True)
        drag(src, dst, args.speed)
        print(" ok")

    if not args.dry_run:
        print("\nListo. Revisá:")
        print("  - Los tracks tonales: si la melodía suena corrida, ajustá la root note")
        print("    del Sampler con la nota de 'pitches' en mapping.toml (root note = C5")
        print("    por default en FL; poné ahí la más grave/común de la lista).")
        print("  - Borrá o muteá los canales C01..C10 si no los vas a usar de referencia.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrumpido por el usuario.")
        sys.exit(130)

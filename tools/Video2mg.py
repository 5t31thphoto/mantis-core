#!/usr/bin/env python3
"""video2mg - export every frame of an MP4 as a Mantis 320x240 RGB565 .mg image.

The Mantis .mg file is a raw, uncompressed raster with a fixed 8 byte header:

    offset  size  content
    0       4     ASCII "MG01"                     (4D 47 30 31)
    4       2     width  = 320, uint16 little endian (40 01)
    6       2     height = 240, uint16 little endian (F0 00)
    8       153600 pixel data, row-major, 16 bit RGB565, little endian
                  (offset = 8 + 2 * (y * 320 + x))

Every file written by this tool is exactly 153,608 bytes: 8 header bytes plus
320 * 240 * 2 pixel bytes.  No compression, no alpha, no row padding.

Requires numpy, opencv-python and tkinter:
    pip install numpy opencv-python
    sudo apt install python3-tk      (Debian/Ubuntu; the python.org build
                                      for Windows/macOS already includes Tk)
"""

import glob
import os
import queue
import re
import struct
import sys
import threading
from typing import Callable, NamedTuple, Optional

try:
    import cv2
    import numpy as np
except ImportError as exc:  # pragma: no cover - dependency guard
    cv2 = None
    np = None
    _DEPENDENCY_ERROR = str(exc)
else:
    _DEPENDENCY_ERROR = None

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError as exc:  # pragma: no cover - dependency guard
    sys.exit(
        "tkinter is not available (%s).\n"
        "Debian/Ubuntu: sudo apt install python3-tk\n"
        "Fedora: sudo dnf install python3-tkinter\n"
        "Otherwise use the python.org installer, which bundles Tk." % exc
    )


# --------------------------------------------------------------------------
# Format constants
# --------------------------------------------------------------------------

WIDTH = 320
HEIGHT = 240
HEADER_SIZE = 8
PIXEL_BYTES = WIDTH * HEIGHT * 2          # 153,600
FILE_SIZE = HEADER_SIZE + PIXEL_BYTES     # 153,608

HEADER = b"MG01" + struct.pack("<HH", WIDTH, HEIGHT)

SCALE_FIT = "Fit (letterbox)"
SCALE_CROP = "Fill (center crop)"
SCALE_STRETCH = "Stretch"

VIDEO_TYPES = [
    ("Video files", "*.mp4 *.m4v *.mov *.avi *.mkv *.webm *.mpg *.mpeg"),
    ("All files", "*.*"),
]

_INVALID_NAME_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


class ConversionError(Exception):
    """Raised when the source cannot be decoded or the input is invalid."""


class ConversionCancelled(Exception):
    """Raised when the user cancels a running export."""


class VideoInfo(NamedTuple):
    width: int
    height: int
    fps: float
    frames: int


class ExportResult(NamedTuple):
    folder: str
    prefix: str
    frames: int
    first_file: str
    total_size: int


# --------------------------------------------------------------------------
# Conversion core
# --------------------------------------------------------------------------

def rgb565_bytes(rgb) -> bytes:
    """Pack an HxWx3 uint8 RGB image into little-endian RGB565 bytes."""
    arr = np.asarray(rgb, dtype=np.uint8)
    if arr.shape[:2] != (HEIGHT, WIDTH) or arr.ndim != 3 or arr.shape[2] != 3:
        raise ConversionError("expected a %dx%d RGB image" % (WIDTH, HEIGHT))
    r = (arr[:, :, 0].astype(np.uint16) >> 3) << 11
    g = (arr[:, :, 1].astype(np.uint16) >> 2) << 5
    b = (arr[:, :, 2].astype(np.uint16) >> 3)
    return (r | g | b).astype("<u2").tobytes()


def write_mg(path: str, rgb) -> int:
    """Write one 320x240 RGB image as a complete Mantis .mg file."""
    pixels = rgb565_bytes(rgb)
    data = HEADER + pixels
    with open(path, "wb") as handle:
        handle.write(data)
    return len(data)


def _to_rgb(frame):
    """OpenCV decodes video as BGR; convert to RGB, tolerating odd layouts."""
    if frame.ndim == 2:
        return cv2.cvtColor(frame, cv2.COLOR_GRAY2RGB)
    if frame.shape[2] == 4:
        return cv2.cvtColor(frame, cv2.COLOR_BGRA2RGB)
    return cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)


def scale_frame(frame, mode: str = SCALE_FIT):
    """Resize an RGB frame to exactly 320x240 using the requested fit mode."""
    src_h, src_w = frame.shape[:2]
    if (src_w, src_h) == (WIDTH, HEIGHT):
        return frame

    if mode == SCALE_STRETCH:
        return cv2.resize(frame, (WIDTH, HEIGHT), interpolation=cv2.INTER_AREA)

    if mode == SCALE_CROP:
        scale = max(WIDTH / src_w, HEIGHT / src_h)
        new_w = max(WIDTH, int(round(src_w * scale)))
        new_h = max(HEIGHT, int(round(src_h * scale)))
        resized = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_AREA)
        x = (new_w - WIDTH) // 2
        y = (new_h - HEIGHT) // 2
        return resized[y:y + HEIGHT, x:x + WIDTH]

    # Letterbox: scale the whole frame down and centre it on a black canvas.
    scale = min(WIDTH / src_w, HEIGHT / src_h)
    new_w = max(1, min(WIDTH, int(round(src_w * scale))))
    new_h = max(1, min(HEIGHT, int(round(src_h * scale))))
    resized = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_AREA)
    canvas = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    x = (WIDTH - new_w) // 2
    y = (HEIGHT - new_h) // 2
    canvas[y:y + new_h, x:x + new_w] = resized
    return canvas


def probe_video(path: str) -> VideoInfo:
    """Read the basic properties of a video file."""
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise ConversionError("Could not open the video: %s" % path)
    try:
        fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0)
        if not (0.0 < fps < 1000.0):  # NaN, zero or nonsense
            fps = 0.0
        frames = max(int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0), 0)
        width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 0)
        height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 0)
    finally:
        cap.release()
    if width <= 0 or height <= 0:
        raise ConversionError("Could not determine the video size: %s" % path)
    return VideoInfo(width, height, fps, frames)


def plan_frame_count(video: VideoInfo, *, target_fps: Optional[float] = None,
                     max_frames: Optional[int] = None) -> int:
    """Estimate how many frames an export will produce (0 if unknown)."""
    source_fps = video.fps if video.fps > 0 else 30.0
    step = max(1.0, source_fps / target_fps) if target_fps else 1.0
    count = int(round(video.frames / step)) if video.frames > 0 else 0
    if max_frames:
        count = min(count, max_frames) if count else max_frames
    return count


def safe_prefix(name: str) -> str:
    """Turn arbitrary text into a usable file name stem."""
    cleaned = _INVALID_NAME_CHARS.sub("_", name).strip().strip(".")
    return cleaned or "frame"


def export_frames(src: str, folder: str, prefix: str, *,
                  target_fps: Optional[float] = None,
                  max_frames: Optional[int] = None,
                  scaling: str = SCALE_FIT,
                  progress: Optional[Callable[[int, int], None]] = None,
                  cancel: Optional[threading.Event] = None,
                  on_file: Optional[Callable[[str], None]] = None) -> ExportResult:
    """Decode `src` and write one 320x240 .mg file per selected frame."""
    if scaling not in (SCALE_FIT, SCALE_CROP, SCALE_STRETCH):
        raise ConversionError("unknown scaling mode: %s" % scaling)
    prefix = safe_prefix(prefix)
    folder = os.path.abspath(folder)
    if not os.path.isdir(folder):
        raise ConversionError("Output folder does not exist: %s" % folder)

    info = probe_video(src)
    source_fps = info.fps if info.fps > 0 else 30.0
    step = max(1.0, source_fps / target_fps) if target_fps else 1.0
    expected = plan_frame_count(info, target_fps=target_fps,
                                max_frames=max_frames)

    cap = cv2.VideoCapture(src)
    if not cap.isOpened():
        raise ConversionError("Could not open the video: %s" % src)

    count = 0
    next_time = 0.0
    frame_index = 0
    total_bytes = 0
    first_file = ""

    try:
        while True:
            if cancel is not None and cancel.is_set():
                raise ConversionCancelled()
            ok, frame = cap.read()
            if not ok:
                break

            take = (frame_index / source_fps) + 1e-6 >= next_time
            frame_index += 1
            if not take:
                continue
            next_time += step / source_fps

            rgb = scale_frame(_to_rgb(frame), scaling)
            path = os.path.join(folder, "%s_%04d.mg" % (prefix, count))
            total_bytes += write_mg(path, rgb)
            if not first_file:
                first_file = path
            count += 1

            if on_file is not None:
                on_file(path)
            if progress is not None:
                progress(count, expected)
            if max_frames and count >= max_frames:
                break

        if count == 0:
            raise ConversionError("No frames could be decoded from %s" % src)
    finally:
        cap.release()

    return ExportResult(folder=folder, prefix=prefix, frames=count,
                        first_file=first_file, total_size=total_bytes)


def leftover_frames(folder: str, prefix: str, written: int):
    """Existing prefix_NNNN.mg files numbered beyond this run's last frame."""
    pattern = re.compile(r"^%s_(\d+)\.mg$" % re.escape(prefix))
    stale = []
    for path in glob.glob(os.path.join(folder, "%s_*.mg" % prefix)):
        match = pattern.match(os.path.basename(path))
        if match and int(match.group(1)) >= written:
            stale.append(path)
    return sorted(stale)


# --------------------------------------------------------------------------
# GUI
# --------------------------------------------------------------------------

def human_size(num_bytes: int) -> str:
    size = float(num_bytes)
    if size < 1024:
        return "%d B" % size
    for unit in ("KB", "MB", "GB"):
        size /= 1024.0
        if size < 1024.0 or unit == "GB":
            return "%.1f %s" % (size, unit)
    return "%.1f GB" % size


class ConverterApp:
    """Tk application wrapping `export_frames`."""

    def __init__(self, master):
        self.master = master
        master.title("MP4 to Mantis .mg frames")
        master.minsize(760, 0)
        master.columnconfigure(0, weight=1)
        master.rowconfigure(0, weight=1)

        self.src_var = tk.StringVar()
        self.folder_var = tk.StringVar()
        self.prefix_var = tk.StringVar(value="frame")
        self.fps_var = tk.StringVar()
        self.max_var = tk.StringVar()
        self.scaling_var = tk.StringVar(value=SCALE_FIT)
        self.info_var = tk.StringVar(value="")
        self.names_var = tk.StringVar(value="")
        self.status_var = tk.StringVar(
            value="Choose an MP4 to export as 320 x 240 .mg frames.")

        self._queue = queue.Queue()
        self._cancel = threading.Event()
        self._worker = None
        self._controls = []
        self._video_info = None

        self._build()
        self.master.after(100, self._drain_queue)

    # -- widgets ----------------------------------------------------------

    def _build(self):
        pad = {"padx": 6, "pady": 4}
        root = ttk.Frame(self.master, padding=12)
        root.grid(row=0, column=0, sticky="nsew")
        root.columnconfigure(1, weight=1)

        ttk.Label(root, text="Input video:").grid(row=0, column=0, sticky="w", **pad)
        src_entry = ttk.Entry(root, textvariable=self.src_var)
        src_entry.grid(row=0, column=1, sticky="ew", **pad)
        src_entry.bind("<Return>", lambda _event: self._load_info())
        src_entry.bind("<FocusOut>", lambda _event: self._load_info())
        src_btn = ttk.Button(root, text="Browse...", command=self._browse_source)
        src_btn.grid(row=0, column=2, sticky="ew", **pad)
        self._controls.append(src_btn)

        ttk.Label(root, text="Output folder:").grid(row=1, column=0, sticky="w", **pad)
        ttk.Entry(root, textvariable=self.folder_var).grid(
            row=1, column=1, sticky="ew", **pad)
        folder_btn = ttk.Button(root, text="Browse...", command=self._browse_folder)
        folder_btn.grid(row=1, column=2, sticky="ew", **pad)
        self._controls.append(folder_btn)

        ttk.Label(root, text="File name prefix:").grid(
            row=2, column=0, sticky="w", **pad)
        prefix_entry = ttk.Entry(root, textvariable=self.prefix_var)
        prefix_entry.grid(row=2, column=1, sticky="ew", **pad)
        self._controls.append(prefix_entry)
        ttk.Label(root, textvariable=self.names_var, foreground="#555555").grid(
            row=3, column=0, columnspan=3, sticky="w", padx=6)

        ttk.Label(root, textvariable=self.info_var, foreground="#555555",
                  wraplength=700, justify="left").grid(
            row=4, column=0, columnspan=3, sticky="w", padx=6, pady=(2, 0))

        settings = ttk.LabelFrame(root, text="Export settings", padding=10)
        settings.grid(row=5, column=0, columnspan=3, sticky="ew", pady=(10, 4))
        settings.columnconfigure(1, weight=1)

        ttk.Label(settings, text="Scaling:").grid(row=0, column=0, sticky="w", pady=3)
        scaling_box = ttk.Combobox(
            settings, textvariable=self.scaling_var, state="readonly",
            values=[SCALE_FIT, SCALE_CROP, SCALE_STRETCH])
        scaling_box.grid(row=0, column=1, sticky="ew", padx=6, pady=3)
        self._controls.append(scaling_box)

        ttk.Label(settings, text="Frame rate:").grid(row=1, column=0, sticky="w", pady=3)
        fps_row = ttk.Frame(settings)
        fps_row.grid(row=1, column=1, sticky="w", padx=6, pady=3)
        fps_entry = ttk.Entry(fps_row, width=8, textvariable=self.fps_var)
        fps_entry.pack(side="left")
        ttk.Label(fps_row, text="fps   blank = keep every frame").pack(
            side="left", padx=(8, 0))
        self._controls.append(fps_entry)

        ttk.Label(settings, text="Max frames:").grid(row=2, column=0, sticky="w", pady=3)
        max_row = ttk.Frame(settings)
        max_row.grid(row=2, column=1, sticky="w", padx=6, pady=3)
        max_entry = ttk.Entry(max_row, width=8, textvariable=self.max_var)
        max_entry.pack(side="left")
        ttk.Label(max_row, text="frames   blank = all").pack(
            side="left", padx=(8, 0))
        self._controls.append(max_entry)

        format_note = ("Every file is a complete %d x %d image: 8 byte MG01 header "
                       "plus %s of little-endian RGB565 pixels (exactly %s)."
                       % (WIDTH, HEIGHT, "{:,}".format(PIXEL_BYTES),
                          "{:,} bytes".format(FILE_SIZE)))
        ttk.Label(settings, text=format_note, foreground="#555555",
                  wraplength=620, justify="left").grid(
            row=3, column=0, columnspan=2, sticky="w", pady=(8, 0))

        buttons = ttk.Frame(root)
        buttons.grid(row=6, column=0, columnspan=3, sticky="ew", pady=(12, 4))
        self.convert_btn = ttk.Button(buttons, text="Convert", command=self._start)
        self.convert_btn.pack(side="left")
        self.cancel_btn = ttk.Button(buttons, text="Cancel", state="disabled",
                                     command=self._cancel_click)
        self.cancel_btn.pack(side="left", padx=8)

        self.progress = ttk.Progressbar(root, mode="determinate", maximum=100)
        self.progress.grid(row=7, column=0, columnspan=3, sticky="ew", pady=(8, 4))

        ttk.Label(root, textvariable=self.status_var, wraplength=700,
                  justify="left").grid(row=8, column=0, columnspan=3,
                                       sticky="w", padx=6)

        for var in (self.src_var, self.folder_var, self.prefix_var,
                    self.fps_var, self.max_var):
            var.trace_add("write", lambda *_args: self._update_info())

    # -- helpers ----------------------------------------------------------

    def _browse_source(self):
        path = filedialog.askopenfilename(
            title="Select a video", filetypes=VIDEO_TYPES)
        if not path:
            return
        self.src_var.set(path)
        stem, _ext = os.path.splitext(os.path.basename(path))
        self.prefix_var.set(safe_prefix(stem))
        self.folder_var.set(os.path.dirname(path))
        self._load_info()

    def _browse_folder(self):
        folder = filedialog.askdirectory(title="Choose the output folder",
                                         mustexist=True)
        if folder:
            self.folder_var.set(folder)

    def _load_info(self):
        self._video_info = None
        if _DEPENDENCY_ERROR:
            self.info_var.set("Missing dependency: %s" % _DEPENDENCY_ERROR)
            return
        path = self.src_var.get().strip()
        if not path or not os.path.isfile(path):
            self.info_var.set("")
            return
        try:
            self._video_info = probe_video(path)
        except ConversionError as exc:
            self.info_var.set(str(exc))
            return
        self._update_info()

    def _read_int(self, var, label):
        text = var.get().strip()
        if not text:
            return None
        try:
            value = int(text, 10)
        except ValueError:
            raise ValueError("%s must be a whole number." % label)
        if value <= 0:
            raise ValueError("%s must be greater than zero." % label)
        return value

    def _read_float(self, var, label):
        text = var.get().strip()
        if not text:
            return None
        try:
            value = float(text)
        except ValueError:
            raise ValueError("%s must be a number." % label)
        if not (0.0 < value < 1000.0):
            raise ValueError("%s must be between 0 and 1000." % label)
        return value

    def _options(self):
        return {
            "target_fps": self._read_float(self.fps_var, "Frame rate"),
            "max_frames": self._read_int(self.max_var, "Max frames"),
            "scaling": self.scaling_var.get(),
        }

    def _update_info(self):
        prefix = safe_prefix(self.prefix_var.get())
        self.names_var.set("Writes %s_0000.mg, %s_0001.mg, ..."
                           % (prefix, prefix))

        video = self._video_info
        if video is None:
            return
        source = "%d x %d" % (video.width, video.height)
        if video.fps:
            source += ", %.2f fps" % video.fps
        if video.frames:
            source += ", %d frames" % video.frames
        text = "Source: %s" % source
        try:
            opts = self._options()
        except ValueError:
            self.info_var.set(text)
            return
        count = plan_frame_count(video, target_fps=opts["target_fps"],
                                 max_frames=opts["max_frames"])
        if count:
            text += ("\nOutput: about %d files, each %s - %s total"
                     % (count, "{:,} bytes".format(FILE_SIZE),
                        human_size(count * FILE_SIZE)))
        self.info_var.set(text)

    # -- conversion -------------------------------------------------------

    def _start(self):
        if self._worker is not None and self._worker.is_alive():
            return
        if _DEPENDENCY_ERROR:
            messagebox.showerror(
                "Missing dependency",
                "This tool needs numpy and opencv-python:\n\n"
                "    pip install numpy opencv-python\n\n%s" % _DEPENDENCY_ERROR)
            return

        src = self.src_var.get().strip()
        folder = self.folder_var.get().strip()
        prefix = safe_prefix(self.prefix_var.get())

        if not src:
            messagebox.showerror("No input", "Choose a video file first.")
            return
        if not os.path.isfile(src):
            messagebox.showerror("No input", "Video not found:\n%s" % src)
            return
        if not folder:
            messagebox.showerror("No output", "Choose an output folder.")
            return
        if not os.path.isdir(folder):
            messagebox.showerror("No output",
                                 "Folder does not exist:\n%s" % folder)
            return
        try:
            opts = self._options()
        except ValueError as exc:
            messagebox.showerror("Invalid setting", str(exc))
            return

        existing = glob.glob(os.path.join(folder, "%s_*.mg" % prefix))
        if existing and not messagebox.askyesno(
                "Replace frames?",
                "%d file(s) called %s_*.mg already exist in that folder.\n"
                "The new frames will replace them. Continue?"
                % (len(existing), prefix)):
            return

        estimate = 0
        if self._video_info is not None:
            estimate = plan_frame_count(
                self._video_info, target_fps=opts["target_fps"],
                max_frames=opts["max_frames"])

        self._cancel.clear()
        self._set_busy(True, estimate)
        self._worker = threading.Thread(
            target=self._run_worker,
            args=(src, os.path.abspath(folder), prefix, opts), daemon=True)
        self._worker.start()

    def _run_worker(self, src, folder, prefix, opts):
        try:
            result = export_frames(
                src, folder, prefix, progress=self._on_progress,
                cancel=self._cancel, **opts)
        except ConversionCancelled:
            self._queue.put(("cancelled", None))
        except Exception as exc:  # noqa: BLE001 - surfaced in the UI
            self._queue.put(("error", "%s: %s" % (type(exc).__name__, exc)))
        else:
            self._queue.put(("done", result))

    def _on_progress(self, done, total):
        self._queue.put(("progress", done, total))

    def _cancel_click(self):
        self._cancel.set()
        self.status_var.set("Cancelling...")

    def _set_busy(self, busy, estimate=0):
        for widget in self._controls:
            if busy:
                widget.configure(state="disabled")
            elif isinstance(widget, ttk.Combobox):
                widget.configure(state="readonly")
            else:
                widget.configure(state="normal")
        self.convert_btn.configure(state="disabled" if busy else "normal")
        self.cancel_btn.configure(state="normal" if busy else "disabled")
        self.progress.stop()
        if busy:
            if estimate:
                self.progress.configure(mode="determinate", maximum=estimate,
                                        value=0)
            else:
                self.progress.configure(mode="indeterminate", value=0)
                self.progress.start(15)
            self.status_var.set("Converting...")
        else:
            self.progress.configure(mode="determinate", value=0)

    def _drain_queue(self):
        try:
            while True:
                message = self._queue.get_nowait()
                kind = message[0]
                if kind == "progress":
                    done, total = message[1], message[2]
                    if total:
                        self.progress.configure(mode="determinate",
                                                maximum=total)
                        self.progress.configure(value=min(done, total))
                        self.status_var.set(
                            "Converting... %d / %d frames" % (done, total))
                    else:
                        self.status_var.set("Converting... %d frames" % done)
                elif kind == "done":
                    result = message[1]
                    self._set_busy(False)
                    self.progress.configure(maximum=max(1, result.frames),
                                            value=result.frames)
                    text = ("Done: %d frames written to %s (%s)"
                            % (result.frames, result.folder,
                               human_size(result.total_size)))
                    stale = leftover_frames(result.folder, result.prefix,
                                            result.frames)
                    if stale:
                        text += ("\nNote: %d older %s_*.mg file(s) with higher "
                                 "numbers are still in that folder."
                                 % (len(stale), result.prefix))
                    self.status_var.set(text)
                elif kind == "cancelled":
                    self._set_busy(False)
                    self.status_var.set("Cancelled - frames written so far "
                                        "were kept.")
                elif kind == "error":
                    self._set_busy(False)
                    self.status_var.set("Failed: %s" % message[1])
        except queue.Empty:
            pass
        self.master.after(100, self._drain_queue)


def main():
    root = tk.Tk()
    ConverterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""MantisCore MP3 converter: turns any audio files into the MP3 format MantisAmp plays best.

Standalone: Python 3.8+ with tkinter (included with the python.org installers) and ffmpeg
(https://ffmpeg.org; on PATH, next to this script, or picked with the Browse button).

    python3 mantis_mp3_converter.py

The output is tuned to the Core2 music player (src/ui/mantis_music.cpp):
  * 44.1 kHz - the player runs the speaker at 44.1 kHz, so nothing is resampled on the device
  * constant bitrate (CBR) - the player works out duration and seek position from the bitrate,
    so variable-bitrate files would show the wrong length and seek to the wrong place
  * mono by default - the Core2 has one speaker; mono halves the decode work and SD reads
  * ID3v2.3 title/artist (what the player shows), no cover art (big tags slow track changes)
  * no Xing/LAME header frame, so the duration is exact from the first byte
  * optional loudness normalisation (EBU R128, -14 LUFS), so songs play at the same volume
Put the results in /music on the SD card (subfolders up to two levels deep, max 1000 songs).
"""
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

AUDIO_EXT = {
    ".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wma", ".aif", ".aiff",
    ".alac", ".ape", ".wv", ".mka", ".webm", ".mp4", ".m4b", ".mkv", ".mov", ".ac3", ".amr", ".3gp",
}
BITRATES = [96, 128, 160, 192]
MAX_TRACKS = 1000   # the player lists at most this many
MAX_DEPTH = 2       # folders under /music the player looks into
_NO_WINDOW = 0x08000000 if os.name == "nt" else 0  # no console window per ffmpeg run on Windows


# ------------------------------------------------------------------ ffmpeg
def find_tool(name, hint=None):
    """ffmpeg/ffprobe: explicit path, next to this script / the exe, or on PATH."""
    exe = name + (".exe" if os.name == "nt" else "")
    cands = []
    if hint:
        cands.append(os.path.join(os.path.dirname(hint), exe) if os.path.isfile(hint) else hint)
    here = os.path.dirname(os.path.abspath(sys.argv[0] if getattr(sys, "frozen", False) else __file__))
    cands += [os.path.join(here, exe), os.path.join(here, "ffmpeg", "bin", exe)]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return shutil.which(name)


def probe(ffprobe, path):
    """Duration (s) and tags of a file; empty values if ffprobe is missing or fails."""
    if not ffprobe:
        return 0.0, {}
    try:
        out = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries", "format=duration:format_tags", "-of", "json", path],
            capture_output=True, text=True, timeout=30, creationflags=_NO_WINDOW,
        ).stdout
        fmt = json.loads(out or "{}").get("format", {})
        tags = {k.lower(): v for k, v in (fmt.get("tags") or {}).items()}
        return float(fmt.get("duration") or 0), tags
    except (OSError, ValueError, subprocess.SubprocessError):
        return 0.0, {}


def build_command(ffmpeg, src, dst, mono=True, kbps=128, normalize=True, tags=None):
    """The ffmpeg command line for one file (kept separate so it can be tested without a GUI)."""
    cmd = [ffmpeg, "-hide_banner", "-nostdin", "-y", "-i", src,
           "-map", "0:a:0",            # first audio stream only: no cover art, no video
           "-map_metadata", "-1"]      # drop everything, then write back just the useful tags
    tags = tags or {}
    for key in ("title", "artist", "album", "track"):
        val = tags.get(key) or (tags.get("album_artist") if key == "artist" else None)
        if val:
            cmd += ["-metadata", f"{key}={val}"]
    if normalize:
        cmd += ["-af", "loudnorm=I=-14:TP=-1.5:LRA=11"]
    cmd += ["-ac", "1" if mono else "2", "-ar", "44100",
            "-c:a", "libmp3lame", "-b:a", f"{kbps}k",   # -b:a without -q = constant bitrate
            "-write_xing", "0",
            "-id3v2_version", "3", "-write_id3v1", "0",
            "-progress", "pipe:1", "-nostats", "-loglevel", "error",
            dst]
    return cmd


# ------------------------------------------------------------------ file names (FAT32-safe)
_BAD = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
_RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}


def safe_name(name, limit=100):
    name = _BAD.sub("_", name).strip().rstrip(". ")
    if not name:
        name = "track"
    if name.split(".")[0].upper() in _RESERVED:
        name = "_" + name
    return name[:limit].rstrip(". ")


def plan_outputs(items, out_dir, keep_folders):
    """items: [(src, rel_dir)] -> [(src, dst)], unique FAT-safe names, folders at most 2 deep."""
    used, plan = set(), []
    for src, rel in items:
        parts = [safe_name(p, 60) for p in rel.replace("\\", "/").split("/") if p and p != "."] if keep_folders else []
        if len(parts) > MAX_DEPTH:  # the player only looks two folders deep: fold the rest in
            parts = parts[:MAX_DEPTH - 1] + [safe_name(" - ".join(parts[MAX_DEPTH - 1:]), 60)]
        base = safe_name(os.path.splitext(os.path.basename(src))[0])
        d = os.path.join(out_dir, *parts)
        dst = os.path.join(d, base + ".mp3")
        n = 2
        while dst.lower() in used:
            dst = os.path.join(d, f"{base} ({n}).mp3")
            n += 1
        used.add(dst.lower())
        plan.append((src, dst))
    return plan


def convert_one(ffmpeg, ffprobe, src, dst, opts, on_progress=None, procs=None, cancel=None):
    """Convert one file. Returns (ok, message). on_progress(fraction) while it runs."""
    duration, tags = probe(ffprobe, src)
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    tmp = dst + ".part.mp3"
    cmd = build_command(ffmpeg, src, tmp, opts["mono"], opts["kbps"], opts["normalize"], tags)
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             creationflags=_NO_WINDOW)
    except OSError as e:
        return False, f"could not start ffmpeg: {e}"
    if procs is not None:
        procs.add(p)
    try:
        for line in p.stdout:
            if cancel is not None and cancel.is_set():
                p.terminate()
                break
            if line.startswith("out_time_ms=") and duration > 0 and on_progress:
                try:
                    on_progress(min(1.0, int(line.split("=", 1)[1]) / 1e6 / duration))
                except ValueError:
                    pass
        err = p.stderr.read()
        rc = p.wait()
    finally:
        if procs is not None:
            procs.discard(p)
    if cancel is not None and cancel.is_set():
        _remove(tmp)
        return False, "cancelled"
    if rc != 0 or not os.path.isfile(tmp) or os.path.getsize(tmp) < 1024:
        _remove(tmp)
        last = (err or "").strip().splitlines()[-1:] or ["no audio found"]
        return False, last[0][:200]
    os.replace(tmp, dst)
    return True, f"{os.path.getsize(dst) / 1e6:.1f} MB"


def _remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def collect(paths):
    """Files and folders -> [(file, folder relative to what was added)], audio only, sorted."""
    out = []
    for p in paths:
        if os.path.isdir(p):
            root_name = os.path.basename(os.path.normpath(p))
            for root, dirs, files in os.walk(p):
                dirs.sort()
                for f in sorted(files):
                    if os.path.splitext(f)[1].lower() in AUDIO_EXT and not f.startswith("._"):
                        rel = os.path.relpath(root, p)
                        out.append((os.path.join(root, f), root_name if rel == "." else os.path.join(root_name, rel)))
        elif os.path.isfile(p) and os.path.splitext(p)[1].lower() in AUDIO_EXT:
            out.append((p, ""))
    return out


# ------------------------------------------------------------------ window
def main():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("MantisCore MP3 Converter")
    root.geometry("860x600")
    root.minsize(720, 480)

    state = {"items": [], "ffmpeg": find_tool("ffmpeg"), "running": False}
    events = queue.Queue()
    cancel = threading.Event()
    procs = set()

    # --- top: files
    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    ttk.Button(top, text="Add files...", command=lambda: add(filedialog.askopenfilenames(
        title="Audio files", filetypes=[("Audio", " ".join("*" + e for e in sorted(AUDIO_EXT))), ("All files", "*.*")]))
    ).pack(side="left")
    ttk.Button(top, text="Add folder...", command=lambda: add([filedialog.askdirectory(title="Folder of music")])
               ).pack(side="left", padx=6)
    ttk.Button(top, text="Clear", command=lambda: clear()).pack(side="left")
    count_lbl = ttk.Label(top, text="")
    count_lbl.pack(side="right")

    cols = ("file", "to", "status")
    tree = ttk.Treeview(root, columns=cols, show="headings", height=12)
    for c, w, t in (("file", 330, "Source"), ("to", 300, "Output (inside the output folder)"), ("status", 150, "Status")):
        tree.heading(c, text=t)
        tree.column(c, width=w, stretch=c != "status")
    tree.pack(fill="both", expand=True, padx=8)

    # --- options
    opt = ttk.LabelFrame(root, text="Format (tuned for the Core2 music player)", padding=8)
    opt.pack(fill="x", padx=8, pady=6)
    mono = tk.BooleanVar(value=True)
    kbps = tk.IntVar(value=128)
    norm = tk.BooleanVar(value=True)
    keep = tk.BooleanVar(value=True)
    ttk.Radiobutton(opt, text="Mono (recommended: one speaker)", variable=mono, value=True).grid(row=0, column=0, sticky="w")
    ttk.Radiobutton(opt, text="Stereo", variable=mono, value=False).grid(row=0, column=1, sticky="w", padx=10)
    ttk.Label(opt, text="Bitrate (constant):").grid(row=0, column=2, sticky="e", padx=(20, 4))
    ttk.Combobox(opt, textvariable=kbps, values=BITRATES, width=5, state="readonly").grid(row=0, column=3, sticky="w")
    ttk.Label(opt, text="kbps").grid(row=0, column=4, sticky="w")
    ttk.Checkbutton(opt, text="Even out loudness (-14 LUFS)", variable=norm).grid(row=1, column=0, sticky="w", pady=(6, 0))
    ttk.Checkbutton(opt, text="Keep folder structure (max 2 levels)", variable=keep,
                    command=lambda: refresh()).grid(row=1, column=1, columnspan=3, sticky="w", pady=(6, 0), padx=10)
    ttk.Label(opt, text="Always 44.1 kHz, CBR, ID3v2.3 title/artist, no cover art.", foreground="#666"
              ).grid(row=2, column=0, columnspan=5, sticky="w", pady=(6, 0))

    # --- output + ffmpeg
    io = ttk.Frame(root, padding=(8, 0))
    io.pack(fill="x")
    out_var = tk.StringVar(value=os.path.join(os.path.expanduser("~"), "MantisMusic"))
    ff_var = tk.StringVar(value=state["ffmpeg"] or "")
    ttk.Label(io, text="Output folder:").grid(row=0, column=0, sticky="w")
    ttk.Entry(io, textvariable=out_var).grid(row=0, column=1, sticky="ew", padx=4)
    ttk.Button(io, text="Browse...", command=lambda: pick_out()).grid(row=0, column=2)
    ttk.Label(io, text="ffmpeg:").grid(row=1, column=0, sticky="w", pady=4)
    ttk.Entry(io, textvariable=ff_var).grid(row=1, column=1, sticky="ew", padx=4)
    ttk.Button(io, text="Browse...", command=lambda: pick_ff()).grid(row=1, column=2)
    io.columnconfigure(1, weight=1)
    ttk.Label(io, text="Tip: choose the SD card's music folder as the output, e.g.  E:\\music  or  /Volumes/SD/music",
              foreground="#666").grid(row=2, column=1, sticky="w", padx=4)

    # --- bottom: progress
    bot = ttk.Frame(root, padding=8)
    bot.pack(fill="x")
    bar = ttk.Progressbar(bot, maximum=1000)
    bar.pack(side="left", fill="x", expand=True)
    status = ttk.Label(bot, text="", width=34)
    status.pack(side="left", padx=8)
    go = ttk.Button(bot, text="Convert")
    go.pack(side="left")

    def pick_out():
        d = filedialog.askdirectory(title="Output folder (e.g. the SD card's /music)")
        if d:
            out_var.set(d)
            refresh()

    def pick_ff():
        f = filedialog.askopenfilename(title="Locate ffmpeg")
        if f:
            ff_var.set(f)

    def add(paths):
        paths = [p for p in (paths or []) if p]
        known = {s for s, _ in state["items"]}
        state["items"] += [it for it in collect(paths) if it[0] not in known]
        refresh()

    def clear():
        if not state["running"]:
            state["items"] = []
            refresh()

    def refresh():
        tree.delete(*tree.get_children())
        plan = plan_outputs(state["items"], out_var.get(), keep.get())
        for i, (src, dst) in enumerate(plan):
            tree.insert("", "end", iid=str(i), values=(src, os.path.relpath(dst, out_var.get() or "."), "ready"))
        n = len(plan)
        count_lbl.config(text=f"{n} file{'s' if n != 1 else ''}" + (f"  (player shows the first {MAX_TRACKS})" if n > MAX_TRACKS else ""))

    def start():
        if state["running"]:
            cancel.set()
            for p in list(procs):
                try:
                    p.terminate()
                except OSError:
                    pass
            status.config(text="Stopping...")
            return
        ffmpeg = find_tool("ffmpeg", ff_var.get().strip() or None)
        if not ffmpeg:
            messagebox.showerror("ffmpeg not found",
                                 "This converter uses ffmpeg.\n\nInstall it (https://ffmpeg.org/download.html, or "
                                 "'brew install ffmpeg' / 'winget install ffmpeg' / 'sudo apt install ffmpeg'), "
                                 "or use Browse to point at ffmpeg.")
            return
        if not state["items"]:
            messagebox.showinfo("Nothing to convert", "Add some audio files or a folder first.")
            return
        out = out_var.get().strip()
        if not out:
            messagebox.showinfo("Output folder", "Choose an output folder.")
            return
        ffprobe = find_tool("ffprobe", ffmpeg)
        plan = plan_outputs(state["items"], out, keep.get())
        opts = {"mono": mono.get(), "kbps": int(kbps.get()), "normalize": norm.get()}
        state["running"] = True
        cancel.clear()
        go.config(text="Stop")
        bar["value"] = 0
        threading.Thread(target=worker, args=(ffmpeg, ffprobe, plan, opts), daemon=True).start()

    def worker(ffmpeg, ffprobe, plan, opts):
        total = len(plan)
        frac = [0.0] * total
        done = {"ok": 0, "fail": 0}
        lock = threading.Lock()

        def job(i, src, dst):
            if cancel.is_set():
                return
            events.put(("row", i, "converting"))

            def prog(f):
                frac[i] = f
                events.put(("bar", sum(frac) / total))

            ok, msg = convert_one(ffmpeg, ffprobe, src, dst, opts, prog, procs, cancel)
            frac[i] = 1.0
            with lock:
                done["ok" if ok else "fail"] += 1
            events.put(("row", i, ("done  " + msg) if ok else ("failed: " + msg)))
            events.put(("bar", sum(frac) / total))
            events.put(("status", f"{done['ok'] + done['fail']} / {total} done"))

        workers = max(1, min(4, (os.cpu_count() or 2) // 2))
        with ThreadPoolExecutor(workers) as ex:
            for i, (s, d) in enumerate(plan):
                ex.submit(job, i, s, d)
        events.put(("finished", done["ok"], done["fail"], cancel.is_set()))

    def pump():
        try:
            while True:
                ev = events.get_nowait()
                if ev[0] == "row" and tree.exists(str(ev[1])):
                    vals = list(tree.item(str(ev[1]), "values"))
                    vals[2] = ev[2]
                    tree.item(str(ev[1]), values=vals)
                    if ev[2] == "converting":
                        tree.see(str(ev[1]))
                elif ev[0] == "bar":
                    bar["value"] = ev[1] * 1000
                elif ev[0] == "status":
                    status.config(text=ev[1])
                elif ev[0] == "finished":
                    state["running"] = False
                    go.config(text="Convert")
                    ok, fail, stopped = ev[1], ev[2], ev[3]
                    status.config(text=("Stopped. " if stopped else "Finished. ") + f"{ok} converted, {fail} failed")
                    if not stopped:
                        bar["value"] = 1000
                        messagebox.showinfo("Done", f"{ok} file(s) converted" + (f", {fail} failed" if fail else "") +
                                            f".\n\nCopy them to /music on the SD card (unless you converted straight onto it).")
        except queue.Empty:
            pass
        root.after(80, pump)

    go.config(command=start)
    add(sys.argv[1:])  # files/folders given on the command line (or dropped onto the script)
    if not state["ffmpeg"]:
        status.config(text="ffmpeg not found: use Browse")
    pump()
    root.mainloop()


if __name__ == "__main__":
    main()

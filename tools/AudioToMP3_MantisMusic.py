#!/usr/bin/env python3
"""Batch audio-to-MP3 converter with a Tkinter GUI. FFmpeg auto-installs on Windows."""
import os
import shutil
import subprocess
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from pathlib import Path
import tempfile
import urllib.request
import zipfile


def app_folder():
    base = Path(os.environ.get("LOCALAPPDATA", str(Path.home())))
    folder = base / "AudioToMp3Converter"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


def find_ffmpeg():
    found = shutil.which("ffmpeg")
    if found:
        return found
    name = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"
    candidate = app_folder() / "ffmpeg" / "bin" / name
    return str(candidate) if candidate.is_file() else ""


def install_ffmpeg(status):
    existing = find_ffmpeg()
    if existing:
        return existing
    if os.name != "nt":
        raise RuntimeError("FFmpeg is not installed. Install it with your system package manager and try again.")

    root = app_folder()
    archive = root / "ffmpeg-essentials.zip"
    temp = Path(tempfile.mkdtemp(prefix="ffmpeg_", dir=root))
    url = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
    try:
        status("FFmpeg missing — downloading automatically (one-time setup)…")
        request = urllib.request.Request(url, headers={"User-Agent": "AudioToMp3Converter/1.0"})
        with urllib.request.urlopen(request, timeout=60) as response, archive.open("wb") as out:
            total = int(response.headers.get("Content-Length", "0") or 0)
            done = 0
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                if total:
                    status(f"Downloading FFmpeg… {done * 100 // total}%")

        status("Installing FFmpeg…")
        with zipfile.ZipFile(archive) as zf:
            members = [n for n in zf.namelist()
                       if n.lower().endswith("/bin/ffmpeg.exe")
                       or n.lower().endswith("/bin/ffprobe.exe")]
            if not any(n.lower().endswith("/bin/ffmpeg.exe") for n in members):
                raise RuntimeError("The FFmpeg download did not contain ffmpeg.exe.")
            for member in members:
                target = temp / "bin" / Path(member).name
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(member) as src, target.open("wb") as dst:
                    shutil.copyfileobj(src, dst)

        install_dir = root / "ffmpeg"
        if install_dir.exists():
            shutil.rmtree(install_dir)
        shutil.move(str(temp), str(install_dir))
        exe = install_dir / "bin" / "ffmpeg.exe"
        if not exe.is_file():
            raise RuntimeError("FFmpeg installation did not complete.")
        return str(exe)
    finally:
        if temp.exists():
            shutil.rmtree(temp, ignore_errors=True)
        try:
            archive.unlink()
        except FileNotFoundError:
            pass


class Converter:
    AUDIO_TYPES = [
        ("Audio files", "*.wav *.flac *.mp3 *.aac *.m4a *.ogg *.oga *.opus *.wma *.aiff *.aif *.ape *.mp4 *.mkv *.webm"),
        ("All files", "*.*"),
    ]

    def __init__(self, root):
        self.root = root
        root.title("Audio to MP3 Converter")
        root.geometry("720x510")
        root.minsize(610, 440)

        self.files = []
        self.destination = tk.StringVar()
        self.bitrate = tk.StringVar(value="192")
        self.status = tk.StringVar(value="Add one or more audio files to begin.")
        self.ffmpeg = find_ffmpeg()
        self.busy = False

        frame = ttk.Frame(root, padding=18)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="Audio to MP3 Converter",
                  font=("Segoe UI", 17, "bold")).pack(anchor="w")
        ttk.Label(frame, text="Select a batch of audio files and convert them all in one go.",
                  foreground="#555").pack(anchor="w", pady=(2, 12))

        file_frame = ttk.LabelFrame(frame, text="1. Audio files", padding=10)
        file_frame.pack(fill="both", expand=True, pady=(0, 10))

        list_area = ttk.Frame(file_frame)
        list_area.pack(fill="both", expand=True)
        self.file_list = tk.Listbox(list_area, selectmode=tk.EXTENDED,
                                    height=8, activestyle="dotbox")
        scrollbar = ttk.Scrollbar(list_area, orient="vertical",
                                  command=self.file_list.yview)
        self.file_list.configure(yscrollcommand=scrollbar.set)
        self.file_list.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        controls = ttk.Frame(file_frame)
        controls.pack(fill="x", pady=(8, 0))
        ttk.Button(controls, text="Add files…", command=self.add_files).pack(side="left")
        ttk.Button(controls, text="Add folder…", command=self.add_folder).pack(side="left", padx=6)
        ttk.Button(controls, text="Remove selected", command=self.remove_selected).pack(side="left")
        ttk.Button(controls, text="Clear list", command=self.clear_files).pack(side="left", padx=6)

        out_frame = ttk.LabelFrame(frame, text="2. Save MP3 files to", padding=10)
        out_frame.pack(fill="x", pady=(0, 10))
        ttk.Entry(out_frame, textvariable=self.destination).pack(
            side="left", fill="x", expand=True, padx=(0, 8))
        ttk.Button(out_frame, text="Choose folder…", command=self.pick_folder).pack(side="right")

        settings = ttk.LabelFrame(frame, text="3. MP3 quality", padding=10)
        settings.pack(fill="x", pady=(0, 10))
        ttk.Label(settings, text="Bitrate:").pack(side="left")
        ttk.Combobox(settings, textvariable=self.bitrate, state="readonly",
                     values=("128", "160", "192", "224", "256", "320"),
                     width=7).pack(side="left", padx=8)
        ttk.Label(settings, text="kbps  •  192 kbps recommended").pack(side="left")

        self.ffmpeg_label = ttk.Label(
            frame,
            text="FFmpeg ready" if self.ffmpeg else "FFmpeg will download automatically on first conversion.",
            foreground="#187a36" if self.ffmpeg else "#555")
        self.ffmpeg_label.pack(anchor="w", pady=(0, 6))

        self.progress = ttk.Progressbar(frame, mode="determinate", maximum=100)
        self.progress.pack(fill="x", pady=(2, 8))
        bottom = ttk.Frame(frame)
        bottom.pack(fill="x")
        ttk.Label(bottom, textvariable=self.status, wraplength=450).pack(
            side="left", fill="x", expand=True)
        self.button = ttk.Button(bottom, text="Convert all to MP3", command=self.convert)
        self.button.pack(side="right")

    def add_paths(self, paths):
        existing = {str(Path(p).resolve()).lower() for p in self.files}
        added = 0
        for path in paths:
            p = Path(path)
            if p.is_file():
                key = str(p.resolve()).lower()
                if key not in existing:
                    self.files.append(str(p))
                    self.file_list.insert(tk.END, p.name)
                    existing.add(key)
                    added += 1
        if added and not self.destination.get():
            self.destination.set(str(Path(self.files[0]).parent))
        if self.files:
            self.status.set(f"{len(self.files)} file(s) queued.")

    def add_files(self):
        paths = filedialog.askopenfilenames(title="Select audio files", filetypes=self.AUDIO_TYPES)
        if paths:
            self.add_paths(paths)

    def add_folder(self):
        folder = filedialog.askdirectory(title="Choose a folder containing audio files")
        if not folder:
            return
        extensions = {".wav", ".flac", ".mp3", ".aac", ".m4a", ".ogg", ".oga",
                      ".opus", ".wma", ".aiff", ".aif", ".ape", ".mp4", ".mkv", ".webm"}
        paths = sorted(str(p) for p in Path(folder).iterdir()
                       if p.is_file() and p.suffix.lower() in extensions)
        self.add_paths(paths)
        if not paths:
            messagebox.showinfo("No audio files found", "That folder did not contain supported audio files.")

    def remove_selected(self):
        selected = list(self.file_list.curselection())
        for index in reversed(selected):
            self.file_list.delete(index)
            del self.files[index]
        self.status.set(f"{len(self.files)} file(s) queued.")

    def clear_files(self):
        self.files.clear()
        self.file_list.delete(0, tk.END)
        self.status.set("File list cleared.")

    def pick_folder(self):
        folder = filedialog.askdirectory(title="Choose output folder")
        if folder:
            self.destination.set(folder)

    def set_status(self, text):
        self.root.after(0, self.status.set, text)

    def convert(self):
        if self.busy:
            return
        if not self.files:
            messagebox.showerror("No files selected", "Add at least one audio file.")
            return
        folder = Path(self.destination.get().strip().strip('"'))
        if not folder.is_dir():
            messagebox.showerror("No output folder", "Choose an existing destination folder.")
            return
        if self.bitrate.get() not in ("128", "160", "192", "224", "256", "320"):
            messagebox.showerror("Invalid bitrate", "Choose a bitrate from the list.")
            return

        targets = [folder / (Path(src).stem + ".mp3") for src in self.files]
        collisions = [p for p in targets if p.exists()]
        overwrite = False
        if collisions:
            answer = messagebox.askyesnocancel(
                "MP3 files already exist",
                f"{len(collisions)} output file(s) already exist.\n\n"
                "Yes = overwrite existing files\nNo = skip existing files\nCancel = stop"
            )
            if answer is None:
                return
            overwrite = answer

        self.busy = True
        self.button.configure(state="disabled")
        self.progress.configure(maximum=len(self.files), value=0)
        self.status.set("Preparing batch…")
        threading.Thread(target=self.worker,
                         args=(list(self.files), str(folder), self.bitrate.get(), overwrite),
                         daemon=True).start()

    def worker(self, sources, folder, bitrate, overwrite):
        successes, skipped, failures = [], [], []
        try:
            ffmpeg = self.ffmpeg or install_ffmpeg(self.set_status)
            self.ffmpeg = ffmpeg
            self.root.after(0, self.ffmpeg_label.configure,
                            {"text": "FFmpeg ready", "foreground": "#187a36"})
            total = len(sources)
            for index, source in enumerate(sources, start=1):
                src = Path(source)
                target = Path(folder) / (src.stem + ".mp3")
                self.set_status(f"Converting {index} of {total}: {src.name}")
                if target.exists() and not overwrite:
                    skipped.append(src.name)
                else:
                    command = [
                        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                        "-i", str(src), "-map", "0:a:0", "-vn", "-sn", "-dn",
                        "-codec:a", "libmp3lame", "-b:a", bitrate + "k",
                        "-ar", "44100", "-ac", "2", "-id3v2_version", "3",
                        str(target)
                    ]
                    result = subprocess.run(command, capture_output=True, text=True,
                                            encoding="utf-8", errors="replace")
                    if result.returncode == 0 and target.is_file():
                        successes.append(target.name)
                    else:
                        failures.append((src.name, result.stderr.strip() or "FFmpeg conversion failed."))
                self.root.after(0, self.progress.configure, {"value": index})

            self.root.after(0, self.finished, successes, skipped, failures, folder)
        except Exception as exc:
            self.root.after(0, self.finished, successes, skipped,
                            failures + [("Batch setup", str(exc))], folder)

    def finished(self, successes, skipped, failures, folder):
        self.busy = False
        self.button.configure(state="normal")
        self.progress.configure(value=len(successes) + len(skipped) + len(failures))
        summary = [f"Converted: {len(successes)}", f"Skipped: {len(skipped)}",
                   f"Failed: {len(failures)}", "", f"Output folder: {folder}"]
        if failures:
            summary.extend(["", "Errors:"])
            summary.extend(f"• {name}: {error[:300]}" for name, error in failures[:8])
            if len(failures) > 8:
                summary.append(f"…and {len(failures) - 8} more.")
        self.status.set(f"Batch finished — {len(successes)} converted, {len(skipped)} skipped, {len(failures)} failed.")
        messagebox.showinfo("Batch conversion finished", "\n".join(summary))


if __name__ == "__main__":
    root = tk.Tk()
    try:
        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
    except tk.TclError:
        pass
    Converter(root)
    root.mainloop()

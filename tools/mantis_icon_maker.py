#!/usr/bin/env python3
"""Mantis Icon Maker - turns PNG/JPG artwork into .mi icons for the Mantis loader.

.mi format (little-endian):
  "MI01" | width u16 | height u16 | RGB565 pixels (w*h*2) | alpha A8 (w*h)
Saved as <SD root>/icons/<bin name without .bin>.mi
Requires: pip install pillow
"""
import os, struct, tkinter as tk
from tkinter import filedialog, messagebox
from PIL import Image, ImageDraw, ImageTk

SIZE = 64          # icon size on device
PREVIEW = 4        # preview scale
SS = 4             # supersampling for smooth corners

def sd_root_for(path):
    d = os.path.dirname(os.path.abspath(path))
    while not os.path.ismount(d):
        parent = os.path.dirname(d)
        if parent == d: break
        d = parent
    return d

class App:
    def __init__(self, root):
        self.root, self.art, self.bin_path = root, None, None
        root.title("Mantis Icon Maker")
        f = tk.Frame(root, padx=8, pady=8); f.pack()
        tk.Button(f, text="1. Select app .bin on SD", command=self.pick_bin).grid(row=0, column=0, sticky="ew")
        self.bin_lbl = tk.Label(f, text="(no app selected)", anchor="w", width=40)
        self.bin_lbl.grid(row=0, column=1, sticky="w")
        tk.Button(f, text="2. Select artwork", command=self.pick_art).grid(row=1, column=0, sticky="ew")
        self.art_lbl = tk.Label(f, text="(no artwork)", anchor="w", width=40)
        self.art_lbl.grid(row=1, column=1, sticky="w")

        self.canvas = tk.Canvas(f, width=SIZE*PREVIEW, height=SIZE*PREVIEW, bg="#203040", highlightthickness=0)
        self.canvas.grid(row=2, column=0, columnspan=2, pady=8)

        self.vars = {}
        for i, (name, lo, hi, val) in enumerate([("Zoom %", 50, 400, 100), ("Offset X", -100, 100, 0),
                                                ("Offset Y", -100, 100, 0), ("Corner radius", 0, 32, 14)]):
            v = tk.IntVar(value=val); self.vars[name] = v
            tk.Scale(f, label=name, from_=lo, to=hi, orient="horizontal", variable=v, length=320,
                     command=lambda _e: self.render()).grid(row=3+i, column=0, columnspan=2)
        tk.Button(f, text="3. Make icon", command=self.save, bg="#3a7", fg="white").grid(row=7, column=0, columnspan=2, sticky="ew", pady=6)

    def pick_bin(self):
        p = filedialog.askopenfilename(title="Select app .bin", filetypes=[("Firmware", "*.bin")])
        if p: self.bin_path = p; self.bin_lbl.config(text=os.path.basename(p))

    def pick_art(self):
        p = filedialog.askopenfilename(title="Select artwork", filetypes=[("Images", "*.png *.jpg *.jpeg")])
        if p:
            self.art = Image.open(p).convert("RGBA"); self.art_lbl.config(text=os.path.basename(p)); self.render()

    def build(self):
        """Return SIZE x SIZE RGBA icon from current slider values."""
        big = SIZE * SS
        z = self.vars["Zoom %"].get() / 100
        w, h = self.art.size
        scale = big / min(w, h) * z                      # 100% = cover-fit
        img = self.art.resize((max(1, int(w*scale)), max(1, int(h*scale))), Image.LANCZOS)
        canvas = Image.new("RGBA", (big, big), (0, 0, 0, 0))
        ox = (big - img.width)//2 + self.vars["Offset X"].get() * big // 100
        oy = (big - img.height)//2 + self.vars["Offset Y"].get() * big // 100
        canvas.paste(img, (ox, oy), img)
        mask = Image.new("L", (big, big), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, big-1, big-1), radius=self.vars["Corner radius"].get()*SS, fill=255)
        r, g, b, alpha = canvas.split()
        alpha = Image.composite(alpha, Image.new("L", (big, big), 0), mask)
        return Image.merge("RGBA", (r, g, b, alpha)).resize((SIZE, SIZE), Image.LANCZOS)

    def render(self):
        if not self.art: return
        icon = self.build().resize((SIZE*PREVIEW,)*2, Image.NEAREST)
        self.tkimg = ImageTk.PhotoImage(icon)
        self.canvas.delete("all"); self.canvas.create_image(0, 0, anchor="nw", image=self.tkimg)

    def save(self):
        if not (self.bin_path and self.art):
            return messagebox.showwarning("Missing", "Select an app .bin and artwork first.")
        icon = self.build()
        rgb565, alpha = bytearray(), bytearray()
        for r, g, b, a in icon.getdata():
            rgb565 += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); alpha.append(a)
        out_dir = os.path.join(sd_root_for(self.bin_path), "icons"); os.makedirs(out_dir, exist_ok=True)
        name = os.path.splitext(os.path.basename(self.bin_path))[0] + ".mi"
        with open(os.path.join(out_dir, name), "wb") as fh:
            fh.write(b"MI01" + struct.pack("<HH", SIZE, SIZE) + rgb565 + alpha)
        messagebox.showinfo("Saved", f"Saved {os.path.join(out_dir, name)}")

if __name__ == "__main__":
    root = tk.Tk(); App(root); root.mainloop()

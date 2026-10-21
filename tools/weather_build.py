"""MantisCore weather screen asset builder.
Usage: python tools/weather_build.py src/ui/mantis_weather_assets.h <SD card>/mantiscore
Writes the weather backdrop (bench + instrument chassis, readout areas cleaned) to weather.mg and
the icons, animation frames, minus-sign nixie glyph and screen geometry to the header."""
import os, sys, struct, collections, re
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageChops
HERE = os.path.dirname(os.path.abspath(__file__)); ART = os.path.join(HERE, 'art')
OUT_H, OUT_SD = sys.argv[1], sys.argv[2]
os.makedirs(OUT_SD, exist_ok=True)
c565 = lambda r, g, b: ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
def arr(name, ctype, vals, per=16):
    s = f"static const {ctype} {name}[{len(vals)}] PROGMEM = {{\n"
    for i in range(0, len(vals), per): s += "  " + ",".join(str(v) for v in vals[i:i + per]) + ",\n"
    return s + "};\n"

def flood_key(img, tol, seed_color=None):
    """Make the border-connected background transparent."""
    img = img.convert('RGBA'); W, H = img.size; px = img.load()
    bg = seed_color or px[1, 1][:3]
    seen = bytearray(W * H); q = collections.deque()
    for x in range(W): q += [(x, 0), (x, H - 1)]
    for y in range(H): q += [(0, y), (W - 1, y)]
    while q:
        x, y = q.popleft()
        if not (0 <= x < W and 0 <= y < H) or seen[y * W + x]: continue
        seen[y * W + x] = 1
        r, g, b, a = px[x, y]
        if abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2]) >= tol: continue
        px[x, y] = (r, g, b, 0)
        q += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    return img

# ------------------------------------------------------------------ backdrop
sheet = Image.open(os.path.join(ART, 'weather_sheet_chassis.jpg')).convert('RGB')
chassis = flood_key(sheet.crop((40, 112, 780, 612)), 45)
chassis = chassis.crop((0, 0, 738, 500))  # geometry below was measured at exactly this scale (320/738)
S = 320 / chassis.width
chs = chassis.resize((320, round(chassis.height * S)), Image.LANCZOS)
Y0 = 240 - chs.height
bench = Image.open(os.path.join(ART, 'nixie_bench.jpg')).convert('RGB').resize((440, 240), Image.LANCZOS).crop((60, 0, 380, 240))
bench = ImageEnhance.Brightness(bench).enhance(0.55)
bg = bench.convert('RGBA'); bg.alpha_composite(chs, (0, Y0)); bg = bg.convert('RGB')
d = ImageDraw.Draw(bg)

# geometry (device pixels, measured on the composed backdrop)
TUBES_CX = [112, 142, 172, 202]; TUBE_CY = 82
PLATE = (108, 127, 206, 141)
CELLS = [(86, 157, 117, 177), (119, 157, 150, 177), (152, 157, 186, 177), (188, 157, 227, 177),
         (86, 179, 117, 201), (119, 179, 150, 201), (152, 179, 186, 201), (188, 179, 227, 201)]
CRT = (240, 67, 297, 141)       # inside the bezel
WAVE = (236, 161, 296, 199)

# clean the printed labels so the firmware can draw live ones
plate_px = sorted(bg.crop(PLATE).getdata(), key=sum); plate_col = plate_px[len(plate_px) // 2]
d.rectangle(PLATE, fill=plate_col)
for (x0, y0, x1, y1) in CELLS:
    d.rectangle((x0 + 1, y0 + 1, x1 - 1, y1 - 1), fill=(34, 36, 40))
    d.line((x0 + 1, y0 + 1, x1 - 1, y0 + 1), fill=(22, 23, 26)); d.line((x0 + 1, y0 + 1, x0 + 1, y1 - 1), fill=(22, 23, 26))
d.rounded_rectangle(CRT, radius=7, fill=(6, 18, 11))
d.rectangle(WAVE, fill=(6, 14, 10)); d.rectangle(WAVE, outline=(20, 40, 30))
d.rectangle((0, 0, 319, Y0 - 1), fill=None)
# header strip: darken
hdr = bg.crop((0, 0, 320, 22)); hdr = ImageEnhance.Brightness(hdr).enhance(0.45); bg.paste(hdr, (0, 0))
with open(os.path.join(OUT_SD, 'weather.mg'), 'wb') as f:
    f.write(b'MG01' + struct.pack('<HH', 320, 240))
    f.write(b''.join(struct.pack('<H', c565(*p)) for p in bg.getdata()))

# ------------------------------------------------------------------ icons (32x32, RGB565 + alpha)
icons_sheet = Image.open(os.path.join(ART, 'weather_sheet_icons.jpg')).convert('RGB')
SHEET_BG = (34, 37, 44)
def icon_from(box, size=32):
    im = flood_key(icons_sheet.crop(box), 40, SHEET_BG)
    bb = im.getbbox(); im = im.crop(bb)
    k = min((size - 2) / im.width, (size - 2) / im.height)
    im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    out = Image.new('RGBA', (size, size), (0, 0, 0, 0)); out.paste(im, ((size - im.width) // 2, (size - im.height) // 2), im)
    return out
sun = icon_from((945, 52, 1032, 140)); partly = icon_from((1030, 52, 1112, 138)); rain = icon_from((1114, 55, 1196, 138))
bolt = icon_from((1200, 50, 1268, 140)); snow = icon_from((1276, 50, 1352, 133)); cloud = icon_from((946, 152, 1030, 194))
heavy = icon_from((1114, 148, 1196, 222)); wind = icon_from((946, 232, 1034, 298))
storm = cloud.copy(); sb = bolt.resize((18, 18), Image.LANCZOS); storm.alpha_composite(sb, (14, 14))
fog = cloud.copy(); fd = ImageDraw.Draw(fog)
for y in (24, 28): fd.line((5, y, 26, y), fill=(190, 200, 210, 255), width=2)
ICONS = [('SUN', sun), ('PARTLY', partly), ('CLOUD', cloud), ('FOG', fog), ('RAIN', rain), ('HEAVY', heavy),
         ('SNOW', snow), ('STORM', storm), ('WIND', wind)]
H = "// Generated by tools/weather_build.py - do not edit\n#pragma once\n#include <stdint.h>\n#include <pgmspace.h>\n\n"
H += "#define WX_ICON 32\nenum McWxIcon { " + ", ".join(f"WX_{n}" for n, _ in ICONS) + ", WX_ICON_COUNT };\n"
rgb, alp = [], []
for _, im in ICONS:
    for r, g, b, a in im.getdata(): rgb.append(c565(r, g, b)); alp.append(a)
H += arr("WX_ICON_RGB", "uint16_t", rgb) + arr("WX_ICON_A", "uint8_t", alp, 32)

# ------------------------------------------------------------------ animation frames (additive light on the CRT)
AX, AY = 950, 525
def tile(x0, y0, x1, y1): return icons_sheet.crop((AX + x0 + 3, AY + y0 + 3, AX + x1 - 3, AY + y1 - 3))
def additive(img, size, floor):
    """Tile on dark navy -> light only (tile colour subtracted)."""
    img = img.resize(size, Image.LANCZOS)
    base = Image.new('RGB', img.size, floor)
    return ImageChops.subtract(img, base)
cw, chh = CRT[2] - CRT[0], CRT[3] - CRT[1]
rain_frames = [additive(tile(*b), (cw, chh), (14, 22, 45)) for b in [(16, 14, 106, 103), (120, 14, 208, 103), (222, 14, 312, 103), (325, 14, 414, 103)]]
bolt_frames = [additive(tile(*b), (30, 30), (10, 14, 32)) for b in [(182, 125, 256, 198), (262, 125, 336, 198), (342, 125, 415, 198)]]
H += f"#define WX_CRT_X {CRT[0]}\n#define WX_CRT_Y {CRT[1]}\n#define WX_CRT_W {cw}\n#define WX_CRT_H {chh}\n"
H += arr("WX_RAIN_FRAMES", "uint16_t", [c565(*p) for f in rain_frames for p in f.getdata()])
H += "#define WX_BOLT 30\n" + arr("WX_BOLT_FRAMES", "uint16_t", [c565(*p) for f in bolt_frames for p in f.getdata()])

# ------------------------------------------------------------------ nixie minus sign (same box as the clock glyphs)
sys.path.insert(0, HERE)
hdr_txt = open(os.path.join(HERE, '..', 'src', 'ui', 'mantis_assets.h')).read()
GW = int(re.search(r'#define NIXIE_GW (\d+)', hdr_txt).group(1)); GH = int(re.search(r'#define NIXIE_GH (\d+)', hdr_txt).group(1))
PAD = int(re.search(r'#define NIXIE_PAD (\d+)', hdr_txt).group(1))
m = Image.new('RGB', (GW, GH)); md = ImageDraw.Draw(m)
cy = GH // 2; md.line((PAD + 2, cy, GW - PAD - 3, cy), fill=(255, 120, 40), width=3)
m = ImageChops.add(m.filter(ImageFilter.GaussianBlur(3)), m)
H += "// nixie '-' in the clock glyph box (NIXIE_GW x NIXIE_GH)\n" + arr("WX_NIXIE_MINUS", "uint16_t", [c565(*p) for p in m.getdata()])

# ------------------------------------------------------------------ geometry
H += "static const int16_t WX_TUBE_XY[4][2] = {" + ",".join(f"{{{cx - GW // 2},{TUBE_CY - GH // 2}}}" for cx in TUBES_CX) + "};\n"
H += f"static const int16_t WX_PLATE[4] = {{{PLATE[0]},{PLATE[1]},{PLATE[2]},{PLATE[3]}}};\n"
H += "static const int16_t WX_CELLS[8][4] = {" + ",".join("{%d,%d,%d,%d}" % c for c in CELLS) + "};\n"
H += f"static const int16_t WX_WAVE[4] = {{{WAVE[0]},{WAVE[1]},{WAVE[2]},{WAVE[3]}}};\n"
H += f"#define WX_HEADER_H {Y0 - 1}\n"
open(OUT_H, 'w').write(H)
print('ok', os.path.getsize(OUT_H) // 1024, 'KB header; chassis top', Y0)

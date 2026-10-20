# Builds nixie clock assets (sheet coords measured from IMG_6639)
import os
from PIL import Image, ImageFilter, ImageChops
ART=os.path.join(os.path.dirname(os.path.abspath(__file__)),'art')
sheet=Image.open(os.path.join(ART,'nixie_sheet.jpg')).convert('RGB')
BG=(71,74,79)
def keyout(img,tol=24):
    img=img.convert('RGBA'); px=img.load()
    for y in range(img.height):
        for x in range(img.width):
            r,g,b,a=px[x,y]
            if abs(r-BG[0])+abs(g-BG[1])+abs(b-BG[2])<tol: px[x,y]=(r,g,b,0)
    img.putalpha(img.split()[3].filter(ImageFilter.MedianFilter(5))); return img
SOCK_X=[302,436,570,704,838]; SOCK_Y=140          # socket hole centres
TUBE_CX, TUBE_TOP, TUBE_BOT = 1172, 52, 244        # glass only, pins cut off
WIN=(-22,-117,25,-30)                              # digit window rel. to (centre, glass bottom)
board=keyout(sheet.crop((52,58,945,250)))
tube=keyout(sheet.crop((TUBE_CX-46,TUBE_TOP,TUBE_CX+46,TUBE_BOT)),22)
# assembly canvas in sheet coords: x 52..945, y -60..250
OX,OY=52,-60
asm=Image.new('RGBA',(893,310),(0,0,0,0)); asm.paste(board,(0,58-OY),board)
for sx in SOCK_X:
    asm.alpha_composite(tube,(sx-46-OX, SOCK_Y-(TUBE_BOT-TUBE_TOP)-OY))
# glyphs: keep orange light only, crop to lit bbox
def glyph(cx,y0,y1):
    g=sheet.crop((cx-52,y0,cx+52,y1)); out=Image.new('RGB',g.size); gp,op=g.load(),out.load()
    for y in range(g.height):
        for x in range(g.width):
            r,gg,b=gp[x,y]; k=min(1,max(0,r-b-25)/110); op[x,y]=(int(r*k),int(gg*k),int(b*k))
    bb=out.convert('L').point(lambda v:255 if v>40 else 0).getbbox(); return out.crop(bb)
r1=[140,330,515,705,890,1080,1268]; r2=[140,330,515,705,890,1268]
D={0:(r1[0],360,512),1:(r1[1],360,512),2:(r1[2],360,512),3:(r1[3],360,512),4:(r1[4],360,512),
   5:(r1[6],360,512),6:(r2[1],568,720),7:(r2[2],568,720),8:(r2[3],568,720),9:(r2[4],568,720),':':(r2[5],568,720)}
G={k:glyph(*v) for k,v in D.items()}
# bench background, centre-cropped to 320x240
bench=Image.open(os.path.join(ART,'nixie_bench.jpg')).convert('RGB').resize((440,240),Image.LANCZOS).crop((60,0,380,240))
S=0.35; AX,AY=4,92                                   # assembly scale/position on screen
asm_s=asm.resize((round(893*S),round(310*S)),Image.LANCZOS)
scene=bench.copy(); scene.paste(asm_s,(AX,AY),asm_s)
def to_screen(x,y): return AX+round((x-OX)*S), AY+round((y-OY)*S)
def render(t):
    s=scene.copy()
    for ch,sx in zip(t,SOCK_X):
        x0,y0=to_screen(sx+WIN[0],SOCK_Y+WIN[1]); x1,y1=to_screen(sx+WIN[2],SOCK_Y+WIN[3])
        g=G[ch if ch==':' else int(ch)]
        if ch==':': w=max(4,(x1-x0)//3); g=g.resize((w,round(g.height*w/g.width)),Image.LANCZOS); gx=(x0+x1-w)//2; gy=(y0+y1-g.height)//2
        else: g=g.resize((x1-x0,y1-y0),Image.LANCZOS); gx,gy=x0,y0
        pad=6; L=Image.new('RGB',(g.width+2*pad,g.height+2*pad)); L.paste(g,(pad,pad))
        for layer in (L.filter(ImageFilter.GaussianBlur(3)),L):
            box=(gx-pad,gy-pad,gx-pad+L.width,gy-pad+L.height); s.paste(ImageChops.add(s.crop(box),layer),box[:2])
    return s



# loading-screen background


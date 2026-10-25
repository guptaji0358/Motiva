"""Illustrated mockups of the Motiva 2.x GL interface (derived from Theme.cpp / GlUi.cpp)."""
import os, colorsys
OUT = r"C:\Users\lenovo\OneDrive\Desktop\projects\Motiva\website\Assets\screenshots"
FONT = "Bahnschrift, 'Segoe UI', system-ui, sans-serif"

def hexc(s): s = s.lstrip('#'); return tuple(int(s[i:i+2], 16) for i in (0, 2, 4))
def tohex(c): return '#%02x%02x%02x' % tuple(max(0, min(255, int(round(v)))) for v in c)
def mix(a, b, t): a, b = hexc(a) if isinstance(a, str) else a, hexc(b) if isinstance(b, str) else b; return tohex([a[i] + (b[i] - a[i]) * t for i in range(3)])
def accent2(acc):
    r, g, b = [v / 255 for v in hexc(acc)]
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    h = (h + 0.16) % 1.0
    return tohex([c * 255 for c in colorsys.hsv_to_rgb(h, max(s, 0.65), max(v, 0.88))])

RAW = {
 "da": dict(name="Dark Aurora", win="#1e1e1e", text="#f0f0f0", dim="#9a9a9a", acc="#3fae5c", dark=True, onyx=False),
 "la": dict(name="Light Aurora", win="#f3f3f3", text="#1b1b1b", dim="#6e6e6e", acc="#2f9750", dark=False, onyx=False),
 "do": dict(name="Dark Onyx", win="#141414", text="#f5f5f5", dim="#9d9d9d", acc="#34c76f", dark=True, onyx=True),
 "lo": dict(name="Light Onyx", win="#f5f5f2", text="#14171a", dim="#5a5d60", acc="#1f8a4c", dark=False, onyx=True),
}
def P(key):
    r = dict(RAW[key]); r['acc2'] = accent2(r['acc'])
    r['tint'] = mix(r['win'], r['acc'], 0.30 if r['dark'] else 0.16)
    r['glass'] = 'rgba(255,255,255,.085)' if r['dark'] else 'rgba(255,255,255,.62)'
    r['edge'] = 'rgba(255,255,255,.20)' if r['dark'] else 'rgba(0,0,0,.14)'
    r['rad'] = 5 if r['onyx'] else 16
    r['key'] = key
    return r

STYLE = """<style>
text{font-family:%s}
@media (prefers-reduced-motion:reduce){*{animation:none!important}}
.d1{animation:d1 16s ease-in-out infinite alternate}.d2{animation:d2 19s ease-in-out infinite alternate}.d3{animation:d3 13s ease-in-out infinite alternate}
@keyframes d1{to{transform:translate(70px,50px)}}@keyframes d2{to{transform:translate(-80px,-40px)}}@keyframes d3{to{transform:translate(40px,-60px)}}
.wave{stroke-dasharray:14 10;animation:wv 6s linear infinite}@keyframes wv{to{stroke-dashoffset:-96}}
.wave2{stroke-dasharray:22 16;animation:wv 9s linear infinite reverse}
.s1{animation:s1 10s infinite}.s2{animation:s2 10s infinite}.s3{animation:s3 10s infinite}
@keyframes s1{0%%,30%%{opacity:1}34%%,96%%{opacity:0}100%%{opacity:1}}
@keyframes s2{0%%,30%%{opacity:0}34%%,48%%{opacity:1}52%%,100%%{opacity:0}}
@keyframes s3{0%%,48%%{opacity:0}52%%,92%%{opacity:1}96%%,100%%{opacity:0}}
.pulse{animation:pl 2.4s ease-in-out infinite}@keyframes pl{50%%{opacity:.35}}
.spin{transform-box:fill-box;transform-origin:center;animation:sp 1.1s linear infinite}@keyframes sp{to{transform:rotate(360deg)}}
.hov{animation:hv 12s infinite steps(1)}
.flowdash{stroke-dasharray:9 9;animation:fd 1.4s linear infinite}@keyframes fd{to{stroke-dashoffset:-36}}
.tg{animation:tg 6s ease-in-out infinite alternate}@keyframes tg{0%%,35%%{transform:translateX(0)}65%%,100%%{transform:translateX(20px)}}
.tgb{animation:tgb 6s ease-in-out infinite alternate}@keyframes tgb{0%%,35%%{opacity:0}65%%,100%%{opacity:1}}
.tg2{animation:tg 6s ease-in-out infinite alternate-reverse}.tgb2{animation:tgb 6s ease-in-out infinite alternate-reverse}
.thumb{animation:th 5s ease-in-out infinite alternate}@keyframes th{from{transform:translateX(-70px)}to{transform:translateX(40px)}}
.fill{animation:fl 5s ease-in-out infinite alternate;transform-box:fill-box;transform-origin:left}@keyframes fl{from{transform:scaleX(.45)}to{transform:scaleX(1)}}
.ring{animation:rg 12s infinite steps(1)}
.fade{animation:fa 12s infinite}
</style>""" % FONT

def esc(t): return t.replace('&', '&amp;').replace('<', '&lt;')
def T(x, y, t, size=14, fill="#fff", w=400, anchor="start", extra=""):
    return f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{w}" fill="{fill}" text-anchor="{anchor}" {extra}>{esc(t)}</text>'
def R(x, y, w, h, fill="none", r=0, stroke=None, sw=1, extra=""):
    st = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ''
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}"{st} {extra}/>'

def gdefs(p, u):
    a, b = p['acc'], p['acc2']
    return f'''<defs>
<linearGradient id="bg{u}" x1="0" y1="0" x2=".25" y2="1"><stop offset="0" stop-color="{p['win']}"/><stop offset="1" stop-color="{p['tint']}"/></linearGradient>
<radialGradient id="ba{u}"><stop offset="0" stop-color="{a}" stop-opacity="{.42 if p['dark'] else .32}"/><stop offset="1" stop-color="{a}" stop-opacity="0"/></radialGradient>
<radialGradient id="bb{u}"><stop offset="0" stop-color="{b}" stop-opacity="{.42 if p['dark'] else .30}"/><stop offset="1" stop-color="{b}" stop-opacity="0"/></radialGradient>
<radialGradient id="vg{u}" cx=".5" cy=".5" r=".75"><stop offset=".55" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity=".42"/></radialGradient>
<linearGradient id="pb{u}" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="{a}"/><stop offset="1" stop-color="{mix(a,b,.85)}"/></linearGradient>
<linearGradient id="pm{u}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{b}"/><stop offset="1" stop-color="{mix(b,'#000000',.3)}"/></linearGradient>
<linearGradient id="sh{u}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".28"/><stop offset=".55" stop-color="#fff" stop-opacity="0"/></linearGradient>
<linearGradient id="rim{u}" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{a}" stop-opacity=".55"/><stop offset="1" stop-color="{b}" stop-opacity=".25"/></linearGradient>
<filter id="gl{u}" x="-20%" y="-40%" width="140%" height="180%"><feGaussianBlur stdDeviation="7"/></filter>
</defs>'''

def thumbdefs(u):
    cols = [("#0f9e8e", "#2b5fd0"), ("#7a45d6", "#1f3a9a"), ("#14a37a", "#0b5a86"), ("#d6624a", "#7a2fa8"),
            ("#1fb0d1", "#4b4fd6"), ("#52c06a", "#0f7f8e"), ("#a84fd6", "#d65a8f"), ("#2b6fd6", "#12b5a0")]
    s = ''.join(f'<linearGradient id="t{u}{i}" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{a}"/><stop offset="1" stop-color="{b}"/></linearGradient>' for i, (a, b) in enumerate(cols))
    s += f'<filter id="bl{u}" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="38"/></filter>'
    return f'<defs>{s}</defs>'

def bgfill(p, u, w, h):
    s = R(0, 0, w, h, f'url(#bg{u})')
    s += f'<g class="d1"><circle cx="{w*.22}" cy="{h*.2}" r="{max(w,h)*.42}" fill="url(#ba{u})"/></g>'
    s += f'<g class="d2"><circle cx="{w*.82}" cy="{h*.72}" r="{max(w,h)*.46}" fill="url(#bb{u})"/></g>'
    s += f'<g class="d3"><circle cx="{w*.55}" cy="{h*.5}" r="{max(w,h)*.28}" fill="url(#ba{u})" opacity=".6"/></g>'
    if p['dark']: s += R(0, 0, w, h, f'url(#vg{u})')
    return s

def glass(p, u, x, y, w, h, r=None, glow=False, extra=""):
    r = p['rad'] if r is None else r
    s = ''
    if glow: s += R(x, y, w, h, p['acc2'], r, extra='opacity=".35" filter="url(#gl%s)"' % u)
    s += R(x, y, w, h, 'rgba(0,0,0,.18)' if p['dark'] else 'rgba(0,0,0,.05)', r, extra=f'transform="translate(0 3)"')
    s += R(x, y, w, h, p['glass'], r, extra=extra)
    s += R(x, y, w, h, f'url(#sh{u})', r, extra='opacity="%s"' % (.7 if p['dark'] else 1))
    s += R(x + .6, y + .6, w - 1.2, h - 1.2, 'none', r, f'url(#rim{u})', 1.2)
    return s

def pill(p): return lambda h: h / 2 if not p['onyx'] else max(5, h * .5 * .4 + 1.2)

def sbtn(p, u, x, y, w, h, label, size=14, icon=None):
    r = pill(p)(h)
    s = R(x, y, w, h, p['glass'], r) + R(x + .5, y + .5, w - 1, h - 1, 'none', r, p['edge'], 1.1)
    if icon: s += icon(x + 14, y + h / 2, p['text'])
    s += T(x + (w + (22 if icon else 0)) / 2, y + h / 2 + size * .35, label, size, p['text'], 600, 'middle')
    return s

def pbtn(p, u, x, y, w, h, label, icon=True, size=17):
    g = p['u']
    r = pill(p)(h)
    clip = f'pc{u}{int(x)}{int(y)}'
    s = f'<clipPath id="{clip}"><rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}"/></clipPath>'
    s += R(x, y, w, h, p['acc2'], r, extra='opacity=".4" filter="url(#gl%s)"' % g)
    s += f'<g clip-path="url(#{clip})">' + R(x, y, w, h, f'url(#pb{g})')
    seg = h if icon else 0
    skew = h * .2
    if icon:
        s += f'<polygon points="{x},{y} {x+seg+skew},{y} {x+seg-skew},{y+h} {x},{y+h}" fill="url(#pm{g})"/>'
    s += R(x, y, w, h, f'url(#sh{g})') + '</g>'
    ink = '#fff'
    if icon:
        cx, cy = x + seg / 2 - 2, y + h / 2
        s += f'<rect x="{cx-11}" y="{cy-9}" width="22" height="15" rx="2.5" fill="none" stroke="{ink}" stroke-width="1.8"/><path d="M{cx-6} {cy+10}h12M{cx} {cy+6}v4" stroke="{ink}" stroke-width="1.8" fill="none"/>'
    s += T(x + seg + (w - seg) / 2, y + h / 2 + size * .35, label, size, ink, 700, 'middle')
    return s

def icbtn(p, u, x, y, s_, kind):
    r = s_ * (.28 + .22 * (0.4 if p['onyx'] else 1))
    s = R(x, y, s_, s_, p['glass'], r) + R(x + .5, y + .5, s_ - 1, s_ - 1, 'none', r, p['edge'], 1.1)
    cx, cy = x + s_ / 2, y + s_ / 2
    c = p['text']
    if kind == 'play': s += f'<path d="M{cx-5} {cy-8}l15 8-15 8z" fill="{c}"/>'
    elif kind == 'pause': s += f'<rect x="{cx-7}" y="{cy-8}" width="5" height="16" rx="1" fill="{c}"/><rect x="{cx+2}" y="{cy-8}" width="5" height="16" rx="1" fill="{c}"/>'
    elif kind == 'remove': s += f'<path d="M{cx-7} {cy-7}l14 14M{cx+7} {cy-7}l-14 14" stroke="{c}" stroke-width="2" stroke-linecap="round"/>'
    return s

def folder_icon(x, y, c): return f'<path d="M{x-8} {y-5}h6l2 2h8v9h-16z" fill="none" stroke="{c}" stroke-width="1.6" stroke-linejoin="round"/>'
def list_icon(x, y, c): return f'<path d="M{x-7} {y-5}h14M{x-7} {y}h14M{x-7} {y+5}h14" stroke="{c}" stroke-width="1.7" stroke-linecap="round"/>'
def gear_icon(x, y, c): return f'<circle cx="{x}" cy="{y}" r="3.2" fill="none" stroke="{c}" stroke-width="1.7"/><circle cx="{x}" cy="{y}" r="7" fill="none" stroke="{c}" stroke-width="2.4" stroke-dasharray="2.6 3"/>'

def winctl(p, x, y, w=36, h=30, which=3):
    s = ''
    cx = x
    for i, k in enumerate(('min', 'max', 'close')[3 - which:]):
        bx = cx + i * (w + 2)
        c = p['dim']
        mx, my = bx + w / 2, y + h / 2
        if k == 'min': s += f'<path d="M{mx-6} {my}h12" stroke="{c}" stroke-width="1.6"/>'
        if k == 'max': s += f'<rect x="{mx-5.5}" y="{my-5.5}" width="11" height="11" rx="1.5" fill="none" stroke="{c}" stroke-width="1.6"/>'
        if k == 'close': s += f'<path d="M{mx-5.5} {my-5.5}l11 11M{mx+5.5} {my-5.5}l-11 11" stroke="{c}" stroke-width="1.6"/>'
    return s

def logo(p, x, y, sz=24):
    return f'<rect x="{x}" y="{y}" width="{sz}" height="{sz}" rx="{sz*.26}" fill="url(#pb{p["u"]})"/><path d="M{x+sz*.38} {y+sz*.28}l{sz*.3} {sz*.22}-{sz*.3} {sz*.22}z" fill="#fff"/>'

def media(p, u, x, y, w, h, idx=0, r=None, lines=True, clip_id=None):
    r = p['rad'] - 2 if r is None else r
    cid = clip_id or f'm{u}{int(x)}{int(y)}'
    s = f'<clipPath id="{cid}"><rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}"/></clipPath><g clip-path="url(#{cid})">'
    s += R(x, y, w, h, f'url(#t{u}{idx})')
    s += f'<circle cx="{x+w*.2}" cy="{y+h*.25}" r="{w*.3}" fill="#7be6ff" opacity=".35" filter="url(#bl{u})"/><circle cx="{x+w*.85}" cy="{y+h*.8}" r="{w*.35}" fill="#b08bff" opacity=".4" filter="url(#bl{u})"/>'
    if lines:
        for k, (yy, op, cl) in enumerate(((.62, .6, 'wave'), (.70, .42, 'wave2'), (.78, .3, 'wave'))):
            Y = y + h * yy
            s += f'<path class="{cl}" d="M{x} {Y}C{x+w*.25} {Y-h*.12} {x+w*.4} {Y+h*.1} {x+w*.6} {Y}S{x+w*.9} {Y-h*.1} {x+w} {Y+h*.02}" fill="none" stroke="#dbe9ff" stroke-opacity="{op}" stroke-width="1.7"/>'
    s += '</g>'
    return s

def frame(p, u, w, h, body, title="Motiva"):
    p['u'] = u
    rr = 10
    s = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img" aria-label="Illustration of the {esc(title)} window ({p["name"]} theme)">'
    s += STYLE + gdefs(p, u) + thumbdefs(u)
    s += f'<clipPath id="wc{u}"><rect width="{w}" height="{h}" rx="{rr}"/></clipPath><g clip-path="url(#wc{u})">' + bgfill(p, u, w, h) + body + '</g>'
    s += f'<rect x=".5" y=".5" width="{w-1}" height="{h-1}" rx="{rr}" fill="none" stroke="{p["edge"]}"/></svg>'
    return s

# ------------------------------------------------------------------ main window
W, H = 1000, 700
def main_body(p, u, w=W, h=H):
    p['u'] = u
    m, titleH, dockH = 12, 46, 172
    s = glass(p, u, m, m, w - 2 * m, titleH)
    s += logo(p, m + 14, m + 11) + T(m + 48, m + 22, "Motiva", 17, p['text'], 700) + T(m + 48, m + 38, "Live wallpapers", 11, p['dim'])
    x = w - m - 10 - 36
    s += winctl(p, x - 76, m + 8, 36, 30, 3)
    x -= 76 + 14 + 112
    s += sbtn(p, u, x, m + 7, 112, 32, "Settings", 14, gear_icon)
    s += sbtn(p, u, x - 8 - 118, m + 7, 118, 32, "Playlists", 14, list_icon)
    dock_y = h - m - dockH
    prev_y = m + titleH + 10
    s += glass(p, u, m, prev_y, w - 2 * m, dock_y - 10 - prev_y, p['rad'] + 2)
    s += media(p, u, m + 6, prev_y + 6, w - 2 * m - 12, dock_y - 10 - prev_y - 12, 0, p['rad'] - 2)
    s += glass(p, u, m, dock_y, w - 2 * m, dockH, p['rad'] + 2)
    pad = 22
    rx = w - m - pad
    s += icbtn(p, u, rx - 40, dock_y + 21, 40, 'play')
    s += sbtn(p, u, rx - 48 - 132, dock_y + 24, 132, 34, "Open Media", 14, folder_icon)
    s += icbtn(p, u, rx - 48 - 140 - 40 - 0, dock_y + 21, 40, 'remove')
    s += T(m + pad, dock_y + 36, "Aurora Loop.gif", 16, p['text'], 700) + T(m + pad, dock_y + 58, "960×540  •  GIF  •  sample artwork", 12.5, p['dim'])
    sy = dock_y + 18 + 52
    by = dock_y + dockH - pad - 52
    bw = w - 2 * m - 2 * pad
    # three states cycling: Ready -> Applying -> Active
    s += f'<g class="s1"><circle cx="{m+pad+7}" cy="{sy+11}" r="5" fill="{p["dim"]}"/>' + T(m + pad + 22, sy + 16, "Ready", 14, p['dim']) + pbtn(p, u + 'a', m + pad, by, bw, 52, "Set as Wallpaper") + '</g>'
    s += f'<g class="s2"><circle cx="{m+pad+7}" cy="{sy+11}" r="5" fill="#e0a43a" class="pulse"/>' + T(m + pad + 22, sy + 16, "Applying…", 14, '#e0a43a')
    s += R(m + pad, by, bw, 52, p['glass'], pill(p)(52)) + R(m + pad + .5, by + .5, bw - 1, 51, 'none', pill(p)(52), p['edge'], 1.1)
    s += f'<g class="spin"><circle cx="{w/2-70}" cy="{by+26}" r="8" fill="none" stroke="{p["acc"]}" stroke-width="2.4" stroke-dasharray="26 26"/></g>' + T(w / 2 - 50, by + 32, "Applying…", 17, p['text'], 700) + '</g>'
    s += f'<g class="s3"><circle cx="{m+pad+7}" cy="{sy+11}" r="5" fill="{p["acc"]}"/>' + T(m + pad + 22, sy + 16, "Active", 14, p['acc']) + pbtn(p, u + 'c', m + pad, by, bw, 52, "Remove Wallpaper") + '</g>'
    return s

def main_win(key, u):
    p = P(key)
    return frame(p, u, W, H, main_body(p, u))

# ------------------------------------------------------------------ utility window chrome
def uchrome(p, u, w, h, title, sub=None):
    m = 12
    s = glass(p, u, m, m, w - 2 * m, 46)
    s += logo(p, m + 14, m + 11) + T(m + 48, m + 22, title, 16, p['text'], 700)
    if sub: s += T(m + 48, m + 38, sub, 11, p['dim'])
    s += winctl(p, w - m - 10 - 36, m + 8, 36, 30, 1)
    return s

def card(p, u, x, y, w, h, idx, label, sub, sel=False, video=False, hover=False):
    s = glass(p, u, x, y, w, h, p['rad'] - 2, glow=sel)
    s += media(p, u, x + 8, y + 8, w - 16, h - 62, idx, p['rad'] - 6, lines=False, clip_id=f'cd{u}{idx}{int(x)}')
    if video:
        s += R(x + w - 46, y + 14, 30, 18, 'rgba(0,0,0,.55)', 5) + f'<path d="M{x+w-34} {y+18}l8 5-8 5z" fill="#fff"/>'
    s += T(x + 14, y + h - 30, label, 13.5, p['text'], 700) + T(x + 14, y + h - 12, sub, 11.5, p['dim'])
    if sel: s += R(x + 1, y + 1, w - 2, h - 2, 'none', p['rad'] - 2, p['acc'], 2)
    return s

PL_NAMES = ["Aurora Dawn", "Violet Tide", "Ocean Glow", "Northern Lights", "Ember Sky", "Mint Haze", "Night Bloom", "Cyan Drift"]

def playlists(key, u):
    p = P(key); w, h = 1100, 720; p['u'] = u
    s = uchrome(p, u, w, h, "Playlists", "Image, video and mixed")
    for i, (lab, wd) in enumerate((("Image", 100), ("Video", 100), ("Mixed", 100))):
        pass
    bx = w - 12 - 10 - 36 - 14
    for i, lab in enumerate(("Mixed", "Video", "Image")):
        bw = 96; bx -= bw + 8 if i else bw
        s += sbtn(p, u, bx, 19, bw, 32, "+ " + lab, 13)
    s += sbtn(p, u, bx - 104, 19, 96, 32, "Import", 13)
    # left list
    s += glass(p, u, 12, 68, 270, h - 80, p['rad'] + 2)
    s += T(32, 100, "My Playlists", 15, p['text'], 700)
    pls = [("Aurora Mix", "Mixed · 24 items", True), ("Night Drive", "Video · 9 items", False), ("Calm Gradients", "Image · 31 items", False), ("Focus Loops", "Video · 6 items", False)]
    for i, (n, t, sel) in enumerate(pls):
        y = 116 + i * 62
        if sel: s += R(24, y, 246, 54, 'rgba(255,255,255,.07)' if p['dark'] else 'rgba(0,0,0,.05)', p['rad'] - 4, p['acc'], 1.4)
        s += T(40, y + 24, n, 14.5, p['text'], 700) + T(40, y + 43, t, 12, p['dim'])
    s += T(32, 400, "Categories", 12, p['dim'], 700, extra='letter-spacing="1"')
    for i, (c, ind) in enumerate((("All", 0), ("Calm", 0), ("Ocean", 1), ("Forest", 1), ("Vivid", 0))):
        s += T(36 + ind * 18, 430 + i * 28, ("▸ " if c in ("Calm", "Vivid") else "") + c if False else c, 14, p['text'] if i == 0 else p['dim'], 700 if i == 0 else 400)
    # right
    X = 298; rw = w - X - 12
    s += T(X + 4, 94, "Aurora Mix", 21, p['text'], 700)
    s += R(X + rw - 270, 72, 270, 32, p['glass'], pill(p)(32)) + R(X + rw - 269.5, 72.5, 269, 31, 'none', pill(p)(32), p['edge'], 1.1) + T(X + rw - 252, 93, "Search items", 13, p['dim'])
    s += sbtn(p, u, X, 116, 120, 32, "Category ▾", 13) + sbtn(p, u, X + 128, 116, 104, 32, "Arrange ▾", 13) + sbtn(p, u, X + 240, 116, 100, 32, "Flow", 13) + sbtn(p, u, X + 348, 116, 120, 32, "Add Folder", 13)
    cw, ch, gap = 178, 190, 14
    cols = 4
    pos = []
    for i, n in enumerate(PL_NAMES):
        cx = X + (i % cols) * (cw + gap); cy = 164 + (i // cols) * (ch + gap)
        pos.append((cx, cy))
        s += card(p, u, cx, cy, cw, ch, i, n, f"#{i+1}  ·  {'Video' if i % 3 == 1 else 'Image'}", sel=(i == 2), video=(i % 3 == 1))
    # animated "Show now" hover sweep
    seq = [0, 1, 2, 3, 7, 6, 5, 4]
    frames = ''.join(f'{k*100/len(seq):.2f}%{{transform:translate({pos[i][0]-pos[0][0]}px,{pos[i][1]-pos[0][1]}px)}}' for k, i in enumerate(seq))
    s = s.replace('</style>', '') if False else s
    cx0, cy0 = pos[0]
    s += f'<style>.hv{u}{{animation:hv{u} 12s infinite steps(1)}}@keyframes hv{u}{{{frames}}}</style>'
    s += f'<g class="hv{u}">' + R(cx0 + 1, cy0 + 1, cw - 2, ch - 2, 'none', p['rad'] - 2, p['acc2'], 2) + R(cx0 + 14, cy0 + 82, 86, 24, 'url(#pb%s)' % u, 12) + T(cx0 + 57, cy0 + 99, "Show now", 12, '#fff', 700, 'middle') + '</g>'
    # footer strip
    fy = h - 12 - 78
    s += glass(p, u, X, fy, rw, 78, p['rad'])
    s += T(X + 20, fy + 30, "Use this playlist", 14, p['text'], 700)
    s += R(X + 160, fy + 14, 44, 24, p['acc'], 12) + f'<circle cx="{X+192}" cy="{fy+26}" r="9" fill="#fff"/>'
    s += T(X + 20, fy + 58, "After Lock → Unlock   ·   After Windows restarts   ·   Every 30 min", 12.5, p['dim'])
    s += pbtn(p, u + 'p', X + rw - 244, fy + 14, 228, 50, "Set as Wallpaper", True, 15)
    return frame(p, u, w, h, s, "Playlists")

def flow(key, u):
    p = P(key); w, h = 1100, 720; p['u'] = u
    s = uchrome(p, u, w, h, "Wallpaper Flow", "Playback order")
    x = 12
    labels = ("Add Media", "Arrange ▾", "Earlier", "Later", "Remove", "Undo", "Redo", "Fit")
    wd = (112, 104, 84, 76, 88, 72, 72, 56)
    cx = 12
    for l, ww in zip(labels, wd):
        s += sbtn(p, u, cx, 68, ww, 32, l, 13); cx += ww + 8
    s += glass(p, u, 12, 112, w - 24, h - 124, p['rad'] + 2)
    pos = [(70, 150), (330, 200), (590, 150), (840, 230), (700, 420), (440, 460), (180, 410), (900, 520)]
    path = ''
    for i in range(len(pos) - 1):
        x1, y1 = pos[i][0] + 70, pos[i][1] + 45; x2, y2 = pos[i + 1][0] + 70, pos[i + 1][1] + 45
        mx = (x1 + x2) / 2
        d = f'M{x1} {y1}Q{mx} {min(y1, y2) - 60} {x2} {y2}'
        s += f'<path d="{d}" fill="none" stroke="{p["acc"]}" stroke-opacity=".35" stroke-width="2.6"/><path class="flowdash" d="{d}" fill="none" stroke="{p["acc2"]}" stroke-width="2.6"/>'
        if i == 0: path = d
    for i, ((px, py), n) in enumerate(zip(pos, PL_NAMES)):
        s += R(px, py + 6, 140, 92, 'rgba(0,0,0,.2)', p['rad'] - 4) + R(px, py, 140, 92, p['glass'] if p['dark'] else 'rgba(255,255,255,.85)', p['rad'] - 4, p['acc'] if i == 2 else p['edge'], 1.6 if i == 2 else 1)
        s += media(p, u, px + 6, py + 6, 128, 54, i, 8, lines=False, clip_id=f'fn{u}{i}')
        s += T(px + 12, py + 80, n, 12, p['text'], 700)
        s += f'<circle cx="{px+16}" cy="{py+16}" r="11" fill="url(#pb{u})"/>' + T(px + 16, py + 21, str(i + 1), 12, '#fff', 700, 'middle')
    s += f'<style>.trav{{offset-path:path("{path}");animation:trv 4s linear infinite}}@keyframes trv{{from{{offset-distance:0%}}to{{offset-distance:100%}}}}</style>'
    s += f'<circle class="trav" r="7" fill="#fff" stroke="{p["acc2"]}" stroke-width="3"/>'
    s += glass(p, u, w - 12 - 330, h - 12 - 52, 318, 40, 20) + T(w - 12 - 330 + 20, h - 12 - 26 + 5, "−", 18, p['text'], 700) + T(w - 12 - 171, h - 12 - 26 + 5, "100%", 13, p['dim'], 700, 'middle') + T(w - 12 - 30, h - 12 - 26 + 5, "+", 18, p['text'], 700) + T(w - 12 - 250, h - 12 - 26 + 5, "Columns  4", 12.5, p['dim'], 400)
    return frame(p, u, w, h, s, "Wallpaper Flow")

def toggle(p, u, x, y, on, anim=None):
    if anim is None:
        s = R(x, y, 44, 24, p['acc'] if on else p['glass'], 12, None if on else p['edge'])
        s += f'<circle cx="{x+(32 if on else 12)}" cy="{y+12}" r="9" fill="#fff"/>'
        return s
    cls, clsb = anim
    s = R(x, y, 44, 24, p['glass'], 12, p['edge']) + R(x, y, 44, 24, p['acc'], 12, extra=f'class="{clsb}"')
    s += f'<g class="{cls}"><circle cx="{x+12}" cy="{y+12}" r="9" fill="#fff"/></g>'
    return s

def settings(key, u):
    p = P(key); w, h = 1000, 700; p['u'] = u
    s = uchrome(p, u, w, h, "Settings", "Preferences")
    s += glass(p, u, 12, 68, 236, h - 80, p['rad'] + 2)
    for i, n in enumerate(("General", "Appearance", "Display and Playback", "Notifications", "Integration")):
        y = 84 + i * 48
        if i == 2:
            s += R(24, y, 212, 38, 'url(#pb%s)' % u, pill(p)(38)) + T(42, y + 25, n, 14, '#fff', 700)
        else: s += T(42, y + 25, n, 14, p['dim'], 600)
    X = 264; cw = w - X - 12
    s += glass(p, u, X, 68, cw, h - 80 - 56, p['rad'] + 2)
    s += T(X + 28, 106, "Display", 17, p['text'], 700)
    def drop(y, label, val):
        r = pill(p)(34)
        return T(X + 28, y + 23, label, 14, p['text']) + R(X + cw - 300, y, 272, 34, p['glass'], r) + R(X + cw - 299.5, y + .5, 271, 33, 'none', r, p['edge'], 1.1) + T(X + cw - 280, y + 23, val, 13.5, p['text'], 600) + f'<path d="M{X+cw-52} {y+14}l6 6 6-6" fill="none" stroke="{p["dim"]}" stroke-width="1.8"/>'
    s += drop(122, "Scaling", "Fill") + drop(170, "Monitor", "All monitors")
    s += f'<path d="M{X+28} 232H{X+cw-28}" stroke="{p["edge"]}"/>' + T(X + 28, 262, "Playback", 17, p['text'], 700)
    s += T(X + 28, 304, "Volume", 14, p['text']) + R(X + cw - 300, 296, 272, 6, p['glass'], 3, p['edge']) + f'<clipPath id="vc{u}"><rect x="{X+cw-300}" y="296" width="272" height="6" rx="3"/></clipPath><g clip-path="url(#vc{u})"><rect class="fill" x="{X+cw-300}" y="296" width="272" height="6" fill="url(#pb{u})"/></g>'
    s += f'<g class="thumb"><circle cx="{X+cw-60}" cy="299" r="10" fill="#fff" stroke="{p["acc"]}" stroke-width="2.4"/></g>'
    rows = (("Mute", ('tg', 'tgb')), ("Loop video", True), ("Play Images on Battery", True), ("Play Videos on Battery", ('tg2', 'tgb2')), ("Start with Windows", True))
    for i, (n, st) in enumerate(rows):
        y = 328 + i * 50
        s += T(X + 28, y + 24, n, 14, p['text'])
        if isinstance(st, tuple): s += toggle(p, u, X + cw - 72, y + 6, False, st)
        else: s += toggle(p, u, X + cw - 72, y + 6, st)
        s += f'<path d="M{X+28} {y+42}H{X+cw-28}" stroke="{p["edge"]}" stroke-opacity=".6"/>'
    fy = h - 12 - 44
    s += T(X + 4, fy + 28, "Settings layout:  Tabs · Sidebar · Grouped page", 12.5, p['dim']) + sbtn(p, u, w - 12 - 112, fy, 112, 38, "Close", 14)
    return frame(p, u, w, h, s, "Settings")

# ------------------------------------------------------------------ themes
def themes(u="th"):
    p = P("da"); w, h = 1100, 720; p['u'] = u
    s = uchrome(p, u, w, h, "Customize Theme", "Four themes, your colors")
    sc = .34; mw, mh = W * sc, H * sc
    keys = ("da", "la", "do", "lo")
    pos = [(30, 84), (30 + mw + 24, 84), (30, 84 + mh + 56), (30 + mw + 24, 84 + mh + 56)]
    for k, (x, y) in zip(keys, pos):
        q = P(k); uu = 'm' + k
        inner = f'<clipPath id="mc{uu}"><rect width="{W}" height="{H}" rx="14"/></clipPath><g clip-path="url(#mc{uu})">' + bgfill(q, uu, W, H) + main_body(q, uu) + '</g>'
        q['u'] = uu
        s += f'<g transform="translate({x} {y}) scale({sc})">' + gdefs(q, uu) + thumbdefs(uu) + inner + f'<rect x="1" y="1" width="{W-2}" height="{H-2}" rx="14" fill="none" stroke="{q["edge"]}" stroke-width="3"/></g>'
        s += T(x + mw / 2, y + mh + 26, q['name'], 15, p['text'], 700, 'middle')
    # selection ring cycling through the four themes
    fr = ''.join(f'{i*25}%{{transform:translate({pos[i][0]-pos[0][0]}px,{pos[i][1]-pos[0][1]}px)}}' for i in range(4))
    s += f'<style>.ring{u}{{animation:rg{u} 10s infinite steps(1)}}@keyframes rg{u}{{{fr}}}</style>'
    s += f'<g class="ring{u}">' + R(pos[0][0] - 6, pos[0][1] - 6, mw + 12, mh + 12, 'none', 16, p['acc2'], 3) + '</g>'
    ex = 30 + 2 * mw + 48 + 8; ew = w - ex - 12
    s += glass(p, u, ex, 68, ew, h - 80, p['rad'] + 2)
    s += T(ex + 22, 102, "Colors", 16, p['text'], 700) + T(ex + 22, 122, "Dark Aurora · overrides are saved per theme", 11.5, p['dim'])
    rows = (("Accent", p['acc']), ("Window background", p['win']), ("Panel", '#242426'), ("Text", p['text']), ("Border", '#3a3a3a'))
    for i, (n, c) in enumerate(rows):
        y = 142 + i * 46
        s += T(ex + 22, y + 22, n, 14, p['text']) + R(ex + ew - 130, y + 2, 34, 26, c, 7, p['edge']) + T(ex + ew - 88, y + 21, c, 12, p['dim'])
    s += T(ex + 22, 400, "Accent presets", 12, p['dim'], 700)
    for i, c in enumerate(("#3fae5c", "#35b8e0", "#8a6bff", "#ff7a59", "#f2c94c", "#e04f8a")):
        s += f'<circle cx="{ex+36+i*38}" cy="432" r="13" fill="{c}" stroke="{p["text"] if i == 0 else p["edge"]}" stroke-width="{2.4 if i == 0 else 1}"/>'
    s += sbtn(p, u, ex + 22, 470, ew - 44, 36, "Fix 2 low-contrast colors", 13)
    s += T(ex + 22, 540, "Contrast  ", 12, p['dim'], 700) + R(ex + 92, 530, ew - 124, 8, p['glass'], 4, p['edge']) + R(ex + 92, 530, (ew - 124) * .8, 8, 'url(#pb%s)' % u, 4) + T(ex + ew - 22, 540, "AA", 12, p['acc'], 700, 'end')
    s += sbtn(p, u, ex + 22, h - 12 - 76, 112, 38, "Reset", 14) + pbtn(p, u + 'x', ex + 146, h - 12 - 76, ew - 168, 38, "Apply", False, 15)
    return frame(p, u, w, h, s, "Customize Theme")

files = {
 "v2-main-darkaurora.svg": main_win("da", "a"), "v2-main-lightaurora.svg": main_win("la", "b"),
 "v2-main-darkonyx.svg": main_win("do", "c"), "v2-main-lightonyx.svg": main_win("lo", "d"),
 "v2-playlists.svg": playlists("da", "e"), "v2-flow.svg": flow("da", "f"),
 "v2-themes.svg": themes(), "v2-settings.svg": settings("da", "g"),
}
for n, c in files.items():
    open(os.path.join(OUT, n), "w", encoding="utf-8").write(c)
print("wrote", len(files))

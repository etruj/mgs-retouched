#!/usr/bin/env python3
"""Launcher icon for METAL GEAR SOLID RETOUCHED, made at build time from the game's own iPad icon
(IconPad.png, Apple CgBI format) in the user's bundle: enlarged to 144 px with a green Re-Touched
stripe. Standard library only.  usage: make_icon.py BUNDLE_DIR OUT.png"""
import struct, sys, zlib

def read_png(path):
    d = open(path, 'rb').read(); o = 8; cgbi = False; idat = b''; w = h = 0; ctype = 6
    while o < len(d):
        n, t = struct.unpack('>I4s', d[o:o + 8]); body = d[o + 8:o + 8 + n]; o += 12 + n
        if t == b'CgBI': cgbi = True
        elif t == b'IHDR': w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif t == b'IDAT': idat += body
    raw = zlib.decompress(idat, -15) if cgbi else zlib.decompress(idat)
    bpp = 4 if ctype == 6 else 3; stride = w * bpp; out = bytearray(); prev = bytearray(stride); i = 0
    for _ in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line; prev = line
    px = []
    for k in range(w * h):
        q = out[k * bpp:(k + 1) * bpp]
        r, g, b, a = (q[2], q[1], q[0], q[3] if bpp == 4 else 255) if cgbi else (q[0], q[1], q[2], q[3] if bpp == 4 else 255)
        if cgbi and 0 < a < 255: r, g, b = (min(255, v * 255 // a) for v in (r, g, b))   # CgBI is premultiplied
        px.append((r, g, b, a))
    return w, h, px

def write_png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(c for p in px[y * w:(y + 1) * w] for c in p) for y in range(h))
    chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

w, h, px = read_png(sys.argv[1] + '/IconPad.png')
N = 144; out = []
for y in range(N):
    for x in range(N):
        fx, fy = (x + 0.5) * w / N - 0.5, (y + 0.5) * h / N - 0.5
        x0, y0 = max(0, int(fx)), max(0, int(fy)); x1, y1 = min(w - 1, x0 + 1), min(h - 1, y0 + 1); tx, ty = fx - x0, fy - y0
        c = [sum(px[yy * w + xx][k] * wt for (xx, yy, wt) in ((x0, y0, (1 - tx) * (1 - ty)), (x1, y0, tx * (1 - ty)),
             (x0, y1, (1 - tx) * ty), (x1, y1, tx * ty))) for k in range(4)]
        if y >= N - 22: c = [c[0] * 0.25 + 140 * 0.75, c[1] * 0.25 + 224 * 0.75, c[2] * 0.25 + 90 * 0.75, 255]   # the green stripe
        out.append(tuple(max(0, min(255, int(v + 0.5))) for v in c))
write_png(sys.argv[2], N, N, out)

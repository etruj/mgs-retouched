#!/usr/bin/env python3
"""Extract the app bundle (Payload/*.app) from your own .ipa: extract_ipa.py GAME.ipa OUT_DIR
Produces OUT_DIR/MGSTUS.app. (An .ipa is a zip; only the bundle's files are needed.)"""
import sys, os, zipfile, shutil
ipa, out = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(ipa)
apps = sorted({n.split('/')[1] for n in z.namelist() if n.startswith('Payload/') and n.split('/')[1].endswith('.app')})
if not apps: sys.exit(f'{ipa}: no Payload/*.app inside - is this an .ipa?')
app = apps[0]; dst = os.path.join(out, 'MGSTUS.app')
if os.path.exists(dst): shutil.rmtree(dst)
n = 0
for info in z.infolist():
    parts = info.filename.split('/')
    if len(parts) < 3 or parts[0] != 'Payload' or parts[1] != app or info.is_dir(): continue
    rel = '/'.join(parts[2:])
    if rel.startswith('_CodeSignature/') or rel == 'SC_Info' or rel.startswith('SC_Info/'): continue
    p = os.path.join(dst, rel); os.makedirs(os.path.dirname(p), exist_ok=True)
    with z.open(info) as src, open(p, 'wb') as f: shutil.copyfileobj(src, f)
    n += 1
print(f'extracted {n} files from {app} -> {dst}')

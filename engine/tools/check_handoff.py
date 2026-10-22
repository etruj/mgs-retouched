#!/usr/bin/env python3
"""Check that a decompiler output folder (the "handoff") is complete and is the build this
port was verified against. Usage: check_handoff.py HANDOFF_DIR [--assets DIR]"""
import hashlib, os, sys, json
MGSTUS_SHA256 = '07ac5db07fb8909a95b23c791d20800237bf14a51de1cec32cd0be8e37773a9c'     # MGSTUS v2.3.0, cryptid 0 (armv6)
FUNCTIONS = 1395
REQUIRED = ['index.tsv', 'src', 'include/mgst_types.fixed.h', 'include/ghidra_ops.h', 'tables/symbols.tsv',
            'tables/functions.jsonl', 'tables/extslots.tsv', 'binary/MGSTUS']
def fail(msg): print(f'handoff check FAILED: {msg}'); sys.exit(1)
if len(sys.argv) < 2: fail('usage: check_handoff.py HANDOFF_DIR [--assets DIR]')
H = sys.argv[1]
assets = sys.argv[sys.argv.index('--assets') + 1] if '--assets' in sys.argv else os.path.join(H, 'assets/MGSTUS.app')
for r in REQUIRED:
    if not os.path.exists(os.path.join(H, r)): fail(f'{r} missing in {H}: is this the output of make_port_handoff.py?')
d = open(os.path.join(H, 'binary/MGSTUS'), 'rb').read()
if hashlib.sha256(d).hexdigest() != MGSTUS_SHA256:
    fail('binary/MGSTUS is not MGS Touch v2.3.0 (the only version this port supports): '
         'the runtime and the decompilation fixes are specific to it')
rows = sum(1 for _ in open(os.path.join(H, 'index.tsv'))) - 1
if rows != FUNCTIONS: fail(f'index.tsv lists {rows} functions, expected {FUNCTIONS}')
missing = [l.split('\t')[6] for l in list(open(os.path.join(H, 'index.tsv')))[1:] if not os.path.exists(os.path.join(H, l.rstrip('\n').split('\t')[6]))]
if missing: fail(f'{len(missing)} function files listed in index.tsv are missing, e.g. {missing[0]}')
if not os.path.isfile(os.path.join(assets, 'Info.plist')) or not os.path.isfile(os.path.join(assets, 'loading.png')):
    fail(f'game assets not found at {assets} (give --ipa YOUR.ipa, or a handoff that includes assets/MGSTUS.app)')
print(f'handoff OK: MGS Touch v2.3.0, {rows} functions, assets at {assets}')

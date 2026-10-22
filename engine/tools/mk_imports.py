#!/usr/bin/env python3
"""List every C function the game imports (not defined in __text), with the prototype the
verified C declares for it. Input: the decompiler handoff. Output: imports.txt, one
prototype per line (used by mk_autostubs.py). Fails if two files disagree on a prototype."""
import json, re, glob, os, sys, collections
H = os.environ.get('MGST_HANDOFF') or os.path.join(os.path.dirname(__file__), '..', '..', 'PORT_HANDOFF')
out = sys.argv[1]
TEXT_END = 0x5a614
game = {json.loads(l)['cname'] for l in open(f'{H}/tables/functions.jsonl') if int(json.loads(l)['addr'], 16) < TEXT_END}
protos = collections.defaultdict(collections.Counter)
for f in glob.glob(f'{H}/src/*/*.c'):
    for line in open(f):
        m = re.match(r'^([A-Za-z_][\w \*]*?[\s\*])(_?\w+)\((.*)\);\s*$', line)
        if m and not line.startswith(('extern', '#', 'return')) and m.group(2) not in game:
            protos[m.group(2)][line.strip()] += 1
bad = [n for n, c in protos.items() if len(c) > 1]
if bad: sys.exit(f'mk_imports: inconsistent prototypes for {bad}')
open(out, 'w').write('\n'.join(sorted(c.most_common(1)[0][0] for c in protos.values())) + '\n')
print(f'imports: {len(protos)} functions')

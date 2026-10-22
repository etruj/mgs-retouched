#!/usr/bin/env python3
"""Mechanical build-time transform of the verified C for the wasm32 build.

The originals in PORT_HANDOFF/src are never edited. For each function file this emits
build/gen/game/<addr>_<name>.c where:
  * comments are stripped (they are informational only and confuse call parsing);
  * every USED `extern T name;` global becomes `(*(T *)0xADDR)`, its original address
    in the image, which is loaded at the same addresses in wasm memory;
  * `REC_name` definitions are renamed to `name`, which is what callers use;
  * every objc_msgSend / _stret / Super2 call site goes through a generated thunk
    MSG_<n> that packs the arguments into 32-bit words exactly as armv6 APCS softfp
    passed them (floats as bits, 64-bit values as two words) and dispatches through
    the runtime. This sidesteps every prototype disagreement between call sites and
    implementations: under softfp they were all just words.
  * calls to variadic C imports (NSLog, printf) pass their extra arguments as words
    the same way.
Also emits build/gen/msg_thunks.h and build/gen/invoke_orig.c (original address ->
function, unpacking words with the callee's own prototype).
"""
import os, re, sys, json, collections

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
H = os.environ.get('MGST_HANDOFF') or os.path.join(ROOT, 'PORT_HANDOFF')
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/gen')
os.makedirs(os.path.join(OUT, 'game'), exist_ok=True)

# entry/dyld/crt0 glue and switch helpers: replaced by the platform (docs/V0_AUDIT.md)
EXCLUDE = {'entry', 'dyld_stub_binding_helper', '_main', '___switchu8', '___switch8', '___switch16'}
TEXT_END = 0x5a614

funcs = {}            # cname -> record
for line in open(f'{H}/tables/functions.jsonl'):
    r = json.loads(line)
    if int(r['addr'], 16) < TEXT_END: funcs[r['cname']] = r
symaddr = collections.defaultdict(list)
for line in open(f'{H}/tables/symbols.tsv'):
    p = line.rstrip('\n').split('\t')
    if len(p) >= 4 and p[3] == 'data': symaddr[p[1]].append(int(p[2], 16))

# ---------------------------------------------------------------- C text helpers
def strip_comments(s):
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if s.startswith('/*', i):
            j = s.find('*/', i + 2); seg = s[i:j + 2]
            out.append(' ' + '\n' * seg.count('\n')); i = j + 2
        elif s.startswith('//', i):
            j = s.find('\n', i); i = n if j < 0 else j
        elif c in '"\'':
            j = i + 1
            while s[j] != c: j += 2 if s[j] == '\\' else 1
            out.append(s[i:j + 1]); i = j + 1
        else: out.append(c); i += 1
    return ''.join(out)

def code_spans(s):
    """Yield (start, end) of regions outside string/char literals."""
    i, start, n = 0, 0, len(s)
    while i < n:
        if s[i] in '"\'':
            yield start, i
            q = s[i]; j = i + 1
            while s[j] != q: j += 2 if s[j] == '\\' else 1
            i = start = j + 1
        else: i += 1
    yield start, n

def matching_paren(s, i):
    """s[i] == '(' ; return index of its ')'."""
    d = 0
    while True:
        c = s[i]
        if c in '"\'':
            j = i + 1
            while s[j] != c: j += 2 if s[j] == '\\' else 1
            i = j
        elif c == '(': d += 1
        elif c == ')':
            d -= 1
            if d == 0: return i
        i += 1

def split_args(s):
    out, d, cur, i = [], 0, '', 0
    while i < len(s):
        c = s[i]
        if c in '"\'':
            j = i + 1
            while s[j] != c: j += 2 if s[j] == '\\' else 1
            cur += s[i:j + 1]; i = j + 1; continue
        if c in '([{': d += 1
        elif c in ')]}': d -= 1
        if c == ',' and d == 0: out.append(cur.strip()); cur = ''
        else: cur += c
        i += 1
    if cur.strip(): out.append(cur.strip())
    return out

def proto_parts(p):
    p = re.sub(r'\b__(?:thiscall|cdecl|stdcall|fastcall)\b', '', p)
    m = re.match(r'\s*(.*?)\s*\b([A-Za-z_][\w:]*)\s*\((.*)\)\s*;?\s*$', p, re.S)
    ret, name, args = m.group(1), m.group(2), m.group(3).strip()
    params = [] if args in ('', 'void') else split_args(args)
    return ret.strip(), name, params

def param_type(p):
    """'float x' -> 'float', 'void * * name' -> 'void * *', '...' -> '...'"""
    p = p.strip()
    if p == '...': return p
    m = re.match(r'^(.*?)(\b[A-Za-z_]\w*)\s*((?:\[\d*\])*)$', p)
    if m and m.group(1).strip() and not re.fullmatch(r'(unsigned|signed|long|short|const)', m.group(2)):
        t = m.group(1).strip()
        return t + ' *' if m.group(3) else t
    return p

# ---------------------------------------------------------------- msgSend thunks
CAST = re.compile(r'\(\(\s*(?P<rt>[^()]*?)\s*\(\*\)\((?P<args>[^()]*)\)\)\s*(?P<fn>_objc_msgSend(?:_stret|Super2)?)\s*\)\s*\(')
KIND = {'_objc_msgSend': 'MSGK_NORMAL', '_objc_msgSend_stret': 'MSGK_STRET', '_objc_msgSendSuper2': 'MSGK_SUPER2'}
sigs = {}             # (rt, (types...), fn, variadic) -> id
def sig_id(rt, types, fn, variadic):
    k = (rt, tuple(types), fn, variadic)
    if k not in sigs: sigs[k] = len(sigs)
    return sigs[k]

def push_words(args):
    return ''.join(f' MSG_PUSHV(__vw, __vn, {a});' for a in args)

def rewrite_msgsend(s):
    # uncast calls through the variadic prototype: treat every argument as a word
    def uncast(m):
        return m.group(0)
    out, pos = [], 0
    while True:
        m = CAST.search(s, pos)
        u = re.compile(r'(?<![\w)])(_objc_msgSend(?:_stret|Super2)?)\s*\(').search(s, pos)
        if u and (not m or u.start() < m.start()) and not s[max(0, u.start() - 1)] == ')':
            # bare call: _objc_msgSend(a, b, c...)
            op = u.end() - 1; cl = matching_paren(s, op)
            args = split_args(s[op + 1:cl])
            fixed = 3 if u.group(1) == '_objc_msgSend_stret' else 2
            types = ['void *'] * fixed
            i = sig_id('void *', types, u.group(1), True)
            out.append(s[pos:u.start()])
            out.append(f'({{ unsigned __vw[32] = {{0}}; int __vn = 0;{push_words(args[fixed:])} '
                       f'MSGV_{i}({", ".join(args[:fixed])}, __vw, __vn); }})')
            pos = cl + 1; continue
        if not m: break
        rt, fn = m['rt'].strip(), m['fn']
        types = [t.strip() for t in m['args'].split(',')]
        op = m.end() - 1; cl = matching_paren(s, op)
        args = split_args(s[op + 1:cl])
        out.append(s[pos:m.start()])
        if types[-1] == '...':
            fixed = types[:-1]; i = sig_id(rt, fixed, fn, True)
            out.append(f'({{ unsigned __vw[32] = {{0}}; int __vn = 0;{push_words(args[len(fixed):])} '
                       f'MSGV_{i}({", ".join(args[:len(fixed)])}, __vw, __vn); }})')
        else:
            i = sig_id(rt, types, fn, False)
            out.append(f'MSG_{i}({", ".join(args)})')
        pos = cl + 1
    out.append(s[pos:])
    return ''.join(out)


def rewrite_indirect_calls(s):
    """(*EXPR)(args): a call through a function pointer. C++ vtables in the image hold
    original code addresses, which are not wasm function pointers; ICALL() maps them."""
    out, pos = [], 0
    while True:
        i = s.find('(*', pos)
        if i < 0: break
        j = matching_paren(s, i)
        if s[j + 1:j + 2] == '(':
            out.append(s[pos:i]); out.append('(*ICALL(' + s[i + 2:j] + '))'); pos = j + 1
        else:
            out.append(s[pos:i + 2]); pos = i + 2
    out.append(s[pos:])
    return ''.join(out)

VARIADIC_IMPORTS = {}  # name -> fixed param types
def rewrite_variadic_imports(s, decls):
    for name, fixed in decls.items():
        pat = re.compile(r'(?<![\w>.])' + re.escape(name) + r'\s*\(')
        out, pos = [], 0
        while True:
            m = pat.search(s, pos)
            if not m: break
            # skip the prototype itself
            line_start = s.rfind('\n', 0, m.start()) + 1
            if re.match(r'^[A-Za-z_][\w \*]*[\s\*]' + re.escape(name) + r'\s*\(', s[line_start:m.end()]) and s[line_start:m.start()].strip():
                out.append(s[pos:m.end()]); pos = m.end(); continue
            op = m.end() - 1; cl = matching_paren(s, op)
            args = split_args(s[op + 1:cl]); k = len(fixed)
            out.append(s[pos:m.start()])
            out.append(f'({{ unsigned __vw[32] = {{0}}; int __vn = 0;{push_words(args[k:])} '
                       f'{name}__w({", ".join(args[:k] + ["__vw", "__vn"])}); }})')
            pos = cl + 1
        out.append(s[pos:]); s = ''.join(out)
    return s


# ---------------------------------------------------------------- known decompilation defects
# Each entry fixes a defect in the verified C that its differential tests could not see
# (all are in V0 / synthetic-only functions), with the evidence. Applied to the source
# text after comments are stripped. A defect pattern that turns up anywhere unlisted
# stops the build.
DEFECTS = [
    # SoundService::init (V0, synthetic tests only) constructs its Sound ports in a loop.
    # Ghidra models struct Sound as 0x39 bytes (no tail padding) and writes the stride as
    # &p[1] + 3 = 0x3c. Compiled as C, sizeof(Sound) is already 0x3c (4-byte alignment), so
    # the expression steps 0x3f: ports 2..n are constructed at wrong offsets and read as busy
    # garbage ("No port can be used (null port)": no music ever plays). The binary steps 0x3c.
    ('0x21b44_SoundService_init.c', '(Sound *)((int)&this_01[1].field0_0x0 + 3)', '(Sound *)((int)this_01 + 0x3c)'),
]
# Functions whose verified C returns the *bits* of a float as undefined4 (on armv6 softfp a
# float return is just r0). A caller that writes (float)f() converts the integer instead of
# reinterpreting it: masterVolume() = 0x3f800000 became 1.065e9, so music played ~1e27x too
# loud. All sites are in V0 functions (Sound::ready, SoundService::init), outside the
# differential tests. The binary moves r0 to a VFP register unchanged: __BITCAST is exact.
FLOAT_BITS_RETURNERS = {'SoundManager__masterVolume', 'SoundManager__categoryVolume',
                        'SoundResourceManager__attributeVolume'}
def fix_float_bit_returns(fname, s):
    out, pos = [], 0
    for m in re.finditer(r'\(float\)\s*([A-Za-z_]\w*)\s*\(', s):
        name = m.group(1)
        if m.start() < pos: continue
        if name in FLOAT_BITS_RETURNERS:
            op = m.end() - 1; cl = matching_paren(s, op)
            out.append(s[pos:m.start()]); out.append(f'__BITCAST(float, {name}({s[op + 1:cl]}))'); pos = cl + 1
        elif name in funcs and re.match(r'\s*(undefined4|uint|int)\b', funcs[name]['proto']):
            sys.exit(f'{fname}: (float){name}() converts an integer-typed return: check the callee, then list it in FLOAT_BITS_RETURNERS')
    out.append(s[pos:]); return ''.join(out)

DEFECT_SIGNATURE = re.compile(r'\(int\)&[A-Za-z_]\w*\[1\]\.\w+ \+ (?:0x)?[0-9a-f]+\)')

def apply_defect_patches(fname, s):
    for f, old, new in DEFECTS:
        if fname == f:
            if old in s: s = s.replace(old, new)
            elif not DEFECT_SIGNATURE.search(s):
                print(f'note: {f}: decompilation defect already fixed upstream, no patch needed')
            else: sys.exit(f'defect patch for {f} no longer matches: re-check the handoff')
    for m in DEFECT_SIGNATURE.finditer(s):
        sys.exit(f'{fname}: unpadded-struct stride pattern {m.group(0)!r}: add a DEFECTS entry after checking the binary')
    return s

# ---------------------------------------------------------------- per file
EXTERN = re.compile(r'^extern\s+(?P<t>[^;()]*?[\s\*])(?P<n>[A-Za-z_]\w*)\s*(?P<dim>(?:\[\d*\])*)\s*;\s*$', re.M)
stats = collections.Counter(); ambiguous = []; unresolved = set()
index = [l.rstrip('\n').split('\t') for l in open(f'{H}/index.tsv')][1:]
written = []
for row in index:
    addr, fname, path = row[0], row[1], row[6]
    rec = next((r for r in funcs.values() if r['addr'] == addr), None)
    if rec is None or rec['cname'] in EXCLUDE: stats['excluded'] += 1; continue
    s = strip_comments(open(os.path.join(H, path)).read())
    s = apply_defect_patches(os.path.basename(path), s)
    s = fix_float_bit_returns(os.path.basename(path), s)
    # externs
    externs = {}
    for m in EXTERN.finditer(s):
        externs[m['n']] = (m['t'].strip(), m['dim'])
    s = EXTERN.sub(lambda m: '', s)
    # declarations of variadic C imports in this file
    vdecl = {}
    for m in re.finditer(r'^([A-Za-z_][\w \*]*?[\s\*])(_?\w+)\((.*\.\.\.)\);\s*$', s, re.M):
        if m.group(2).startswith('_objc_msgSend') or m.group(2) in funcs: continue
        vdecl[m.group(2)] = [param_type(p) for p in split_args(m.group(3))[:-1]]
        VARIADIC_IMPORTS[m.group(2)] = (m.group(1).strip(), vdecl[m.group(2)])
    # names declared by the function itself (parameters, Ghidra's top-of-body locals,
    # frame macros) shadow file-scope externs, exactly as C scoping would
    local = set()
    hm = re.search(r'\b' + re.escape('REC_' + rec['cname']) + r'\s*\(', s)
    if hm:
        cl = matching_paren(s, hm.end() - 1)
        for prm in split_args(s[hm.end():cl]):
            mm = re.search(r'([A-Za-z_]\w*)\s*(?:\[\d*\])*\s*$', prm)
            if mm: local.add(mm.group(1))
    if hm:
        # Ghidra emits all locals at the top of the body, before the first blank line
        body_start = s.index('{', cl) + 1
        for ln in s[body_start:].split('\n')[1:]:
            if not ln.strip(): break
            mm = re.match(r'^\s+(?!return\b|goto\b)(?:[A-Za-z_][\w]*[\s\*]+)+?\**\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?:__attribute__\(\(.*\)\))?\s*;\s*$', ln)
            if not mm: break
            local.add(mm.group(1))
    for mm in re.finditer(r'^#define\s+([A-Za-z_]\w*)', s, re.M): local.add(mm.group(1))
    local |= set(re.findall(r'\bgoto\s+([A-Za-z_]\w*)', s))          # labels are never globals
    local |= set(re.findall(r'^\s*([A-Za-z_]\w*)\s*:(?!:)', s, re.M))
    # resolve used externs
    used = set()
    pieces = []; last = 0
    ident = re.compile(r'(?<![\w.])(?<!->)\b([A-Za-z_]\w*)\b')
    def rep(m):
        n = m.group(1)
        if n not in externs or n in local: return n
        t, dim = externs[n]
        addrs = symaddr.get(n, [])
        am = re.fullmatch(r'(?:DAT|PTR|UNK|BYTE|WORD|DWORD|QWORD|FLOAT|DOUBLE|SUB|LAB|switchD|PTR_LOOP|s|u|cf|global)_.*?([0-9a-fA-F]{5,8})', n) \
            or re.fullmatch(r'.*__([0-9a-f]{5})', n)
        if len(set(addrs)) > 1:
            in_data = sorted({x for x in addrs if 0x69000 <= x < 0x89000})
            if len(in_data) == 1: addrs = in_data   # a variable lives in __DATA, not __text
        if (not addrs or len(set(addrs)) > 1) and am:
            addrs = [int(am.group(1), 16)]
        if not addrs:
            unresolved.add(n); return n
        if len(set(addrs)) > 1:
            ambiguous.append((fname, n, [hex(x) for x in addrs])); return n
        used.add(n)
        if dim.startswith('[]'): return f'(({t} (*){dim[2:]})0x{addrs[0]:x})'   # unsized array: decays to a pointer
        if dim: return f'(*({t} (*){dim})0x{addrs[0]:x})'
        return f'(*({t} *)0x{addrs[0]:x})'
    # macro bodies above the definition (binding-slot #defines) are code too
    s = re.sub(r'^(#define\s+\w+\s+)(.*)$', lambda m: m.group(1) + ident.sub(rep, m.group(2)), s, flags=re.M)
    hm = re.search(r'\b' + re.escape('REC_' + rec['cname']) + r'\s*\(', s)   # offsets moved: re-find
    body_from = s.index('{', matching_paren(s, hm.end() - 1)) if hm else 0
    for a, b in code_spans(s):
        if b <= body_from: continue       # prototypes before the definition: parameter names only
        a = max(a, body_from)
        seg = s[a:b]
        pieces.append(s[last:a]); pieces.append(ident.sub(rep, seg)); last = b
    pieces.append(s[last:]); s = ''.join(pieces)
    stats["globals"] += len(used)
    # REC_ rename
    s = s.replace('REC_' + rec['cname'], rec['cname'])
    s = re.sub(r'^[^\n;]*\b_objc_msgSend(?:_stret|Super2)?\s*\([^;{]*\.\.\.\s*\)\s*;[ \t]*$', '', s, flags=re.M)
    s = rewrite_msgsend(s)
    s = rewrite_variadic_imports(s, vdecl)
    s = rewrite_indirect_calls(s)
    s = '#include "port_game.h"\n' + s
    out = os.path.join(OUT, 'game', os.path.basename(path))
    open(out, 'w').write(s); written.append(out); stats['files'] += 1

# ---------------------------------------------------------------- thunks header
T = ['/* generated by engine/tools/xform.py - do not edit */', '#ifndef MSG_THUNKS_H', '#define MSG_THUNKS_H', '']
for (rt, types, fn, variadic), i in sorted(sigs.items(), key=lambda kv: kv[1]):
    params = ', '.join(f'{t} a{j}' for j, t in enumerate(types))
    if variadic: params += ', const unsigned *xw, int xn'
    body = ['unsigned __w[48] = {0}; int __n = 0;']
    body += [f'MSG_PUT(__w, __n, a{j});' for j in range(len(types))]
    if variadic: body.append('for (int __k = 0; __k < xn; __k++) __w[__n++] = xw[__k];')
    body.append(f'unsigned long long __r = objc_msgSend_w(__w, __n, {KIND[fn]});')
    if rt == 'void': body.append('(void)__r;')
    else: body.append(f'{rt} __v; __builtin_memcpy(&__v, &__r, sizeof __v); return __v;')
    name = f'MSGV_{i}' if variadic else f'MSG_{i}'
    T.append(f'static inline {rt} {name}({params}) {{ {" ".join(body)} }}')
T += ['', '#endif']
open(os.path.join(OUT, 'msg_thunks.h'), 'w').write('\n'.join(T) + '\n')

# variadic import prototypes (implemented by the runtime with a word array)
V = ['/* generated by engine/tools/xform.py - do not edit */', '#ifndef VARIADIC_IMPORTS_H', '#define VARIADIC_IMPORTS_H']
for n, (rt, fixed) in sorted(VARIADIC_IMPORTS.items()):
    V.append(f'{rt} {n}__w({", ".join([f"{t} a{j}" for j, t in enumerate(fixed)] + ["const unsigned *w", "int n"])});')
V.append('#endif')
open(os.path.join(OUT, 'variadic_imports.h'), 'w').write('\n'.join(V) + '\n')


# ---------------------------------------------------------------- typed indirect calls
# A call through a function pointer has the pointer's C type; the function it reaches (a C++
# vtable entry in the image) has its own. On armv6 softfp both were just registers, so the
# decompiler may type the two differently (the newer exports type every SoundAPI slot as
# returning `undefined`). WebAssembly checks indirect-call signatures exactly. So: every
# function-pointer type in the header gets an id (_Generic at the call site), and for every
# function stored in the image's data, an adapter per type packs the arguments into APCS words
# and calls it through invoke_orig() with its own prototype, then converts the result to the
# caller's type, exactly what the shared registers did.
sys.path.insert(0, os.path.dirname(__file__))
from macho import MachO
_hdr = open(os.path.join(OUT, 'include/mgst_types.fixed.h')).read()
# vtable slots: the decompiler marks them "/* slot N (+0xNN): Class::method ... */"
_FP_FIELD = re.compile(r'^\s+([A-Za-z_][\w \*]*?)\s*\(\*\s*\w+\)\s*\(([^()]*)\)\s*;\s*/\*\s*slot\b', re.M)
fptypes = []
# canonical spelling, so that compatible types (Ghidra's typedef aliases, `X` vs `struct X`)
# appear once in the _Generic list
_ALIAS = {'undefined': 'unsigned char', 'undefined1': 'unsigned char', 'uchar': 'unsigned char', 'byte': 'unsigned char',
          '_gbool': 'unsigned char', 'bool': 'unsigned char', 'undefined2': 'unsigned short', 'ushort': 'unsigned short',
          'word': 'unsigned short', 'undefined4': 'unsigned int', 'uint': 'unsigned int', 'dword': 'unsigned int',
          'ulong': 'unsigned long', 'longlong': 'long long', 'ulonglong': 'unsigned long long', 'undefined8': 'unsigned long long',
          'qword': 'unsigned long long'}
_STRUCTS = set(re.findall(r'^typedef struct (\w+) \1\b', _hdr, re.M))
def _norm(t):
    t = re.sub(r'\s+', ' ', t.strip())
    t = re.sub(r'\bstruct (\w+)', r'\1', t)
    t = re.sub(r'\b\w+\b', lambda m: _ALIAS.get(m.group(0), ('struct ' + m.group(0)) if m.group(0) in _STRUCTS else m.group(0)), t)
    return t
# plus the decompiler's typedefs for function-pointer variables (NAME_proc_t), which the
# runtime's own functions reach (alcGetProcAddress)
_FP_PROC = re.compile(r'^typedef\s+([A-Za-z_][\w \*]*?)\s+\w+_proc_t\s*\(([^()]*)\)\s*;', re.M)
for m_ in list(_FP_FIELD.finditer(_hdr)) + list(_FP_PROC.finditer(_hdr)):
    rt_, params = _norm(m_.group(1)), _norm(m_.group(2)) or 'void'
    key = (rt_, params)
    if key not in fptypes: fptypes.append(key)
_m = MachO(os.path.join(H, 'binary/MGSTUS'))
_byaddr = {int(r['addr'], 16): c for c, r in funcs.items()}
vtfuncs = []
for _sec in ('__const', '__data'):
    _sc = _m.section('__DATA', _sec)
    if not _sc: continue
    for _a in range(_sc.addr, _sc.addr + _sc.size, 4):
        _v = _m.u32(_a) & ~1
        if _v in _byaddr and _byaddr[_v] not in EXCLUDE and _v not in [x[0] for x in vtfuncs]: vtfuncs.append((_v, _byaddr[_v]))
T = ['/* generated by engine/tools/xform.py: ids of the function-pointer types in the header (typed indirect calls) */',
     '#ifndef ICALL_TYPES_H', '#define ICALL_TYPES_H', '#define ICALL_TID(p) _Generic((p), \\']
for k, (rt_, params) in enumerate(fptypes):
    T.append(f'    {rt_} (*)({params}): {k + 1}, \\')
T += ['    default: 0)', '#endif']
open(os.path.join(OUT, 'icall_types.h'), 'w').write('\n'.join(T) + '\n')
# runtime functions handed to the game as pointers, at reserved addresses (engine/rt/audio.c)
RUNTIME_FNS = [(0x5a614, 'alBufferDataStatic')]
ICALL_ADAPTERS = []
for addr, cname in vtfuncs + RUNTIME_FNS:
    for k, (rt_, params) in enumerate(fptypes):
        plist = [] if params == 'void' else [p_.strip() for p_ in split_args(params)]
        if '...' in plist: continue
        decl = ', '.join(f'{t} a{j}' for j, t in enumerate(plist)) or 'void'
        body = ' '.join(f'MSG_PUT(w, n, a{j});' for j in range(len(plist)))
        ret = '(void)r;' if rt_ == 'void' else f'{rt_} v; __builtin_memcpy(&v, &r, sizeof v); return v;'
        callee = 'rt_runtime_call' if (addr, cname) in RUNTIME_FNS else 'invoke_orig'
        ICALL_ADAPTERS.append((addr, k + 1, f'static {rt_} icall_{addr:x}_{k + 1}({decl}) {{ unsigned w[32] = {{0}}; int n = 0; {body} unsigned long long r = {callee}(0x{addr:x}u, w); {ret} }}'))

# ---------------------------------------------------------------- invoke_orig
I = ['/* generated by engine/tools/xform.py: original code address -> recompiled function. */',
     '#include "port_game.h"', '']
cases = []
for c, r in sorted(funcs.items(), key=lambda kv: int(kv[1]['addr'], 16)):
    if c in EXCLUDE: continue
    rt, _, params = proto_parts(r['proto'])
    types = [param_type(p) for p in params]
    if '...' in types: continue
    decl = ', '.join(f'{t} p{j}' for j, t in enumerate(types)) or 'void'
    I.append(f'{rt} {c}({decl});')
    un = ' '.join(f'{t} a{j}; MSG_GET(w, k, a{j});' for j, t in enumerate(types))
    call = f'{c}({", ".join(f"a{j}" for j in range(len(types)))})'
    if rt == 'void': ret = f'{call}; return 0;'
    else: ret = f'{rt} v = {call}; unsigned long long r = 0; __builtin_memcpy(&r, &v, sizeof v); return r;'
    cases.append(f'  case 0x{int(r["addr"], 16):x}u: {{ int k = 0; (void)k; {un} {ret} }}')
I.append('')
I.append('unsigned long long rt_runtime_call(unsigned addr, const unsigned *w);')
I += [a for _, _, a in ICALL_ADAPTERS]
I.append('/* (original code address, call-site pointer type) -> adapter with the call site\'s signature */')
I.append('void *orig_fnptr_typed(unsigned addr, int tid) {')
I.append('  switch ((unsigned long long)(addr & ~1u) << 16 | (unsigned)tid) {')
for a_, k_, _ in ICALL_ADAPTERS: I.append(f'  case 0x{a_:x}ull << 16 | {k_}: return (void *)&icall_{a_:x}_{k_};')
I.append('  default: return 0;')
I.append('  }')
I.append('}')
I.append('')
I.append('/* original code address -> the recompiled function as a C function pointer */')
I.append('void *orig_fnptr(unsigned addr) {')
I.append('  switch (addr & ~1u) {')
for c, r in sorted(funcs.items(), key=lambda kv: int(kv[1]['addr'], 16)):
    if c in EXCLUDE or '...' in r['proto']: continue
    I.append(f'  case 0x{int(r["addr"], 16):x}u: return (void *)&{c};')
I.append('  default: return 0;')
I.append('  }')
I.append('}')
I.append('')
I.append('unsigned long long invoke_orig(unsigned addr, const unsigned *w) {')
I.append('  switch (addr & ~1u) {')
I += cases
I.append('  default: return invoke_orig_missing(addr);')
I.append('  }')
I.append('}')
open(os.path.join(OUT, 'invoke_orig.c'), 'w').write('\n'.join(I) + '\n')

print(f"typed indirect calls: {len(fptypes)} pointer types, {len(vtfuncs)} functions stored in data")
print(f"files {stats['files']}, excluded {stats['excluded']}, globals resolved {stats['globals']}, "
      f"msgSend signatures {len(sigs)}, variadic imports {sorted(VARIADIC_IMPORTS)}")
print(f'ambiguous global names used: {len(ambiguous)}', ambiguous[:8])
print(f'unresolved used externs: {len(unresolved)}', sorted(unresolved)[:20])

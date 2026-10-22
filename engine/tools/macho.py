"""Minimal reader for the thin armv6 MH_EXECUTE MGSTUS: segments, sections, symbols,
indirect symbols and dyld binding info. Only what the port's build tools need."""
import struct

LC_SEGMENT, LC_SYMTAB, LC_DYSYMTAB = 0x1, 0x2, 0xb
LC_DYLD_INFO, LC_DYLD_INFO_ONLY = 0x22, 0x80000022

class Section:
    def __init__(s, raw):
        s.name = raw[0:16].rstrip(b'\0').decode(); s.seg = raw[16:32].rstrip(b'\0').decode()
        (s.addr, s.size, s.offset, s.align, s.reloff, s.nreloc, s.flags, s.r1, s.r2) = struct.unpack('<9I', raw[32:68])
        s.type = s.flags & 0xff
    def __repr__(s): return f'{s.seg},{s.name} 0x{s.addr:x}+0x{s.size:x} type {s.type}'

class Segment:
    def __init__(s, raw):
        s.name = raw[8:24].rstrip(b'\0').decode()
        (s.vmaddr, s.vmsize, s.fileoff, s.filesize, s.maxprot, s.initprot, s.nsects, s.flags) = struct.unpack('<8I', raw[24:56])
        s.sections = [Section(raw[56 + i * 68: 56 + (i + 1) * 68]) for i in range(s.nsects)]

class MachO:
    def __init__(m, path):
        m.data = open(path, 'rb').read()
        magic, cpu, sub, ftype, ncmds, sizecmds, flags = struct.unpack_from('<7I', m.data, 0)
        assert magic == 0xfeedface, 'expected thin 32-bit Mach-O'
        m.segments, m.symtab, m.dysymtab, m.dyld_info = [], None, None, None
        off = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<2I', m.data, off)
            raw = m.data[off:off + size]
            if cmd == LC_SEGMENT: m.segments.append(Segment(raw))
            elif cmd == LC_SYMTAB: m.symtab = struct.unpack_from('<4I', raw, 8)
            elif cmd == LC_DYSYMTAB: m.dysymtab = struct.unpack_from('<18I', raw, 8)
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY): m.dyld_info = struct.unpack_from('<10I', raw, 8)
            off += size
        m.sections = [s for g in m.segments for s in g.sections]
        m._symbols = None

    def seg_for(m, addr):
        for g in m.segments:
            if g.vmaddr <= addr < g.vmaddr + g.vmsize: return g

    def read(m, addr, n):
        """Bytes at a vm address. Zero-fill beyond a segment's file size (bss/common)."""
        g = m.seg_for(addr)
        if g is None: return None
        out = bytearray(n)
        rel = addr - g.vmaddr
        avail = max(0, min(n, g.filesize - rel))
        if avail: out[:avail] = m.data[g.fileoff + rel: g.fileoff + rel + avail]
        return bytes(out)

    def u32(m, addr): return struct.unpack('<I', m.read(addr, 4))[0]

    def cstr(m, addr, limit=4096):
        b = m.read(addr, limit) or b''
        return b.split(b'\0', 1)[0].decode('utf-8', 'replace')

    def section(m, seg, name):
        for s in m.sections:
            if s.seg == seg and s.name == name: return s

    @property
    def symbols(m):
        if m._symbols is None:
            symoff, nsyms, stroff, strsize = m.symtab
            m._symbols = []
            for i in range(nsyms):
                strx, ntype, sect, desc, value = struct.unpack_from('<IBBhI', m.data, symoff + i * 12)
                end = m.data.index(b'\0', stroff + strx)
                m._symbols.append((m.data[stroff + strx:end].decode('utf-8', 'replace'), ntype, sect, desc, value))
        return m._symbols

    def indirect_bindings(m):
        """(slot address, symbol name) for every non-lazy / lazy pointer slot."""
        out = []
        indirectsymoff, nindirect = m.dysymtab[12], m.dysymtab[13]
        syms = m.symbols
        for s in m.sections:
            if s.type not in (6, 7):            # S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS
                continue
            for i in range(s.size // 4):
                idx = struct.unpack_from('<I', m.data, indirectsymoff + (s.r1 + i) * 4)[0]
                if idx & 0xC0000000: continue    # INDIRECT_SYMBOL_LOCAL / ABS
                out.append((s.addr + 4 * i, syms[idx][0], s.name))
        return out

    def external_relocs(m):
        """(address, symbol name, addend) for every external relocation. In this
        MH_EXECUTE (no split segments) r_address is absolute; the addend is the
        value already stored at the address."""
        out = []
        extreloff, nextrel = m.dysymtab[14], m.dysymtab[15]
        for i in range(nextrel):
            r_address, info = struct.unpack_from('<iI', m.data, extreloff + i * 8)
            if not (info >> 27) & 1: continue
            out.append((r_address, m.symbols[info & 0xffffff][0], m.u32(r_address)))
        return out

    def dyld_binds(m):
        """Decode bind + weak bind + lazy bind opcode streams (LC_DYLD_INFO)."""
        if not m.dyld_info: return []
        out = []
        for off, size, lazy in ((m.dyld_info[2], m.dyld_info[3], False), (m.dyld_info[6], m.dyld_info[7], True)):
            if size: out += m._run_binds(off, size, lazy)
        return out

    def _run_binds(m, off, size, lazy):
        d = m.data; p = off; end = off + size
        segs = [g for g in m.segments]
        sym = None; seg = 0; segoff = 0; addend = 0; out = []
        def uleb():
            nonlocal p
            v = s = 0
            while True:
                b = d[p]; p += 1; v |= (b & 0x7f) << s; s += 7
                if not b & 0x80: return v
        def sleb():
            nonlocal p
            v = s = 0
            while True:
                b = d[p]; p += 1; v |= (b & 0x7f) << s; s += 7
                if not b & 0x80:
                    if b & 0x40: v -= 1 << s
                    return v
        while p < end:
            b = d[p]; p += 1; op, imm = b & 0xf0, b & 0x0f
            if op == 0x00:
                if not lazy: break
            elif op in (0x10, 0x30): pass                      # dylib ordinal imm / special
            elif op == 0x20: uleb()                            # dylib ordinal uleb
            elif op == 0x40:
                e = d.index(b'\0', p); sym = d[p:e].decode(); p = e + 1
            elif op == 0x50: pass                              # type
            elif op == 0x60: addend = sleb()
            elif op == 0x70: seg = imm; segoff = uleb()
            elif op == 0x80: segoff += uleb()
            elif op == 0x90:
                out.append((segs[seg].vmaddr + segoff, sym, addend)); segoff += 4
            elif op == 0xA0:
                out.append((segs[seg].vmaddr + segoff, sym, addend)); segoff += 4 + uleb()
            elif op == 0xB0:
                out.append((segs[seg].vmaddr + segoff, sym, addend)); segoff += 4 + imm * 4
            elif op == 0xC0:
                cnt = uleb(); skip = uleb()
                for _ in range(cnt):
                    out.append((segs[seg].vmaddr + segoff, sym, addend)); segoff += 4 + skip
        return out

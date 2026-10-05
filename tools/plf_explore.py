#!/usr/bin/env python3
"""Exploration prototype: read a TIA Portal PEData.plf and decode objects.

Throwaway research code (the real tool is C++). Container layouts handled:
  * "v11" layout (seen in a V13 project): 46-byte file header, blocks of
    [size u32][type u32][id u64][12 bytes] ... 0xFF, plus bare
    "$$COMMIT$$"/"##CLOSE###" marker records (20 bytes each).
  * "v14" layout (per ERNW's description): 98-byte header, 44-byte block
    header that adds a 16-byte UUID, and a SHA-256 after every block.
"""
import sys, struct, zlib, hashlib, datetime, collections
import xml.etree.ElementTree as ET

NS = '{http://www.siemens.com/Automation/2004/04/ObjectFrame/Meta}'

BASIC = {  # name -> (size, kind)
    'xs:boolean': (1, 'bool'), 'xs:unsignedByte': (1, 'u'), 'pe:CharT': (2, 'u'),
    'xs:short': (2, 'i'), 'xs:int': (4, 'i'), 'xs:long': (8, 'i'),
    'xs:float': (4, 'f'), 'xs:double': (8, 'f'), 'xs:decimal': (4, 'u'),
    'xs:dateTime': (8, 'dt'), 'xs:string': (4, 'str'), 'pe:GuidT': (16, 'guid'),
    'pe:BlobT': (4, 'blob'), 'pe:XmlT': (4, 'blob'), 'xs:ushort': (2, 'u'),
    'xs:unsignedShort': (2, 'u'), 'xs:uint': (4, 'u'), 'xs:unsignedInt': (4, 'u'),
    'xs:ulong': (8, 'u'), 'xs:unsignedLong': (8, 'u'), 'xs:byte': (1, 'i'),
    'pe:MapT': (4, 'blob'), 'pe:CoreTextAttributeT': (4, 'rel'),
}
BASIC_BY_ID = {
    0x80000001: 'xs:boolean', 0x80000002: 'xs:unsignedByte', 0x80000003: 'pe:CharT',
    0x80000004: 'xs:short', 0x80000005: 'xs:int', 0x80000006: 'xs:long',
    0x80000007: 'xs:float', 0x80000008: 'xs:double', 0x80000009: 'xs:decimal',
    0x8000000a: 'xs:dateTime', 0x8000000b: 'xs:string', 0x8000000c: 'pe:GuidT',
    0x8000000d: 'pe:BlobT', 0x8000000e: 'pe:XmlT', 0x80000012: 'xs:ushort',
    0x80000013: 'xs:uint', 0x80000014: 'xs:ulong', 0x80000015: 'pe:MapT',
    0x8000001f: 'pe:CoreTextAttributeT',
}


def varint(b, o):
    n = s = 0
    while True:
        c = b[o]; o += 1
        n |= (c & 0x7f) << s; s += 7
        if not c & 0x80:
            return n, o


def pstr(b, o):
    """varint-prefixed string; the length counts the prefix itself."""
    n, p = varint(b, o)
    return b[p:o + n].decode('utf-8', 'replace'), o + n


def pstr0(b, o):
    """varint-prefixed string; the length does NOT count the prefix (system tables)."""
    n, p = varint(b, o)
    return b[p:p + n].decode('utf-8', 'replace'), p + n


def ticks(t):
    t &= 0x3fffffffffffffff
    if t == 0:
        return None
    try:
        return (datetime.datetime(1, 1, 1) + datetime.timedelta(microseconds=t // 10)).isoformat()
    except OverflowError:
        return 'ticks:%d' % t


# ---------------------------------------------------------------- container
class Block:
    __slots__ = ('off', 'size', 'type', 'id', 'hdr', 'body_off', 'seq')

    def __repr__(self):
        return 'Block(%#x,%d @%d size %d)' % (self.type, self.id, self.off, self.size)


def read_container(d):
    blocks, marks = [], []
    if d[45] == 0xff and d[0] == 0x40:
        layout, off, hl = 'v11', 46, 28
    else:
        layout, off, hl = 'v14', 98, 44
    seq = 0
    bad_hash = 0
    while off < len(d):
        if layout == 'v11' and d[off:off + 11] in (b'\x0a$$COMMIT$$', b'\x0a##CLOSE###'):
            marks.append((off, d[off + 1:off + 11].decode(), ticks(struct.unpack_from('<Q', d, off + 11)[0])))
            off += 20
            continue
        if off + hl > len(d):
            raise ValueError('truncated block header at %d' % off)
        s, t, oid = struct.unpack_from('<IIQ', d, off)
        if s < hl or off + s > len(d):
            raise ValueError('bad block size %d at %d' % (s, off))
        b = Block(); b.off, b.size, b.type, b.id = off, s, t, oid
        b.hdr = d[off + 16:off + hl]; b.body_off = off + hl; b.seq = seq; seq += 1
        blocks.append(b)
        off += s
        if layout == 'v14':
            if hashlib.sha256(d[b.off:b.off + s]).digest() != d[off:off + 32]:
                bad_hash += 1
            off += 32
    return layout, hl, blocks, marks, bad_hash


# --------------------------------------------------------------------- meta
class Meta:
    def __init__(self):
        self.types = {}       # full name -> dict
        self.by_id = {}       # id -> dict
        self.relations = {}   # id -> (owner full name, relation name, cardinality, behaviour)

    def load(self, xml_bytes):
        root = ET.fromstring(xml_bytes)
        for pkg in root:
            for nsn in pkg:
                if nsn.tag != NS + 'Namespace':
                    continue
                ns = nsn.get('name')
                for c in nsn:
                    tag = c.tag[len(NS):]
                    if 'name' not in c.attrib:
                        continue
                    full = ns + '.' + c.get('name')
                    t = {'kind': tag, 'name': full, 'ns': ns,
                         'id': int(c.get('id'), 0) if c.get('id') else None}
                    if tag == 'Enumeration':
                        t['base'] = c.get('base', 'xs:int')
                        t['consts'] = {int(k.get('value')): k.get('name') for k in c.findall(NS + 'Constant')}
                    elif tag == 'AttributeSet':
                        t['persistent'] = c.get('persistent') == 'true'
                        t['expando'] = c.get('expando')
                        t['attrs'] = [dict(name=a.get('name'), type=self._tn(a.get('type'), ns),
                                           constant=a.get('constant') == 'true',
                                           intrinsic=a.get('intrinsic') == 'true',
                                           virtual=a.get('virtual') == 'true',
                                           removed=a.get('removed') == 'true')
                                      for a in c.findall(NS + 'Attribute')]
                    elif tag == 'ObjectType':
                        t['bases'] = [(b.get('ref'), b.get('primary') == 'true') for b in c.findall(NS + 'Base')]
                        t['impl'] = [(self._tn(i.get('ref'), ns),
                                      {a.get('name'): (None if a.get('constant') is None else a.get('constant') == 'true')
                                       for a in i.findall(NS + 'Attribute')})
                                     for i in c.findall(NS + 'Implements')]
                        for r in c.findall(NS + 'Relation'):
                            if r.get('id'):
                                self.relations[int(r.get('id'), 0)] = (
                                    full, r.get('name'), r.get('cardinality'),
                                    (r.get('behaviourType') or '').split('.')[-1])
                    elif tag == 'Structure':
                        t['elems'] = [(e.get('name'), self._tn(e.get('type'), ns)) for e in c.findall(NS + 'Element')]
                    elif tag == 'Array':
                        t['elem'] = self._tn(c.get('type'), ns)
                    else:
                        continue
                    self.types[full] = t
                    if t['id'] is not None:
                        self.by_id[t['id']] = t

    @staticmethod
    def _tn(name, ns):
        if name is None:
            return None
        if ':' not in name and '.' not in name:
            return ns + '.' + name
        return name

    def size_kind(self, tname):
        if tname in BASIC:
            return BASIC[tname]
        t = self.types.get(tname)
        if t is None:
            return (4, 'unknown')
        if t['kind'] == 'Enumeration':
            return (self.size_kind(t['base'])[0], 'enum')
        if t['kind'] == 'ObjectType':
            return (0, 'obj')      # mapped to a relation, nothing stored inline
        return (4, 'rel')   # structures, arrays: offset into segment

    # object layout: list of (attribute set, [(attr, persisted)]) sorted by name
    def layout(self, tname, _cache={}):
        if (id(self), tname) in _cache:
            return _cache[(id(self), tname)]
        impls = collections.OrderedDict()
        self._collect(tname, impls, True, set())
        out = []
        for asname, amap in impls.items():
            aset = self.types.get(asname)
            if aset is None:
                out.append((asname, None, []))
                continue
            attrs = []
            for a in aset['attrs']:
                if a['name'] in amap:
                    pers = amap[a['name']]
                else:
                    pers = not a['constant'] and not a['intrinsic']
                attrs.append((a, pers))
            out.append((asname, aset, attrs))
        out.sort(key=lambda x: x[0])
        _cache[(id(self), tname)] = out
        return out

    def linearize(self, tname):
        """Depth first; when a type is reached several times only its last
        position counts, so a base always follows every type derived from it."""
        visits = []

        def rec(n, path):
            t = self.types.get(n)
            if n in path or t is None or t['kind'] != 'ObjectType':
                return
            visits.append(n)
            for ref, _ in t['bases']:
                rec(ref, path | {n})
        rec(tname, frozenset())
        out, seen = [], set()
        for n in reversed(visits):
            if n not in seen:
                seen.add(n)
                out.append(n)
        return out[::-1]

    def _collect(self, tname, impls, primary, seen):
        """For every attribute the first explicit constant="true|false" met in
        linearized order decides whether it is stored; an <Attribute> without
        `constant` decides nothing. Checked against the StorageMetaInfoXML of
        a V13 project: identical for all 438 object types."""
        for n in self.linearize(tname):
            for asname, over in self.types[n]['impl']:
                a = impls.setdefault(asname, {})
                aset = self.types.get(asname)
                for an, const in over.items():
                    if const is None or an in a or not aset:
                        continue
                    orig = next((x for x in aset['attrs'] if x['name'] == an), None)
                    if orig is not None:
                        a[an] = (not const) and not orig['intrinsic']


# ------------------------------------------------------------------ project
class Project:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        self.layout, self.hl, self.blocks, self.marks, self.bad_hash = read_container(d)
        self.meta = Meta()
        self.latest = {}
        for b in self.blocks:
            self.latest[(b.type, b.id)] = b
        self.sys_type = 0 if self.layout == 'v11' else None
        self._load_system()

    def body(self, b):
        return self.d[b.body_off:b.off + b.size]

    def _sys_payload(self, b):
        body = self.body(b)
        ln = struct.unpack_from('<I', body, 0)[0]
        return body[4:4 + ln]

    def _load_system(self):
        self.expando = {}     # object type id -> {key: (name, type id)}
        self.named = {}
        metas = 0
        for b in self.latest.values():
            issys = (b.type == 0) if self.layout == 'v11' else (0x70000 <= b.type < 0x70100)
            if not issys:
                continue
            p = self._sys_payload(b)
            if p[:1] == b'\x78':
                try:
                    x = zlib.decompress(p)
                except zlib.error:
                    continue
                if b'<MetaInfo' in x[:400] or b':MetaInfo' in x[:400]:
                    self.meta.load(x); metas += 1
                continue
            # expando entry table: [len][objtype][count][count][1] then entries
            if len(p) >= 20:
                ln, ot, c1, c2 = struct.unpack_from('<IIII', p, 0)
                if ln == len(p) and c1 == c2 and 0 < c1 < 5000 and ot in self.meta_ids_hint(ot):
                    try:
                        o = 16; ent = {}
                        for _ in range(c1):
                            key = struct.unpack_from('<I', p, o)[0]
                            name, o2 = pstr0(p, o + 4)
                            ty = struct.unpack_from('<I', p, o2)[0]
                            ent[key] = (name, ty)
                            o = o2 + 4
                        self.expando.setdefault(ot, {}).update(ent)
                    except (struct.error, IndexError):
                        pass
        self.meta_count = metas

    def meta_ids_hint(self, ot):
        return (ot,) if (ot >> 12) & 0xf == 1 else ()

    # ---- value decoding
    def _val(self, tname, seg, pos):
        size, kind = self.meta.size_kind(tname)
        raw = seg[pos:pos + size]
        if kind == 'obj':
            return None
        if len(raw) < size:
            return None
        if kind == 'bool':
            return raw[0] != 0
        if kind in ('u', 'enum', 'unknown'):
            v = int.from_bytes(raw, 'little')
            if kind == 'enum':
                return self.meta.types[tname]['consts'].get(v, v)
            return v
        if kind == 'i':
            return int.from_bytes(raw, 'little', signed=True)
        if kind == 'f':
            return struct.unpack('<f' if size == 4 else '<d', raw)[0]
        if kind == 'dt':
            return ticks(int.from_bytes(raw, 'little'))
        if kind == 'guid':
            return raw.hex()
        o = int.from_bytes(raw, 'little')
        if o == 0 or o >= len(seg):
            return None
        if kind == 'str':
            return pstr(seg, o)[0]
        if kind == 'blob':
            n, p = varint(seg, o)
            return ('blob', seg[p:o + n])
        return ('rel', o)

    def decode(self, b):
        t = self.meta.by_id.get(b.type)
        if t is None or t['kind'] != 'ObjectType':
            return None
        d = self.d
        lay = self.meta.layout(t['name'])
        nslots = len(lay)
        base = b.off
        slots = struct.unpack_from('<%dI' % (nslots + 2), d, b.body_off)
        out = {'_type': t['name'], '_id': b.id, 'sets': {}, 'rel': [], 'expando': {}}
        for (asname, aset, attrs), so in zip(lay, slots):
            if so == 0 or aset is None:
                continue
            ln = struct.unpack_from('<I', d, base + so)[0]
            seg = d[base + so:base + so + ln]
            if aset['expando']:
                try:
                    out['expando'].update(self._expando(b.type, seg))
                except (struct.error, IndexError):
                    out.setdefault('_err', []).append(asname.split('.')[-1])
                continue
            vals = {}
            pos = 4
            for a, pers in attrs:
                if not pers:
                    continue
                size, _ = self.meta.size_kind(a['type'])
                vals[a['name']] = self._val(a['type'], seg, pos)
                pos += size
            out['sets'][asname.split('.')[-1]] = vals
        nall = b.hdr[10] if self.layout == 'v11' else nslots + 2
        if nall > nslots + 2:
            slots = struct.unpack_from('<%dI' % nall, d, b.body_off)
        for si, so in enumerate(slots[nslots:], nslots):
            if so == 0:
                continue
            if d[base + so + 4:base + so + 8] == b'\xff\xff\xff\x7f':
                cnt = struct.unpack_from('<I', d, base + so + 8)[0]
                lst = [(None,) + struct.unpack_from('<IQ', d, base + so + 12 + 12 * i) for i in range(cnt)]
                out['rel'].append(('typed', si, lst))
            elif si <= nslots + 1:
                size, cnt = struct.unpack_from('<HH', d, base + so)
                if 4 + 16 * cnt > size:
                    continue
                lst = [struct.unpack_from('<IIQ', d, base + so + 4 + 16 * i) for i in range(cnt)]
                out['rel'].append(('keyed', si, lst))
        return out

    def _expando(self, otype, seg):
        ent = self.expando.get(otype)
        res = {}
        H = 16 if self.layout == 'v11' else 18
        if not ent or len(seg) < H:
            return res
        count = struct.unpack_from('<I', seg, 8)[0] & 0xffffff
        VB = H + count * 8
        for i in range(count):
            key = struct.unpack_from('<I', seg, H + i * 4)[0]
            if key not in ent:
                res['?%d' % key] = None
                continue
            name, ty = ent[key]
            tn = BASIC_BY_ID.get(ty)
            if tn is None:
                mt = self.meta.by_id.get(ty)
                tn = mt['name'] if mt else None
            vpos = H + count * 4 + i * 4
            if tn is None:
                res[name] = ('type?', hex(ty), seg[vpos:vpos + 4].hex())
                continue
            size, kind = self.meta.size_kind(tn)
            if size <= 4 and kind not in ('str', 'blob', 'rel'):
                res[name] = self._val(tn, seg, vpos)
            else:
                o = struct.unpack_from('<I', seg, vpos)[0]
                if o == 0xffffffff:
                    res[name] = None
                    continue
                p = VB + o
                if kind == 'str':
                    res[name] = pstr(seg, p)[0]
                elif kind == 'blob':
                    n, q = varint(seg, p)
                    res[name] = ('blob', seg[q:p + n])
                elif kind == 'rel':
                    res[name] = ('rel', tn, seg[p:p + 48].hex())
                else:
                    res[name] = self._val(tn, seg[p:p + size], 0)
        return res


if __name__ == '__main__':
    P = Project(sys.argv[1])
    print('layout', P.layout, 'blocks', len(P.blocks), 'live objects', len(P.latest),
          'marks', len(P.marks), 'bad hashes', P.bad_hash)
    print('meta documents', P.meta_count, 'types', len(P.meta.types), 'relations', len(P.meta.relations),
          'expando tables', len(P.expando))

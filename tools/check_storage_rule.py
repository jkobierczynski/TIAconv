#!/usr/bin/env python3
# tiaconv - TIA Portal project reader
# Copyright (C) 2026 Jurgen Kobierczynski
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the rule that decides which attributes of an object are stored.

The rule (see docs/FORMAT.md, "Which attributes are stored") cannot be read
off the type model directly, so it is checked against the data in two ways:

1. Tiling. With the right rule, the fixed part of every attribute-set segment
   plus the strings and blobs it points to cover the segment exactly: no gap,
   no overlap, nothing left over. A wrong rule shifts the fixed part and that
   stops being true for a visible share of the objects.

2. StorageMetaInfoXML. Projects from V13 and older carry a second document
   with the result of the rule for every object type (isConstant and
   isIntrinsic per attribute). Where it exists, the outcome is compared with
   it attribute by attribute.

Usage: check_storage_rule.py PEData.plf [PEData.plf ...]
Exit status 1 when a file shows a disagreement.
"""
import collections
import os
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plf_explore import Project, varint  # noqa: E402

SN = '{http://www.siemens.com/Automation/2010/01/ObjectFrame.MetaInfo/ObjectFrame/MetaInfo}'
XSI = '{http://www.w3.org/2001/XMLSchema-instance}type'


def tiling(P):
    """Count segments by outcome; returns (counter, counter of offending type:set)."""
    M, d = P.meta, P.d
    c = collections.Counter()
    bad = collections.Counter()
    for b in P.latest.values():
        t = M.by_id.get(b.type)
        if not t or t['kind'] != 'ObjectType':
            continue
        if struct.unpack_from('<H', b.hdr, 8)[0] & 4:      # deleted
            continue
        lay = M.layout(t['name'])
        short = t['name'].split('.')[-1]
        if b.hdr[10] < len(lay):
            c['fewer slots than attribute sets'] += 1
            bad[short] += 1
            continue
        slots = struct.unpack_from('<%dI' % len(lay), d, b.body_off)
        for (n, aset, attrs), so in zip(lay, slots):
            if so == 0 or aset is None or aset['expando']:
                continue
            where = short + ':' + n.split('.')[-1]
            ln = struct.unpack_from('<I', d, b.off + so)[0]
            seg = d[b.off + so:b.off + so + ln]
            pos = 4
            items = []
            ok = True
            for a, persisted in attrs:
                if not persisted:
                    continue
                size, kind = M.size_kind(a['type'])
                if kind in ('str', 'blob', 'rel'):
                    if pos + 4 > ln:
                        ok = False
                        break
                    items.append((kind, struct.unpack_from('<I', seg, pos)[0]))
                pos += size
            fixed = pos
            if not ok or fixed > ln:
                c['fixed part longer than the segment'] += 1
                bad[where] += 1
                continue
            spans = []
            nested = False
            for kind, o in items:
                if o == 0:
                    continue
                if not fixed <= o < ln:
                    ok = False
                    break
                if kind == 'rel':          # structure, array or text: size not known here
                    nested = True
                    continue
                try:
                    length, after = varint(seg, o)
                except IndexError:
                    ok = False
                    break
                if length < after - o or o + length > ln:
                    ok = False
                    break
                spans.append((o, o + length))
            if not ok:
                c['offset outside the segment'] += 1
                bad[where] += 1
                continue
            if nested:
                c['plausible (holds a structure, not tiled)'] += 1
                continue
            end = fixed
            tiled = True
            for s, e in sorted(set(spans)):
                if s != end:
                    tiled = False
                    break
                end = e
            if tiled and end == ln:
                c['exactly tiled'] += 1
            else:
                c['gap or overlap'] += 1
                bad[where] += 1
    return c, bad


def storage_documents(P):
    """The StorageMetaInfoXML documents of an older project (one per package group)."""
    out = []
    for b in P.latest.values():
        if b.type != 0 or P.layout != 'v11':
            continue
        p = P._sys_payload(b)
        if p[:1] != b'\x78':
            continue
        try:
            x = zlib.decompress(p)
        except zlib.error:
            continue
        if b'<StorageMetaInfoXML' in x[:400]:
            out.append(x)
    return out


def compare_storage(P, doc):
    """Returns (object types that agree, object types compared, differences)."""
    root = ET.fromstring(doc)
    M = P.meta
    agree = total = 0
    diffs = []
    for e in root.findall(SN + 'element'):
        if e.get(XSI) != 'ObjectTypeXML':
            continue
        t = M.by_id.get(int(e.get('id')))
        if not t:
            continue
        total += 1
        good = True
        lay = M.layout(t['name'])
        for (n, aset, attrs), s in zip(lay, e.findall(SN + 'attributeSet')):
            for (a, persisted), info in zip(attrs, s.findall(SN + 'attributeInfo')):
                expect = info.get('isConstant') != 'true' and info.get('isIntrinsic') != 'true'
                if M.size_kind(a['type'])[0] and persisted != expect:
                    good = False
                    diffs.append('%s %s.%s: rule says %s' % (t['name'].split('.')[-1], n.split('.')[-1], a['name'],
                                                           'stored' if persisted else 'not stored'))
        agree += good
    return agree, total, diffs


def main(paths):
    failed = False
    for path in paths:
        P = Project(path)
        c, bad = tiling(P)
        print(path)
        for k, v in sorted(c.items(), key=lambda kv: -kv[1]):
            print('  %7d  %s' % (v, k))
        wrong = sum(v for k, v in c.items() if k not in ('exactly tiled', 'plausible (holds a structure, not tiled)'))
        for k, v in bad.most_common(10):
            print('           %s x%d' % (k, v))
        agree = total = 0
        diffs = []
        for doc in storage_documents(P):
            a, t, d = compare_storage(P, doc)
            agree += a
            total += t
            diffs += d
        if total:
            print('  StorageMetaInfoXML: %d of %d object types agree' % (agree, total))
            for line in diffs[:10]:
                print('           ' + line)
            wrong += total - agree
        failed = failed or wrong > 0
    return 1 if failed else 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Prototype: hardware + network inventory from a TIA Portal PEData.plf."""
import sys, json, struct, collections
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from plf_explore import Project

def ip4(v):
    return None if v is None else '.'.join(str((v >> s) & 255) for s in (24, 16, 8, 0))

def mac(v):
    return None if not v else '-'.join('%02X' % ((v >> s) & 255) for s in (40, 32, 24, 16, 8, 0))

def build(path):
    P = Project(path)
    M = P.meta
    relid = {(o.split('.')[-1], n): i for i, (o, n, c, bh) in M.relations.items()}
    R_PARENT = relid[('BaseDeviceItemData', 'Parent')]
    R_CONT = relid[('BaseDeviceItemData', 'Container')]
    R_NODE_ITEM = relid[('NodeData', 'DeviceItem')]
    R_NODE_SUBNET = relid[('NodeData', 'Subnet')]

    def derives(tname, base, seen=None):
        if tname == base:
            return True
        t = M.types.get(tname)
        return bool(t) and t['kind'] == 'ObjectType' and any(derives(r, base) for r, _ in t['bases'])

    live = {}
    deleted = 0
    for key, b in P.latest.items():
        if b.type == 0 or 0x70000 <= b.type < 0x70100:
            continue
        if struct.unpack_from('<H', b.hdr, 8)[0] & 4:
            deleted += 1
            continue
        live[key] = b
    kinds = {}
    for ty in {k[0] for k in live}:
        t = M.by_id.get(ty)
        if not t:
            continue
        n = t['name']
        if derives(n, 'Siemens.Automation.DomainModel.BaseDeviceData'):
            kinds[ty] = 'device'
        elif derives(n, 'Siemens.Automation.DomainModel.BaseDeviceItemData'):
            kinds[ty] = 'item'
        elif n.endswith('HwConfiguration.Model.NodeData'):
            kinds[ty] = 'node'
        elif n.endswith('HwConfiguration.Model.SubnetData'):
            kinds[ty] = 'subnet'
        elif n.endswith('DomainModel.ProjectData'):
            kinds[ty] = 'project'
    objs = {}
    for key, b in live.items():
        k = kinds.get(key[0])
        if not k:
            continue
        o = P.decode(b)
        o['kind'] = k
        o['single'] = {}
        for form, si, lst in o['rel']:
            if form == 'keyed':
                for rid, ty, oid in lst:
                    o['single'].setdefault(rid, (ty, oid))
        objs[key] = o

    def core(o):
        return o['sets'].get('ICoreAttributes', {})

    devices, subnets, project = {}, {}, None
    for key, o in objs.items():
        c = core(o)
        if o['kind'] == 'project':
            project = dict(name=c.get('Name'), created=c.get('CreationTime'), modified=c.get('ModifiedTime'),
                           author=c.get('Author'), last_modified_by=c.get('LastModifiedBy'))
        elif o['kind'] == 'device':
            devices[key] = dict(name=c.get('Name'), type=c.get('Subtype'), items=[])
        elif o['kind'] == 'subnet':
            subnets[key] = dict(name=c.get('Name'), type=c.get('Subtype'), nodes=[])
    items = {}
    for key, o in objs.items():
        if o['kind'] != 'item':
            continue
        c = core(o); h = o['sets'].get('IDeviceItemData', {})
        items[key] = dict(name=c.get('Name'), type=c.get('Subtype'), type_name=h.get('InvariantTypeName') or None,
                          order_number=h.get('OrderNumber') or None, firmware=h.get('FwVersion') or None,
                          position=h.get('PositionNumber'), author=c.get('Author'), modified=c.get('ModifiedTime'),
                          interfaces=[], _dev=o['single'].get(R_PARENT), _cont=o['single'].get(R_CONT), _id=key[1])
    orphans = 0
    for key, o in objs.items():
        if o['kind'] != 'node':
            continue
        c = core(o); n = o['sets'].get('INodeData', {}); e = o['expando']
        node = dict(name=c.get('Name'), node_id=n.get('NodeID'), node_type=n.get('NodeType'))
        if 'NodeIPAddress' in e:
            node.update(ip=ip4(e.get('NodeIPAddress')), mask=ip4(e.get('NodeIPSubnetMask')),
                        router=ip4(e.get('NodeIPDefaultRouterAddress')) if e.get('NodeIPDefaultRouterAddressUsed') else None,
                        ip_protocol_used=e.get('NodeIPProtocolUsed'),
                        profinet_name=e.get('PnNameOfStation'), profinet_name_auto=e.get('PnPnNoSAutoGenerate'),
                        mac=mac(e.get('NodeMacAddress')))
        sn = o['single'].get(R_NODE_SUBNET)
        node['subnet'] = subnets[sn]['name'] if sn in subnets else None
        if sn in subnets:
            subnets[sn]['nodes'].append(node['name'])
        it = o['single'].get(R_NODE_ITEM)
        if it in items:
            items[it]['interfaces'].append(node)
        else:
            orphans += 1
    def owner_device(it, depth=0):
        p = it['_dev']
        while p is not None and p not in devices and p in items and depth < 32:
            p = items[p]['_dev']; depth += 1
        return p if p in devices else None

    def owner_module(key, depth=0):
        """nearest enclosing item that is an orderable module"""
        while key in items and depth < 32:
            if items[key]['order_number']:
                return key
            key = items[key]['_dev']; depth += 1
        return None

    for key, it in list(items.items()):
        dev = owner_device(it)
        if dev is None:
            orphans += 1
            continue
        if it['interfaces'] and not it['order_number']:
            mod = owner_module(key)
            if mod is not None:
                for n in it['interfaces']:
                    n['interface'] = it['name']
                items[mod]['interfaces'].extend(it['interfaces'])
                it['interfaces'] = []
        devices[dev]['items'].append(it)
    out = dict(project=project, layout=P.layout,
               saves=[m for m in P.marks if m[1].startswith('$$')],
               devices=[], subnets=list(subnets.values()),
               stats=dict(blocks=len(P.blocks), live_objects=len(live), deleted=deleted, orphans=orphans))
    for dev in devices.values():
        byid = {i['_id']: i for i in dev['items']}
        mods = []
        for it in sorted(dev['items'], key=lambda i: (i['position'] if i['position'] is not None else 0, i['_id'])):
            if it['order_number'] or it['interfaces']:
                parent = byid.get(it['_cont'][1]) if it['_cont'] else None
                m = {k: v for k, v in it.items() if not k.startswith('_')}
                m['in'] = parent['name'] if parent else None
                mods.append(m)
        out['devices'].append(dict(name=dev['name'], type=dev['type'], modules=mods))
    return out

if __name__ == '__main__':
    inv = build(sys.argv[1])
    if len(sys.argv) > 2:
        json.dump(inv, open(sys.argv[2], 'w'), indent=1)
    p = inv['project']
    print('Project %s  (created %s by %s, last change %s by %s)' % (p['name'], p['created'], p['author'], p['modified'], p['last_modified_by']))
    for d in inv['devices']:
        print('Device: %s [%s]' % (d['name'], d['type']))
        for m in d['modules']:
            print('  %-4s %-28s %-22s %-21s %-6s in=%s' % (m['position'], m['name'], m['type_name'] or m['type'], m['order_number'] or '-', m['firmware'] or '-', m['in']))
            for n in m['interfaces']:
                print('        %s (%s)  ip=%s mask=%s router=%s pn-name=%s mac=%s subnet=%s' % (n['name'], n.get('interface'), n.get('ip'), n.get('mask'), n.get('router'), n.get('profinet_name'), n.get('mac'), n['subnet']))
    print('Subnets:', inv['subnets'])
    print('Stats:', inv['stats'])

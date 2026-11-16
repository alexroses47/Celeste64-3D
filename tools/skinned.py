"""Conversión de modelos con huesos (.glb) al formato .mdl de la 3DS.

Formato .mdl (little-endian):
  cabecera  '<4sIIIIIII'  magic 'C64M', versión, nVerts, nIdx, nPartes, nHuesos, nAnims, nMateriales
  raíz      12f           matriz 3x4 (filas) de los ancestros que no son huesos (p. ej. "Armature")
  materiales nMat x (nombre[32], textura[32])
  partes    nPartes x '<3I' (material, inicioÍndice, cantidad)
  huesos    nHuesos x ('<i' padre, 12f inversa de bind 3x4, 10f reposo t3 q4 s3, nombre[16])
  anims     nAnims x (nombre[32], '<IfI' cuadros, duración, desplazamiento en floats)
  datos de animación: por cuadro, por hueso: 10f (t3, q4 xyzw, s3)
  vértices  nVerts x '<5f4b4B4B' (pos, uv, normal, huesos*3, pesos*255)
  índices   nIdx x u16 (alineado a 4 bytes)
Las animaciones se muestrean a ANIM_FPS cuadros por segundo; la 3DS interpola entre cuadros.
"""
import json
import math
import os
import struct

import convert as c

MODEL_MAGIC = b'C64M'
MODEL_VERSION = 1
ANIM_FPS = 30.0


def _glb(path):
    b = open(path, 'rb').read()
    jlen = struct.unpack_from('<I', b, 12)[0]
    j = json.loads(b[20:20 + jlen])
    boff = 20 + jlen
    binary = b[boff + 8: boff + 8 + struct.unpack_from('<I', b, boff)[0]]
    return j, binary


def _node_trs(node):
    return (list(node.get('translation', [0, 0, 0])), list(node.get('rotation', [0, 0, 0, 1])),
            list(node.get('scale', [1, 1, 1])))


def _trs_matrix(t, q, s):
    return c.mat_mul(c.mat_mul(c.mat_translate(*t), c.mat_quat(q)), c.mat_scale(*s))


def _slerp(a, b, f):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = [-x for x in b], -d
    if d > 0.9995:
        r = [x + (y - x) * f for x, y in zip(a, b)]
    else:
        th = math.acos(d)
        sa, sb = math.sin((1 - f) * th) / math.sin(th), math.sin(f * th) / math.sin(th)
        r = [x * sa + y * sb for x, y in zip(a, b)]
    l = math.sqrt(sum(x * x for x in r))
    return [x / l for x in r]


def _sample(times, values, interp, t, is_quat):
    if t <= times[0]:
        return list(values[0])
    if t >= times[-1]:
        return list(values[-1])
    for i in range(len(times) - 1):
        if times[i] <= t <= times[i + 1]:
            if interp == 'STEP':
                return list(values[i])
            f = (t - times[i]) / (times[i + 1] - times[i])
            if is_quat:
                return _slerp(values[i], values[i + 1], f)
            return [a + (b - a) * f for a, b in zip(values[i], values[i + 1])]
    return list(values[-1])


def _inverse_affine(m):
    # inversa de una matriz afín 4x4 (parte lineal 3x3 + traslación)
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[0.0] * 3 for _ in range(3)]
    for i in range(3):
        for k in range(3):
            r = [x for x in range(3) if x != k]
            cc = [x for x in range(3) if x != i]
            minor = a[r[0]][cc[0]] * a[r[1]][cc[1]] - a[r[0]][cc[1]] * a[r[1]][cc[0]]
            inv[i][k] = (-1) ** (i + k) * minor / det
    t = [m[0][3], m[1][3], m[2][3]]
    it = [-sum(inv[i][k] * t[k] for k in range(3)) for i in range(3)]
    return [inv[0] + [it[0]], inv[1] + [it[1]], inv[2] + [it[2]], [0, 0, 0, 1]]


def rows34(m):
    return [m[r][k] for r in range(3) for k in range(4)]


def convert_skinned(content, name, bank, romfs):
    j, binary = _glb(os.path.join(content, 'Models', name + '.glb'))
    nodes = j['nodes']
    parent = {}
    for i, n in enumerate(nodes):
        for ch in n.get('children', []):
            parent[ch] = i

    if not j.get('skins'):
        return _convert_static(j, binary, name, bank, romfs)
    skin = j['skins'][0]
    joints = list(skin['joints'])
    # orden padre-antes-que-hijo
    order = []
    def depth(n):
        d = 0
        while n in parent:
            n = parent[n]
            d += 1
        return d
    # los ancestros de los huesos también se exportan como huesos (fijos salvo que una animación los mueva),
    # así toda la jerarquía queda dentro del modelo y la raíz es la identidad
    exported = set(joints)
    for jn_ in joints:
        a = parent.get(jn_)
        while a is not None:
            exported.add(a)
            a = parent.get(a)
    order = sorted(exported, key=lambda n: (depth(n), n))
    remap = {node: i for i, node in enumerate(order)}            # nodo -> índice nuevo
    old_index = {node: i for i, node in enumerate(joints)}        # nodo -> índice en JOINTS_0

    def rest_world(n):
        m = _trs_matrix(*_node_trs(nodes[n]))
        return c.mat_mul(rest_world(parent[n]), m) if n in parent else m

    ibm = c.read_accessor(j, binary, skin['inverseBindMatrices'])
    inv_bind = {}
    for node in order:
        if node in old_index:
            m = ibm[old_index[node]]
            inv_bind[node] = [[m[col * 4 + row] for col in range(4)] for row in range(4)]
        else:
            inv_bind[node] = _inverse_affine(rest_world(node))
    root = c.mat_identity()
    if len(order) + 1 > 24:
        raise ValueError(f'{name}: {len(order)} huesos, el máximo es 24')

    # ---- materiales y texturas
    images = []
    for img in j.get('images', []):
        view = j['bufferViews'][img['bufferView']]
        o = view.get('byteOffset', 0)
        images.append(binary[o:o + view['byteLength']])
    materials = []
    for m in j['materials']:
        tex = m.get('pbrMetallicRoughness', {}).get('baseColorTexture')
        key = bank.by_bytes(images[j['textures'][tex['index']]['source']]) if tex else ''
        materials.append((m.get('name', ''), key))

    # ---- mallas (en espacio de bind: los nodos de malla con skin se ignoran, como dice glTF)
    verts, indices, parts = bytearray(), [], []
    nverts = 0
    by_mat = {}
    for ni, node in enumerate(nodes):
        if 'mesh' not in node or 'skin' not in node:
            continue
        for p in j['meshes'][node['mesh']]['primitives']:
            a = p['attributes']
            pos = c.read_accessor(j, binary, a['POSITION'])
            nrm = c.read_accessor(j, binary, a['NORMAL'])
            uv = c.read_accessor(j, binary, a['TEXCOORD_0'])
            jn = c.read_accessor(j, binary, a['JOINTS_0'])
            wt = c.read_accessor(j, binary, a['WEIGHTS_0'])
            wacc = j['accessors'][a['WEIGHTS_0']]
            idx = c.read_accessor(j, binary, p['indices'])
            if 'JOINTS_0' not in a or 'WEIGHTS_0' not in a:
                continue
            by_mat.setdefault(p.get('material', 0), []).append((pos, nrm, uv, jn, wt, wacc, idx))

    # ---- mallas rígidas (sin skin): se pegan al hueso del que cuelgan, o a un hueso fijo extra.
    # SkinnedModel.Render las dibuja con la matriz de su nodo; con bind == reposo basta con la matriz
    # de reposo del nodo y el peso completo a ese hueso.
    node_world = {}
    def world_of(n):
        if n not in node_world:
            m = _trs_matrix(*_node_trs(nodes[n]))
            node_world[n] = c.mat_mul(world_of(parent[n]), m) if n in parent else m
        return node_world[n]
    static_joint = None
    for ni, node in enumerate(nodes):
        if 'mesh' not in node:
            continue
        for p in j['meshes'][node['mesh']]['primitives']:
            a = p['attributes']
            if 'skin' in node and 'JOINTS_0' in a and 'WEIGHTS_0' in a:
                continue
            anc = parent.get(ni)
            while anc is not None and anc not in remap:
                anc = parent.get(anc)
            if anc is None:
                if static_joint is None:
                    static_joint = len(order)
                ji = static_joint
            else:
                ji = remap[anc]
            wm = world_of(ni)
            nm = c.normal_matrix(wm)
            pos = [c.xform_point(wm, v) for v in c.read_accessor(j, binary, a['POSITION'])]
            nrm = [c.xform_normal(nm, v) for v in c.read_accessor(j, binary, a['NORMAL'])]
            uv = c.read_accessor(j, binary, a['TEXCOORD_0'])
            idx = c.read_accessor(j, binary, p['indices'])
            jn = [(ji, 0, 0, 0)] * len(pos)
            wt = [(1.0, 0.0, 0.0, 0.0)] * len(pos)
            by_mat.setdefault(p.get('material', 0), []).append((pos, nrm, uv, jn, wt, {'componentType': 5126, 'rigid': True}, idx))
    if static_joint is not None:
        # hueso fijo: reposo identidad e inversa de bind = inversa(raíz) => matriz de piel = identidad
        order.append('static')
        inv_bind['static'] = _inverse_affine(root)

    for mat in sorted(by_mat):
        istart = len(indices)
        for pos, nrm, uv, jn, wt, wacc, idx in by_mat[mat]:
            base = nverts
            for k in range(len(pos)):
                w = list(wt[k])
                if wacc['componentType'] == 5121:
                    w = [x / 255 for x in w]
                elif wacc['componentType'] == 5123:
                    w = [x / 65535 for x in w]
                total = sum(w) or 1.0
                wb = [round(x / total * 255) for x in w]
                wb[wb.index(max(wb))] += 255 - sum(wb)          # que sumen exactamente 255
                if wacc.get('rigid'):
                    jb = [i * 3 for i in jn[k]]
                else:
                    jb = [remap[joints[i]] * 3 for i in jn[k]]
                nb = [max(-127, min(127, round(x * 127))) for x in nrm[k]]
                verts += struct.pack('<5f4b4B4B', *pos[k], uv[k][0], 1.0 - uv[k][1], *nb, 0, *jb, *wb)
            nverts += len(pos)
            indices += [base + i for i in idx]
        parts.append((mat, istart, len(indices) - istart))
    if nverts > 65535:
        raise ValueError(f'{name}: demasiados vértices')

    # ---- animaciones
    rest = {node: (_node_trs(nodes[node]) if node != 'static' else ([0, 0, 0], [0, 0, 0, 1], [1, 1, 1])) for node in order}
    anims, data = [], []
    for an in j.get('animations', []):
        channels = {}
        dur = 0.0
        for ch in an['channels']:
            tgt = ch['target']
            if tgt.get('node') not in remap:
                continue
            s = an['samplers'][ch['sampler']]
            times = c.read_accessor(j, binary, s['input'])
            vals = c.read_accessor(j, binary, s['output'])
            channels[(tgt['node'], tgt['path'])] = (times, vals, s.get('interpolation', 'LINEAR'))
            dur = max(dur, times[-1])
        if not channels:
            continue
        frames = max(1, int(round(dur * ANIM_FPS)) + 1)
        offset = len(data)
        for f in range(frames):
            t = min(f / ANIM_FPS, dur)
            for node in order:
                tr, q, sc = rest[node]
                if (node, 'translation') in channels:
                    tr = _sample(*channels[(node, 'translation')][:2], channels[(node, 'translation')][2], t, False)
                if (node, 'rotation') in channels:
                    q = _sample(*channels[(node, 'rotation')][:2], channels[(node, 'rotation')][2], t, True)
                if (node, 'scale') in channels:
                    sc = _sample(*channels[(node, 'scale')][:2], channels[(node, 'scale')][2], t, False)
                data += list(tr) + list(q) + list(sc)
        anims.append((an['name'], frames, dur, offset))

    # ---- escribir
    out = bytearray()
    out += struct.pack('<4sIIIIIII', MODEL_MAGIC, MODEL_VERSION, nverts, len(indices) + (len(indices) % 2),
                       len(parts), len(order), len(anims), len(materials))
    out += struct.pack('<12f', *rows34(root))
    for mname, tex in materials:
        out += mname.encode()[:31].ljust(32, b'\0') + tex.encode().ljust(32, b'\0')
    for mat, istart, cnt in parts:
        out += struct.pack('<3I', mat, istart, cnt)
    for node in order:
        p = parent.get(node)
        out += struct.pack('<i', remap[p] if p in remap else -1)
        out += struct.pack('<12f', *rows34(inv_bind[node]))
        tr, q, sc = rest[node]
        out += struct.pack('<10f', *tr, *q, *sc)
        out += (nodes[node].get('name', '') if node != 'static' else '_static').encode()[:15].ljust(16, b'\0')
    for aname, frames, dur, offset in anims:
        out += aname.encode()[:31].ljust(32, b'\0') + struct.pack('<IfI', frames, dur, offset)
    out += struct.pack(f'<{len(data)}f', *data)
    out += verts
    if len(indices) % 2:
        indices.append(0)
    out += struct.pack(f'<{len(indices)}H', *indices)

    os.makedirs(os.path.join(romfs, 'models'), exist_ok=True)
    with open(os.path.join(romfs, 'models', name + '.mdl'), 'wb') as f:
        f.write(out)
    print(f'modelo {name}: {nverts} vértices, {len(indices) // 3} triángulos, {len(order)} huesos, '
          f'{len(anims)} animaciones ({", ".join(a[0] for a in anims)}), {len(out) / 1024:.0f} KB')


def _write_model(name, romfs, root, materials, parts, joints, anims, data, verts, nverts, indices):
    out = bytearray()
    out += struct.pack('<4sIIIIIII', MODEL_MAGIC, MODEL_VERSION, nverts, len(indices) + (len(indices) % 2),
                       len(parts), len(joints), len(anims), len(materials))
    out += struct.pack('<12f', *rows34(root))
    for mname, tex in materials:
        out += mname.encode()[:31].ljust(32, b'\0') + tex.encode().ljust(32, b'\0')
    for mat, istart, cnt in parts:
        out += struct.pack('<3I', mat, istart, cnt)
    for parent, inv, rest, jname in joints:
        out += struct.pack('<i', parent) + struct.pack('<12f', *rows34(inv)) + struct.pack('<10f', *rest)
        out += jname.encode()[:15].ljust(16, b'\0')
    for aname, frames, dur, offset in anims:
        out += aname.encode()[:31].ljust(32, b'\0') + struct.pack('<IfI', frames, dur, offset)
    out += struct.pack(f'<{len(data)}f', *data)
    out += verts
    if len(indices) % 2:
        indices = indices + [0]
    out += struct.pack(f'<{len(indices)}H', *indices)
    os.makedirs(os.path.join(romfs, 'models'), exist_ok=True)
    with open(os.path.join(romfs, 'models', name + '.mdl'), 'wb') as f:
        f.write(out)
    return len(out)


def _convert_static(j, binary, name, bank, romfs):
    """Modelo sin huesos: nodos horneados en los vértices y un único hueso identidad."""
    images = []
    for img in j.get('images', []):
        view = j['bufferViews'][img['bufferView']]
        o = view.get('byteOffset', 0)
        images.append(binary[o:o + view['byteLength']])
    materials = []
    for m in j['materials']:
        tex = m.get('pbrMetallicRoughness', {}).get('baseColorTexture')
        key = bank.by_bytes(images[j['textures'][tex['index']]['source']]) if tex else ''
        materials.append((m.get('name', ''), key))

    prims = c.load_glb_with_materials(j, binary)
    by_mat = {}
    for pos, nrm, uv, idx, mat in prims:
        by_mat.setdefault(mat, []).append((pos, nrm, uv, idx))
    verts, indices, parts = bytearray(), [], []
    nverts = 0
    for mat in sorted(by_mat):
        istart = len(indices)
        for pos, nrm, uv, idx in by_mat[mat]:
            base = nverts
            for k in range(len(pos)):
                nb = [max(-127, min(127, round(x * 127))) for x in nrm[k]]
                verts += struct.pack('<5f4b4B4B', *pos[k], uv[k][0], 1.0 - uv[k][1], *nb, 0, 0, 0, 0, 0, 255, 0, 0, 0)
            nverts += len(pos)
            indices += [base + i for i in idx]
        parts.append((mat, istart, len(indices) - istart))
    ident = c.mat_identity()
    joints = [(-1, ident, [0, 0, 0, 0, 0, 0, 1, 1, 1, 1], 'Root')]
    size = _write_model(name, romfs, ident, materials, parts, joints, [], [], verts, nverts, list(indices))
    print(f'modelo {name}: {nverts} vértices, {len(indices) // 3} triángulos, estático, {size / 1024:.0f} KB')

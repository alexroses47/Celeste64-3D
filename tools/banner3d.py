#!/usr/bin/env python3
"""Arma el modelo del banner 3D del menú HOME a partir del logo del juego.

Toma Content/Models/logo.glb, lo mete bajo un nodo raíz que lo escala y centra para la
cámara del menú HOME (en z = 44.786 mirando a -Z, 30° de campo vertical: a z = 0 se ven
unas 24 x 40 unidades) y le agrega un balanceo suave en bucle, como el logo de la
pantalla de título. Escribe un .glb que pycgfx convierte a CGFX.

    tools/banner3d.py ext/Celeste64/Content/Models/logo.glb meta/banner_logo.glb
"""
import io
import json
import math
import struct
import sys

HEIGHT = 19.0          # alto del logo en el banner (de ~24 visibles)
CENTER_Y = 1.0         # la cámara está en y = 1
PERIOD = 6.0           # segundos por vaivén
SWAY_DEG = 12.0        # giro máximo a cada lado
STEPS = 24             # fotogramas clave por vuelta
TEXTURE_SIZE = 128     # el CGFX del banner no puede pasar de 512 KB: la textura de 256 no cabe


def read_glb(path):
    b = open(path, 'rb').read()
    jlen = struct.unpack_from('<I', b, 12)[0]
    j = json.loads(b[20:20 + jlen])
    o = 20 + jlen
    blen = struct.unpack_from('<I', b, o)[0]
    return j, bytearray(b[o + 8:o + 8 + blen])


def write_glb(path, j, binary):
    while len(binary) % 4:
        binary += b'\0'
    js = json.dumps(j, separators=(',', ':')).encode()
    while len(js) % 4:
        js += b' '
    total = 12 + 8 + len(js) + 8 + len(binary)
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, total))
        f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
        f.write(struct.pack('<II', len(binary), 0x004E4942) + bytes(binary))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def quat_rotate(q, v):
    x, y, z, _ = quat_mul(quat_mul(q, (v[0], v[1], v[2], 0)), (-q[0], -q[1], -q[2], q[3]))
    return (x, y, z)


def node_bounds(j, binary):
    """Caja del logo en su espacio (nodos de un nivel, como logo.glb)."""
    lo, hi = [1e9] * 3, [-1e9] * 3
    for n in j['nodes']:
        if 'mesh' not in n:
            continue
        t = n.get('translation', [0, 0, 0])
        r = n.get('rotation', [0, 0, 0, 1])
        s = n.get('scale', [1, 1, 1])
        for p in j['meshes'][n['mesh']]['primitives']:
            acc = j['accessors'][p['attributes']['POSITION']]
            bv = j['bufferViews'][acc['bufferView']]
            off = bv.get('byteOffset', 0) + acc.get('byteOffset', 0)
            stride = bv.get('byteStride', 12)
            for k in range(acc['count']):
                v = struct.unpack_from('<3f', binary, off + k * stride)
                v = quat_rotate(r, (v[0] * s[0], v[1] * s[1], v[2] * s[2]))
                v = (v[0] + t[0], v[1] + t[1], v[2] + t[2])
                for i in range(3):
                    lo[i] = min(lo[i], v[i])
                    hi[i] = max(hi[i], v[i])
    return lo, hi


def add_accessor(j, binary, data, fmt, comp, typ, count, minmax=None):
    while len(binary) % 4:
        binary += b'\0'
    off = len(binary)
    binary += data
    j['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': len(data)})
    acc = {'bufferView': len(j['bufferViews']) - 1, 'componentType': comp, 'count': count, 'type': typ}
    if minmax:
        acc['min'], acc['max'] = minmax
    j['accessors'].append(acc)
    return len(j['accessors']) - 1


def main(src, dst):
    j, binary = read_glb(src)
    lo, hi = node_bounds(j, binary)
    scale = HEIGHT / (hi[1] - lo[1])
    cy = (lo[1] + hi[1]) / 2
    print(f'logo: {lo} .. {hi}, escala {scale:.2f}')

    root = {
        'name': 'Banner',
        'translation': [0, CENTER_Y - cy * scale, 0],
        'scale': [scale] * 3,
        'children': list(j['scenes'][0]['nodes']),
    }
    j['nodes'].append(root)
    root_index = len(j['nodes']) - 1
    j['scenes'] = [{'name': 'Scene', 'nodes': [root_index]}]
    j['scene'] = 0

    # balanceo: giro en Y que va y viene (seno), en bucle
    times, quats = [], []
    for k in range(STEPS + 1):
        t = PERIOD * k / STEPS
        a = math.radians(SWAY_DEG) * math.sin(2 * math.pi * k / STEPS)
        times.append(t)
        quats.append((0.0, math.sin(a / 2), 0.0, math.cos(a / 2)))
    tin = add_accessor(j, binary, struct.pack(f'<{len(times)}f', *times), None, 5126, 'SCALAR',
                       len(times), ([times[0]], [times[-1]]))
    qout = add_accessor(j, binary, b''.join(struct.pack('<4f', *q) for q in quats), None, 5126, 'VEC4',
                        len(quats))
    j['animations'] = [{
        'name': 'Sway',
        'samplers': [{'input': tin, 'output': qout, 'interpolation': 'LINEAR'}],
        'channels': [{'sampler': 0, 'target': {'node': root_index, 'path': 'rotation'}}],
    }]
    # textura más chica (pycgfx la guarda en RGBA4: 256x256 = 128 KB, 128x128 = 32 KB)
    from PIL import Image
    for img in j['images']:
        bv = j['bufferViews'][img['bufferView']]
        off = bv.get('byteOffset', 0)
        im = Image.open(io.BytesIO(bytes(binary[off:off + bv['byteLength']]))).convert('RGBA')
        im = im.resize((TEXTURE_SIZE, TEXTURE_SIZE), Image.LANCZOS)
        out = io.BytesIO()
        im.save(out, 'PNG')
        while len(binary) % 4:
            binary += b'\0'
        j['bufferViews'].append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': len(out.getvalue())})
        binary += out.getvalue()
        img['bufferView'] = len(j['bufferViews']) - 1
    j['buffers'][0]['byteLength'] = len(binary)
    write_glb(dst, j, binary)
    print(f'escrito {dst}')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])

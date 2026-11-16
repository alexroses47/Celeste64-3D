#!/usr/bin/env python3
"""Convierte assets de Celeste 64 al formato del port de 3DS.

Uso: tools/.venv/bin/python -I tools/convert.py <carpeta Content> <carpeta romfs> [mapa]

Replica lo que hace Celeste64/Source/Data/Map.cs (con Sledge.Formats.Map 1.2.x):
  - parsea el .map de TrenchBroom (formato Quake estándar),
  - genera los polígonos de cada brush cortando planos,
  - calcula las UV igual que Map.CalculateRotatedUV/CalculateUV,
  - escala por 0.2 (baseTransform),
y además agrupa todo en trozos para descartar lo que no se ve en la 3DS.
"""
import hashlib
import io
import json
import math
import os
import random
import re
import struct
import subprocess
import sys
from collections import defaultdict

from PIL import Image

SCALE = 0.2               # Map.baseTransform
CHUNK = 100.0             # tamaño del trozo en unidades del mundo (ya escaladas)
TEX3DS = '/opt/devkitpro/tools/bin/tex3ds'
SKY_FACE = 256            # lado de cada cara del skybox en la 3DS
LEVEL_MAGIC = b'C64L'
LEVEL_VERSION = 9

# clases cuyos brushes se dibujan (Map.cs: estáticos, decoraciones y actores IsSolidGeometry)
GEOMETRY_CLASSES = {
    'worldspawn', 'func_group', 'Decoration', 'FloatingDecoration',
}

# sólidos que se mueven o desaparecen (Map.cs: IsSolidGeometry): cada uno es un actor con su malla y colisión locales
DYNAMIC_CLASSES = {
    'FallingBlock': 1, 'FloatyBlock': 2, 'MovingBlock': 3, 'TrafficBlock': 4, 'GateBlock': 5,
    'BreakBlock': 6, 'CassetteBlock': 7, 'DoubleDashPuzzleBlock': 8,
}


def skip_texture(name):
    return name.startswith('__') or name in ('TB_empty', 'invisible')


# ---------------------------------------------------------------- vectores
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    l = math.sqrt(dot(a, a))
    return (a[0] / l, a[1] / l, a[2] / l)


def rotate(v, axis, ang):
    """Igual que Vector3.Transform(v, Matrix.CreateFromAxisAngle(axis, ang))."""
    c, s = math.cos(ang), math.sin(ang)
    return add(add(mul(v, c), mul(cross(axis, v), s)), mul(axis, dot(axis, v) * (1 - c)))


# ---------------------------------------------------------------- parseo .map
FACE_RE = re.compile(r'^\(\s*([^)]+)\)\s*\(\s*([^)]+)\)\s*\(\s*([^)]+)\)\s+(\S+)\s+(.+)$')
KV_RE = re.compile(r'^"([^"]*)"\s+"([^"]*)"$')


def parse_map(path):
    entities, stack = [], []
    for raw in open(path, encoding='utf-8-sig'):
        line = raw.strip()
        if not line or line.startswith('//'):
            continue
        if line == '{':
            if not stack:
                ent = {'props': {}, 'brushes': []}
                entities.append(ent)
                stack.append(ent)
            else:
                brush = []
                stack[-1]['brushes'].append(brush)
                stack.append(brush)
        elif line == '}':
            stack.pop()
        elif isinstance(stack[-1], dict):
            m = KV_RE.match(line)
            if m:
                stack[-1]['props'][m.group(1)] = m.group(2)
        else:
            m = FACE_RE.match(line)
            if not m:
                raise ValueError(f'Cara inválida: {line}')
            pts = [tuple(float(x) for x in m.group(i).split()) for i in (1, 2, 3)]
            nums = [float(x) for x in m.group(5).split()]
            a, b, c = pts
            # Sledge: Plane.CreateFromVertices(c, b, a) -> normal hacia afuera
            n = norm(cross(sub(b, c), sub(a, c)))
            stack[-1].append({
                'n': n, 'd': dot(n, c), 'tex': m.group(4),
                'xshift': nums[0], 'yshift': nums[1], 'rot': nums[2],
                'xscale': nums[3], 'yscale': nums[4],
            })
    return entities


# ---------------------------------------------------------------- geometría de brushes
def face_polygon(face, brush):
    """Polígono convexo de una cara: un cuadrado enorme en su plano recortado por los demás."""
    n, d = face['n'], face['d']
    ref = (0.0, 0.0, 1.0) if abs(n[2]) < 0.9 else (1.0, 0.0, 0.0)
    u = norm(cross(ref, n))
    v = cross(n, u)                     # u x v = n -> el cuadrado queda CCW visto desde afuera
    c = mul(n, d)
    R = 1e6
    poly = [add(c, add(mul(u, sx * R), mul(v, sy * R))) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    for other in brush:
        if other is face:
            continue
        on, od = other['n'], other['d']
        if dot(on, n) > 0.99999 and abs(od - d) < 1e-3:
            continue  # plano duplicado
        out = []
        for i in range(len(poly)):
            p, q = poly[i], poly[(i + 1) % len(poly)]
            dp, dq = dot(on, p) - od, dot(on, q) - od
            if dp <= 1e-4:
                out.append(p)
            if (dp <= 1e-4) != (dq <= 1e-4):
                t = dp / (dp - dq)
                out.append(add(p, mul(sub(q, p), t)))
        poly = out
        if len(poly) < 3:
            return None
    # limpia vértices repetidos
    clean = []
    for p in poly:
        if not clean or dot(sub(p, clean[-1]), sub(p, clean[-1])) > 1e-6:
            clean.append(p)
    if len(clean) > 1 and dot(sub(clean[0], clean[-1]), sub(clean[0], clean[-1])) <= 1e-6:
        clean.pop()
    return clean if len(clean) >= 3 else None


def uv_axes(face):
    """Sledge (idTech2) + Map.CalculateRotatedUV."""
    n = face['n']
    an = tuple(abs(x) for x in n)
    if an[2] >= an[0] and an[2] >= an[1]:
        closest = 'z'
    elif an[0] >= an[1]:
        closest = 'x'
    else:
        closest = 'y'
    uaxis = (0.0, 1.0, 0.0) if closest == 'x' else (1.0, 0.0, 0.0)
    vaxis = (0.0, -1.0, 0.0) if closest == 'z' else (0.0, 0.0, -1.0)
    uaxis = mul(uaxis, 1.0 / face['xscale'])
    vaxis = mul(vaxis, 1.0 / face['yscale'])
    if an[0] > an[1] and an[0] > an[2]:
        rot_axis = (1.0, 0.0, 0.0)
    elif an[1] > an[2]:
        rot_axis = (0.0, 1.0, 0.0)
    else:
        rot_axis = (0.0, 0.0, 1.0)
    ang = math.radians(face['rot'])
    return rotate(uaxis, rot_axis, ang), rotate(vaxis, rot_axis, ang)


# ---------------------------------------------------------------- texturas
def find_textures(content):
    found = {}
    root = os.path.join(content, 'Textures')
    for d, _, files in os.walk(root):
        for f in files:
            if f.lower().endswith('.png'):
                key = os.path.relpath(os.path.join(d, f[:-4]), root).replace(os.sep, '/').lower()
                found[key] = os.path.join(d, f)
    return found


def pot(x):
    p = 8
    while p < x and p < 1024:
        p *= 2
    return p


def alpha_format(img):
    lo, hi = img.getchannel('A').getextrema()
    if lo == 255:
        return 'rgb565'
    hist = img.getchannel('A').histogram()
    partial = sum(hist[1:255])
    return 'rgba4' if partial else 'rgba5551'


def write_t3x(img, out_path, work_dir, mipmaps=True):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    tmp = os.path.join(work_dir, os.path.basename(out_path) + '.png')
    img.save(tmp)
    fmt = alpha_format(img)
    cmd = [TEX3DS, '-f', fmt, '-z', 'auto', '-o', out_path, tmp]
    if mipmaps:
        cmd[1:1] = ['-m', 'box']
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return fmt


def convert_texture(src, out_path, work_dir):
    img = Image.open(src).convert('RGBA')
    w, h = img.size
    pw, ph = pot(w), pot(h)
    if (pw, ph) != (w, h):
        img = img.resize((pw, ph), Image.NEAREST)
    fmt = write_t3x(img, out_path, work_dir)
    return (w, h), (pw, ph), fmt


def convert_skybox(src, out_dir, name, work_dir):
    """Corta el skybox en 6 caras (Skybox.cs: bloques de ancho/4)."""
    img = Image.open(src).convert('RGBA')
    block = img.width // 4
    faces = {'u': (0, 0), 'd': (0, 2), 'n': (0, 1), 'e': (1, 1), 's': (2, 1), 'w': (3, 1)}
    for key, (bx, by) in faces.items():
        face = img.crop((bx * block, by * block, (bx + 1) * block, (by + 1) * block))
        face = face.resize((SKY_FACE, SKY_FACE), Image.BOX)
        write_t3x(face, os.path.join(out_dir, f'{name}_{key}.t3x'), work_dir, mipmaps=False)


# ---------------------------------------------------------------- matrices (vector columna: M @ v)
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat_identity():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def mat_translate(x, y, z):
    m = mat_identity()
    m[0][3], m[1][3], m[2][3] = x, y, z
    return m


def mat_scale(x, y=None, z=None):
    y = x if y is None else y
    z = x if z is None else z
    m = mat_identity()
    m[0][0], m[1][1], m[2][2] = x, y, z
    return m


def mat_rot(axis, ang):
    """Rotación estándar (igual dirección que Matrix.CreateRotationX/Y/Z de System.Numerics)."""
    c, s = math.cos(ang), math.sin(ang)
    m = mat_identity()
    i, j = {'x': (1, 2), 'y': (2, 0), 'z': (0, 1)}[axis]
    m[i][i], m[i][j], m[j][i], m[j][j] = c, -s, s, c
    return m


def mat_quat(q):
    x, y, z, w = q
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
        [0, 0, 0, 1],
    ]


def xform_point(m, p):
    return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] for i in range(3))


def normal_matrix(m):
    """Inversa transpuesta de la parte 3x3 (para normales)."""
    a = [row[:3] for row in m[:3]]
    cof = [[(a[(i + 1) % 3][(j + 1) % 3] * a[(i + 2) % 3][(j + 2) % 3] -
             a[(i + 1) % 3][(j + 2) % 3] * a[(i + 2) % 3][(j + 1) % 3]) for j in range(3)] for i in range(3)]
    return cof   # proporcional a la inversa transpuesta; se normaliza después


def xform_normal(nm, n):
    r = tuple(nm[i][0] * n[0] + nm[i][1] * n[1] + nm[i][2] * n[2] for i in range(3))
    l = math.sqrt(dot(r, r)) or 1.0
    return (r[0] / l, r[1] / l, r[2] / l)


# ---------------------------------------------------------------- glTF (.glb)
COMPONENTS = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}
TYPE_SIZES = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def read_accessor(j, binary, idx):
    acc = j['accessors'][idx]
    view = j['bufferViews'][acc['bufferView']]
    fmt = COMPONENTS[acc['componentType']]
    n = TYPE_SIZES[acc['type']]
    size = struct.calcsize(fmt)
    stride = view.get('byteStride', size * n)
    base = view.get('byteOffset', 0) + acc.get('byteOffset', 0)
    out = []
    for i in range(acc['count']):
        vals = struct.unpack_from(f'<{n}{fmt}', binary, base + i * stride)
        out.append(vals if n > 1 else vals[0])
    return out


def load_glb(path):
    """Primitivas en el espacio del modelo (nodos ya aplicados): [(pos, nrm, uv, idx, imagen)]."""
    b = open(path, 'rb').read()
    jlen = struct.unpack_from('<I', b, 12)[0]
    j = json.loads(b[20:20 + jlen])
    boff = 20 + jlen
    binary = b[boff + 8: boff + 8 + struct.unpack_from('<I', b, boff)[0]]

    images = []
    for img in j.get('images', []):
        view = j['bufferViews'][img['bufferView']]
        o = view.get('byteOffset', 0)
        images.append(binary[o:o + view['byteLength']])

    def material_image(mi):
        if mi is None:
            return None
        mat = j['materials'][mi]
        tex = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture')
        if tex is None:
            return None
        return images[j['textures'][tex['index']]['source']]

    def local(node):
        if 'matrix' in node:
            m = node['matrix']
            return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
        t = node.get('translation', [0, 0, 0])
        q = node.get('rotation', [0, 0, 0, 1])
        s = node.get('scale', [1, 1, 1])
        return mat_mul(mat_mul(mat_translate(*t), mat_quat(q)), mat_scale(*s))

    prims = []

    def walk(ni, parent):
        node = j['nodes'][ni]
        world = mat_mul(parent, local(node))
        if 'mesh' in node:
            if 'skin' in node:
                raise ValueError(f'{path}: mallas con huesos todavía no soportadas')
            nm = normal_matrix(world)
            for p in j['meshes'][node['mesh']]['primitives']:
                a = p['attributes']
                pos = [xform_point(world, v) for v in read_accessor(j, binary, a['POSITION'])]
                nrm = [xform_normal(nm, v) for v in read_accessor(j, binary, a['NORMAL'])]
                uv = read_accessor(j, binary, a['TEXCOORD_0'])
                idx = read_accessor(j, binary, p['indices'])
                prims.append((pos, nrm, uv, idx, material_image(p.get('material'))))
        for c in node.get('children', []):
            walk(c, world)

    scene = j['scenes'][j.get('scene', 0)]
    for ni in scene['nodes']:
        walk(ni, mat_identity())
    return prims


def load_glb_with_materials(j, binary):
    """Primitivas con los nodos aplicados y el índice de material: [(pos, nrm, uv, idx, material)]."""
    def local(node):
        if 'matrix' in node:
            m = node['matrix']
            return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
        t = node.get('translation', [0, 0, 0])
        q = node.get('rotation', [0, 0, 0, 1])
        s_ = node.get('scale', [1, 1, 1])
        return mat_mul(mat_mul(mat_translate(*t), mat_quat(q)), mat_scale(*s_))

    prims = []

    def walk(ni, parent):
        node = j['nodes'][ni]
        world = mat_mul(parent, local(node))
        if 'mesh' in node:
            nm = normal_matrix(world)
            for p in j['meshes'][node['mesh']]['primitives']:
                a = p['attributes']
                pos = [xform_point(world, v) for v in read_accessor(j, binary, a['POSITION'])]
                nrm = [xform_normal(nm, v) for v in read_accessor(j, binary, a['NORMAL'])]
                uv = read_accessor(j, binary, a['TEXCOORD_0'])
                idx = read_accessor(j, binary, p['indices'])
                prims.append((pos, nrm, uv, idx, p.get('material', 0)))
        for ch in node.get('children', []):
            walk(ch, world)

    for ni in j['scenes'][j.get('scene', 0)]['nodes']:
        walk(ni, mat_identity())
    return prims


# ---------------------------------------------------------------- rayos contra brushes
def ray_hits_brush(o, d, maxdist, brush):
    tmin, tmax = 0.0, maxdist
    for f in brush:
        denom = dot(f['n'], d)
        dist = dot(f['n'], o) - f['d']
        if abs(denom) < 1e-9:
            if dist > 0:
                return False
            continue
        t = -dist / denom
        if denom < 0:
            tmin = max(tmin, t)
        else:
            tmax = min(tmax, t)
        if tmin > tmax:
            return False
    return True


def solid_raycast(solids, origin, direction, dist):
    """World.SolidRayCast aproximado: rayo (en unidades del mundo) contra los brushes sólidos."""
    o = mul(origin, 1.0 / SCALE)
    return any(ray_hits_brush(o, direction, dist / SCALE, b) for b in solids)


def brush_bounds(brush):
    pts = []
    for face in brush:
        poly = face_polygon(face, brush)
        if poly:
            pts.extend(poly)
    lo = tuple(min(p[i] for p in pts) * SCALE for i in range(3))
    hi = tuple(max(p[i] for p in pts) * SCALE for i in range(3))
    return lo, hi


# ---------------------------------------------------------------- nivel
class TextureBank:
    """Asigna índices a texturas y las convierte a .t3x la primera vez que se usan."""

    def __init__(self, content_textures, romfs, work_dir):
        self.files = content_textures
        self.romfs, self.work = romfs, work_dir
        self.index, self.sizes = {}, {}

    def by_name(self, name):
        name = name.lower()
        if name not in self.files:
            return None
        return self._add(name, self.files[name])

    def by_bytes(self, data):
        key = 'm_' + hashlib.sha1(data).hexdigest()[:12]
        return self._add(key, io.BytesIO(data))

    def _add(self, key, src):
        if key not in self.index:
            orig, _, _ = convert_texture(src, os.path.join(self.romfs, 'tex', key + '.t3x'), self.work)
            self.index[key] = len(self.index)
            self.sizes[key] = orig
        return key


DETAIL_GROUP = 0x8000   # pinchos y props: trozos aparte que la 3DS deja de dibujar a cierta distancia


def chunk_key(center, group=0):
    # group 0 = escenario fijo; 1..N = grupos de FloatingDecoration que suben y bajan
    return (group,) + tuple(int(math.floor(c / CHUNK)) for c in center)


def convert_level(content, romfs, map_name, work_dir, bank):
    path = os.path.join(content, 'Maps', map_name + '.map')
    entities = parse_map(path)
    world = entities[0]['props']
    textures = bank.files

    spawn, spawn_angle = (0.0, 0.0, 0.0), 0.0
    checkpoints = []   # (pos, ángulo), con "Start" primero
    brushes, solids, props, spikes = [], [], [], []
    collision_groups = []          # cada grupo = un Solid del original (brush estático o actor sólido)
    volumes = []                   # (tipo, min, max, extra): 0 muerte, 1 pinchos (extra = dirección), 2 cámara fija (extra = punto), 3 empuje de prop (extra = radio, altura)
    # Map.FindTargetEntity ignora los nombres vacíos (hay fresas con targetname "")
    targets = {e['props']['targetname']: e for e in entities if e['props'].get('targetname')}
    floating, strawberries, dynamic, objects, cars = [], [], [], [], []
    raw_collision = []   # (triángulos locales, actor): colisiones que vienen de modelos (IntroCar)
    group_names = {e['props'].get('_tb_id'): e['props'].get('_tb_name', '') for e in entities
                   if e['props'].get('classname') == 'func_group'}
    for ent in entities:
        pr = ent['props']
        cls = pr.get('classname', '')
        if cls == 'PlayerSpawn':
            pos = tuple(float(x) * SCALE for x in pr['origin'].split())
            ang = float(pr.get('angle', 0))
            if pr.get('name', 'Start') == 'Start':
                spawn, spawn_angle = pos, ang
                checkpoints.insert(0, (pos, ang))
            else:
                checkpoints.append((pos, ang))
        elif cls == 'StaticProp':
            props.append(pr)
        elif cls == 'SpikeBlock' and ent['brushes']:
            spikes.append(ent['brushes'][0])
        elif cls == 'DeathBlock' and ent['brushes']:
            volumes.append((0, *brush_bounds(ent['brushes'][0]), (0.0, 0.0, 0.0), -1))
        elif cls == 'FixedCamera' and ent['brushes']:
            tgt = targets.get(pr.get('target', ''))
            point = tuple(float(x) * SCALE for x in tgt['props'].get('origin', '0 0 0').split()) if tgt else (0.0, 0.0, 0.0)
            volumes.append((2, *brush_bounds(ent['brushes'][0]), point, -1))
        elif cls == 'FloatingDecoration':
            floating.append(ent)
        elif cls == 'Strawberry':
            strawberries.append(pr)
        elif cls == 'IntroCar':
            cars.append(pr)
        elif cls == 'EndingArea' and ent['brushes']:
            volumes.append((4, *brush_bounds(ent['brushes'][0]), (0.0, 0.0, 0.0), -1))
        elif cls in ('Spring', 'Refill', 'Feather', 'Granny', 'Theo', 'Badeline', 'SignPost', 'Cassette', 'Coin'):
            objects.append(pr)
        elif cls in DYNAMIC_CLASSES and ent['brushes']:
            dynamic.append(ent)
            solids.extend(ent['brushes'])
        if cls in GEOMETRY_CLASSES:
            brushes.extend(ent['brushes'])
            if cls not in ('Decoration', 'FloatingDecoration'):
                solids.extend(ent['brushes'])
                collision_groups.extend(([b], -1, (0.0, 0.0, 0.0)) for b in ent['brushes'])

    # Map.cs: las FloatingDecoration se mezclan al azar y se reparten en grupos de count/4
    brush_group = {}
    random.Random(64).shuffle(floating)
    per_group = max(1, len(floating) // 4)
    num_float_groups = 0
    for start in range(0, len(floating), per_group):
        num_float_groups += 1
        for ent in floating[start:start + per_group]:
            for b in ent['brushes']:
                brush_group[id(b)] = num_float_groups

    # chunks[clave][textura] = [(vértices, índices)], vértice = (pos, (u, t), normal)
    chunks = defaultdict(lambda: defaultdict(list))
    missing = set()
    stats = defaultdict(int)

    # ---- brushes
    for brush in brushes:
        polys = []
        for face in brush:
            if skip_texture(face['tex']):
                continue
            poly = face_polygon(face, brush)
            if poly:
                polys.append((face, poly))
        if not polys:
            continue
        pts = [p for _, poly in polys for p in poly]
        center = mul(add(tuple(map(min, zip(*pts))), tuple(map(max, zip(*pts)))), 0.5 * SCALE)
        key = chunk_key(center, brush_group.get(id(brush), 0))
        for face, poly in polys:
            tex = bank.by_name(face['tex'])
            if tex is None:
                missing.add(face['tex'])
                continue
            w, h = bank.sizes[tex]
            ru, rv = uv_axes(face)
            out = []
            for p in poly:
                u = (dot(p, ru) + face['xshift']) / w
                v = (dot(p, rv) + face['yshift']) / h
                out.append((mul(p, SCALE), (u, 1.0 - v), face['n']))
            fan = [k for i in range(len(out) - 2) for k in (0, i + 1, i + 2)]
            chunks[key][tex].append((out, fan))
            stats['caras de brushes'] += 1

    # ---- sólidos dinámicos: malla relativa al centro (Map.GenerateSolid: Position = centro de los límites)
    dynamic_info = []
    for k, ent in enumerate(dynamic):
        pr = ent['props']
        pts = []
        for brush in ent['brushes']:
            for face in brush:
                poly = face_polygon(face, brush)
                if poly:
                    pts.extend(mul(p, SCALE) for p in poly)
        lo = tuple(min(p[i] for p in pts) for i in range(3))
        hi = tuple(max(p[i] for p in pts) for i in range(3))
        center = mul(add(lo, hi), 0.5)
        key = (0x10000 + k, 0, 0, 0)
        for brush in ent['brushes']:
            for face in brush:
                if skip_texture(face['tex']):
                    continue
                poly = face_polygon(face, brush)
                if not poly:
                    continue
                tex = bank.by_name(face['tex'])
                if tex is None:
                    missing.add(face['tex'])
                    continue
                w, h = bank.sizes[tex]
                ru, rv = uv_axes(face)
                out = []
                for p in poly:
                    u = (dot(p, ru) + face['xshift']) / w
                    v = (dot(p, rv) + face['yshift']) / h
                    out.append((sub(mul(p, SCALE), center), (u, 1.0 - v), face['n']))
                fan = [k2 for i in range(len(out) - 2) for k2 in (0, i + 1, i + 2)]
                chunks[key][tex].append((out, fan))
        collision_groups.append((ent['brushes'], k, center))
        tgt = targets.get(pr.get('target', ''))
        end = tuple(float(x) * SCALE for x in tgt['props'].get('origin', '0 0 0').split()) if tgt else center
        flags = 0
        if int(float(pr.get('slow', 0))) > 0: flags |= 1
        if int(float(pr.get('secret', 0))) != 0: flags |= 2
        if int(float(pr.get('bounces', 0))) != 0: flags |= 4
        if int(float(pr.get('transparent', 0))) != 0: flags |= 8
        if int(float(pr.get('startOn', 1))) != 0: flags |= 16
        group = group_names.get(pr.get('_tb_group'), '')
        dynamic_info.append((DYNAMIC_CLASSES[pr['classname']], center, end, flags, group))
        stats['sólidos dinámicos'] += 1

    # ---- modelos colocados en el mundo
    model_cache = {}

    def model(name):
        if name not in model_cache:
            model_cache[name] = load_glb(os.path.join(content, 'Models', name + '.glb'))
        return model_cache[name]

    def place(prims, matrix, key, drop_bottom=False):
        nm = normal_matrix(matrix)
        for pos, nrm, uv, idx, image in prims:
            if image is None:
                continue
            tex = bank.by_bytes(image)
            idx = list(idx)
            if drop_bottom:
                # la cara de abajo del pincho queda contra la pared: nunca se ve
                keep = []
                for i in range(0, len(idx), 3):
                    a, b, c_ = (pos[k] for k in idx[i:i + 3])
                    if norm(cross(sub(b, a), sub(c_, a)))[2] > -0.9:
                        keep += idx[i:i + 3]
                idx = keep
            used = sorted(set(idx))
            remap = {k: i for i, k in enumerate(used)}
            wp = [xform_point(matrix, pos[k]) for k in used]
            wn = [xform_normal(nm, nrm[k]) for k in used]
            vs = [(wp[i], (uv[k][0], 1.0 - uv[k][1]), wn[i]) for i, k in enumerate(used)]
            chunks[key][tex].append((vs, [remap[k] for k in idx]))

    # StaticProp: v * Nodo * RotX(90°) * Escala(0.2) * RotZ(ángulo) * Traslación
    for pr in props:
        name = os.path.splitext(os.path.basename(pr.get('model', '')))[0]
        if not os.path.exists(os.path.join(content, 'Models', name + '.glb')):
            missing.add(name + '.glb')
            continue
        pos = tuple(float(x) * SCALE for x in pr.get('origin', '0 0 0').split())
        ang = math.radians(float(pr.get('angle', 0)))
        # StaticProp: IHavePushout con radio y altura de la entidad (por defecto 6 y 10)
        volumes.append((3, pos, pos, (float(pr.get('radius', 6)), float(pr.get('height', 10)), 0.0), -1))
        m = mat_mul(mat_translate(*pos), mat_mul(mat_rot('z', ang), mat_mul(mat_scale(SCALE), mat_rot('x', math.pi / 2))))
        place(model(name), m, chunk_key(pos, DETAIL_GROUP))
        stats['props'] += 1

    # SpikeBlock (SpikeBlock.Added): rejilla de "spike" sobre la cara del bloque
    step = 6.4
    for brush in spikes:
        lo, hi = brush_bounds(brush)
        center = mul(add(lo, hi), 0.5)
        size = sub(hi, lo)
        if size[2] < size[0] and size[2] < size[1]:
            fwd, horiz, vert, rot = (0, 0, 1), (1, 0, 0), (0, 1, 0), mat_identity()
        elif size[0] < size[1]:
            fwd, horiz, vert, rot = (-1, 0, 0), (0, 0, 1), (0, 1, 0), mat_rot('y', -math.pi / 2)
        else:
            fwd, horiz, vert, rot = (0, -1, 0), (1, 0, 0), (0, 0, 1), mat_rot('x', math.pi / 2)
        if solid_raycast(solids, center, fwd, 10):
            fwd = mul(fwd, -1)
            rot = mat_rot('y', math.pi / 2) if fwd[0] != 0 else mat_rot('x', -math.pi / 2)
        width = math.sqrt(dot(tuple(a * b for a, b in zip(horiz, size)), tuple(a * b for a, b in zip(horiz, size))))
        height = math.sqrt(dot(tuple(a * b for a, b in zip(vert, size)), tuple(a * b for a, b in zip(vert, size))))
        cols = max(1, round(width / step))
        rows = max(1, round(height / step))
        # Attacher.Added: rayo desde el borde del bloque hacia la pared; si toca un sólido dinámico, viaja con él
        attach = -1
        normal = mul(fwd, -1)
        origin = sub(center, mul(tuple(a * b for a, b in zip(size, fwd)), 0.5))
        for k, ent in enumerate(dynamic):
            if solid_raycast(ent['brushes'], sub(origin, normal), normal, 5):
                attach = k
                break
        volumes.append((1, lo, hi, fwd, attach))
        # trozo por dirección (0x8001..0x8006 = +x,-x,+y,-y,+z,-z): la 3DS descarta los pinchos
        # que apuntan al lado contrario de la cámara (quedan detrás de su propia pared)
        dircode = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)].index(tuple(int(v) for v in fwd))
        key = chunk_key(center, DETAIL_GROUP + 1 + dircode) if attach < 0 else (0x10000 + attach, 0, 0, 0)
        shift = (0.0, 0.0, 0.0) if attach < 0 else dynamic_info[attach][1]
        for x in range(cols):
            for y in range(rows):
                off = add(add(mul(horiz, (x + 0.5) * step - width / 2), mul(vert, (y + 0.5) * step - height / 2)),
                          mul(fwd, -step / 2))
                t = sub(add(center, off), shift)
                m = mat_mul(mat_translate(*t), mat_mul(rot, mat_scale(2.5)))
                place(model('spike'), m, key, drop_bottom=True)
                stats['pinchos'] += 1

    # ---- IntroCar: carrocería que se hunde (sólido dinámico), ruedas fijas y colisión de car_collider
    for pr in cars:
        k = len(dynamic_info)
        pos = tuple(float(x) * SCALE for x in pr.get('origin', '0 0 0').split())
        pos = (pos[0], pos[1], pos[2] - 1.3)   # IntroCar.Added: Position += -UnitZ * 1.3
        sc = float(pr.get('scale', 6))
        ang = math.radians(float(pr.get('angle', 0)))
        local = mat_mul(mat_rot('z', ang), mat_mul(mat_scale(sc), mat_rot('x', math.pi / 2)))
        place(model('car_top'), local, (0x10000 + k, 0, 0, 0))
        place(model('car_wheels'), mat_mul(mat_translate(*pos), local), chunk_key(pos))
        tris = []
        for cpos, _, _, cidx, _ in model('car_collider'):
            w = [xform_point(local, v) for v in cpos]
            for i in range(0, len(cidx), 3):
                tris.append((w[cidx[i]], w[cidx[i + 1]], w[cidx[i + 2]]))
        raw_collision.append((tris, k))
        dynamic_info.append((9, pos, pos, 8, ''))   # bandera 8 = Transparent (la cámara lo ignora)
        stats['coches'] += 1

    # ---- vértices e índices por trozo
    verts, indices, chunk_recs, part_recs = bytearray(), [], [], []
    nverts = tris_total = 0
    for key in sorted(chunks):
        vstart, pstart = nverts, len(part_recs)
        lo, hi = [1e30] * 3, [-1e30] * 3
        local = 0
        for tex, entries in sorted(chunks[key].items(), key=lambda kv: bank.index[kv[0]]):
            istart = len(indices)
            for vs, idx in entries:
                base = local
                for p, (u, t), n in vs:
                    for i in range(3):
                        lo[i] = min(lo[i], p[i])
                        hi[i] = max(hi[i], p[i])
                    nb = [max(-127, min(127, round(c * 127))) for c in n]
                    verts += struct.pack('<5f4b', p[0], p[1], p[2], u, t, nb[0], nb[1], nb[2], 0)
                    local += 1
                indices += [base + k for k in idx]
                tris_total += len(idx) // 3
            part_recs.append((bank.index[tex], istart, len(indices) - istart))
        if local > 65535:
            raise ValueError(f'Trozo {key} con {local} vértices: bajar CHUNK')
        nverts += local
        chunk_recs.append((lo, hi, vstart, local, pstart, len(part_recs) - pstart, key[0]))

    if len(indices) % 2:
        indices.append(0)   # alinea a 4 bytes

    out = bytearray()
    names = sorted(bank.index, key=bank.index.get)
    out += struct.pack('<4sIIIIIIffffI', LEVEL_MAGIC, LEVEL_VERSION, len(names), len(chunk_recs),
                       len(part_recs), nverts, len(indices), spawn[0], spawn[1], spawn[2], spawn_angle,
                       len(checkpoints))
    out += (world.get('skybox', 'city').lower().encode().ljust(32, b'\0'))
    for pos, ang in checkpoints:
        out += struct.pack('<4f', pos[0], pos[1], pos[2], ang)
    out += struct.pack('<I', num_float_groups)
    # Snow: worldspawn snowAmount / snowDirection
    snow_amount = float(world.get('snowAmount', 1))
    snow_dir = tuple(float(x) for x in world.get('snowDirection', '0 0 -1').split())
    out += struct.pack('<4f', snow_amount, *snow_dir)
    # música y ambiente del mapa (worldspawn)
    out += world.get('music', '').encode()[:31].ljust(32, b'\0')
    out += world.get('ambience', '').encode()[:31].ljust(32, b'\0')
    for n in names:
        out += n.encode().ljust(32, b'\0')
    for lo, hi, vstart, vcount, pstart, pcount, group in chunk_recs:
        out += struct.pack('<6f5I', *lo, *hi, vstart, vcount, pstart, pcount, group)
    for tex, istart, icount in part_recs:
        out += struct.pack('<3I', tex, istart, icount)
    out += verts
    out += struct.pack(f'<{len(indices)}H', *indices)

    # ---- colisión (Map.GenerateSolid): caras sin texturas "__*" ni TB_empty
    col_solids, col_verts, col_faces = [], [], []
    for group, actor, gcenter in collision_groups:
        vstart, fstart = len(col_verts), len(col_faces)
        for brush in group:
            for face in brush:
                if face['tex'].startswith('__') or face['tex'] == 'TB_empty':
                    continue
                poly = face_polygon(face, brush)
                if not poly:
                    continue
                fv = len(col_verts)
                last = None
                for p in poly:
                    w = sub(mul(p, SCALE), gcenter)
                    # salta vértices consecutivos demasiado cercanos (como el original)
                    if last is None or dot(sub(last, w), sub(last, w)) > 1:
                        col_verts.append(w)
                        last = w
                if len(col_verts) > fv:
                    n = face['n']
                    d = -(face['d'] * SCALE - dot(n, gcenter))   # System.Numerics: dot(n, p) + D = 0 (en local)
                    col_faces.append((n, d, fv, len(col_verts) - fv))
        if len(col_verts) == vstart:
            continue
        pts = col_verts[vstart:]
        lo = tuple(min(p[i] for p in pts) for i in range(3))
        hi = tuple(max(p[i] for p in pts) for i in range(3))
        col_solids.append((lo, hi, fstart, len(col_faces) - fstart, actor))

    for tris, actor in raw_collision:
        fstart = len(col_faces)
        vs = len(col_verts)
        for a, b, c_ in tris:
            n = norm(cross(sub(b, a), sub(c_, a)))   # Plane.CreateFromVertices
            fv = len(col_verts)
            col_verts.extend((a, b, c_))
            col_faces.append((n, -dot(n, a), fv, 3))
        pts = col_verts[vs:]
        lo = tuple(min(p[i] for p in pts) for i in range(3))
        hi = tuple(max(p[i] for p in pts) for i in range(3))
        col_solids.append((lo, hi, fstart, len(col_faces) - fstart, actor))

    out += struct.pack('<III', len(col_solids), len(col_verts), len(col_faces))
    for lo, hi, fstart, fcount, actor in col_solids:
        out += struct.pack('<6f2Ii', *lo, *hi, fstart, fcount, actor)
    for v in col_verts:
        out += struct.pack('<3f', *v)
    for n, d, vstart, vcount in col_faces:
        out += struct.pack('<4f2I', *n, d, vstart, vcount)
    out += struct.pack('<I', len(volumes))
    for kind, lo, hi, extra, attach in volumes:
        out += struct.pack('<I9fi', kind, *lo, *hi, *extra, attach)

    # ---- actores: (tipo, posición, ángulo, banderas, sólido dinámico al que van pegados, texto)
    #   1 fresa (banderas: 1 bloqueada, 2 sin sonido; texto = grupo que la desbloquea)
    #   2 resorte, 3 cristal (bandera 1 = doble), 4 pluma, 6 abuela, 7 Theo, 8 Badeline, 9 letrero (texto = diálogo)
    #   10 cassette (texto = mapa), 11 moneda; extra = destino de la burbuja de la fresa (bandera 4)
    kinds = {'Spring': 2, 'Refill': 3, 'Feather': 4, 'Granny': 6, 'Theo': 7, 'Badeline': 8, 'SignPost': 9,
             'Cassette': 10, 'Coin': 11}
    records = []
    for pr in strawberries:
        flags = 1 if int(float(pr.get('locked', 0))) > 0 else 0
        if int(float(pr.get('noUnlockSound', 0))) != 0: flags |= 2
        extra = (0.0, 0.0, 0.0)
        tgt = targets.get(pr.get('bubbleto', ''))
        if tgt:
            flags |= 4
            extra = tuple(float(x) * SCALE for x in tgt['props'].get('origin', '0 0 0').split())
        records.append((1, pr, flags, -1, pr.get('targetname', ''), extra))
    for pr in objects:
        cls = pr['classname']
        pos = tuple(float(x) * SCALE for x in pr.get('origin', '0 0 0').split())
        attach = -1
        if cls == 'Spring':
            # Attacher.Added: rayo de 5 hacia abajo desde un poco arriba
            o = mul(add(pos, (0.0, 0.0, 1.0)), 1.0 / SCALE)
            for k, ent in enumerate(dynamic):
                if any(ray_hits_brush(o, (0.0, 0.0, -1.0), 5 / SCALE, b) for b in ent['brushes']):
                    attach = k
                    break
        flags = 1 if cls == 'Refill' and int(float(pr.get('double', 0))) > 0 else 0
        text = pr.get('map', '') if cls == 'Cassette' else pr.get('dialog', '')
        records.append((kinds[cls], pr, flags, attach, text, (0.0, 0.0, 0.0)))
    out += struct.pack('<I', len(records))
    for kind, pr, flags, attach, text, extra in records:
        pos = tuple(float(x) * SCALE for x in pr.get('origin', '0 0 0').split())
        out += struct.pack('<I4fIi3f', kind, *pos, float(pr.get('angle', 0)), flags, attach, *extra)
        out += text.encode()[:31].ljust(32, b'\0')
    stats['objetos'] = len(objects)
    stats['fresas'] = len(strawberries)

    # ---- sólidos dinámicos: (tipo, centro, destino, banderas, grupo)
    out += struct.pack('<I', len(dynamic_info))
    for kind, center, end, flags, group in dynamic_info:
        out += struct.pack('<I3f3fI', kind, *center, *end, flags) + group.encode()[:31].ljust(32, b'\0')
    stats['grupos flotantes'] = num_float_groups
    stats['sólidos de colisión'] = len(col_solids)
    stats['caras de colisión'] = len(col_faces)
    stats['volúmenes'] = len(volumes)
    os.makedirs(os.path.join(romfs, 'levels'), exist_ok=True)
    with open(os.path.join(romfs, 'levels', map_name + '.lvl'), 'wb') as f:
        f.write(out)

    sky = world.get('skybox', 'city').lower()
    sky_layers = [k for k in textures if re.fullmatch(rf'skyboxes/{re.escape(sky)}(_\d+)?', k)]
    for k in sky_layers:
        convert_skybox(textures[k], os.path.join(romfs, 'tex'), 'sky_' + k.split('/')[-1], work_dir)

    print(f'{map_name}: {len(brushes)} brushes, ' + ', '.join(f'{v} {k}' for k, v in stats.items()) +
          f'; {tris_total} triángulos, {nverts} vértices, {len(chunk_recs)} trozos, {len(part_recs)} partes, '
          f'{len(names)} texturas, {len(out) / 1024:.0f} KB')
    print(f'  inicio en {tuple(round(c, 1) for c in spawn)}, ángulo {spawn_angle}, {len(checkpoints)} checkpoints, '
          f'skybox {sky} ({len(sky_layers)} capas)')
    if missing:
        print('  no encontrados (se omiten):', ', '.join(sorted(missing)))


def convert_dialog(content, romfs, work_dir):
    """Language.Dialog (inglés) a romfs:/dialog.txt y los retratos de Content/Faces a texturas face_*."""
    data = json.load(open(os.path.join(content, 'Text', 'English.json'), encoding='utf-8-sig'))
    with open(os.path.join(romfs, 'dialog.txt'), 'w', encoding='utf-8') as f:
        for key, lines in data['Dialog'].items():
            f.write('@' + key + '\n')
            for line in lines:
                f.write(line.get('Face', '') + '|' + line.get('Voice', '') + '|' + line['Text'].replace('\n', '\\n') + '\n')
    faces_root = os.path.join(content, 'Faces')
    count = 0
    for d, _, files in os.walk(faces_root):
        for fn in files:
            if not fn.endswith('.png'):
                continue
            rel = os.path.relpath(os.path.join(d, fn[:-4]), faces_root).replace(os.sep, '/')
            img = Image.open(os.path.join(d, fn)).convert('RGBA').resize((128, 128), Image.BOX)
            write_t3x(img, os.path.join(romfs, 'tex', 'face_' + rel.replace('/', '_') + '.t3x'), work_dir, mipmaps=False)
            count += 1
    print(f'diálogos: {len(data["Dialog"])} conversaciones, {count} retratos')


def convert_font(content, romfs, work_dir, size=12, chars='0123456789x', name='font_digits'):
    """Glifos de Renogare (UI.Text) en filas: textura <name> + métricas en romfs:/<name>.bin."""
    from PIL import ImageDraw, ImageFont
    font = ImageFont.truetype(os.path.join(content, 'Fonts', 'Renogare.otf'), size)
    ascent, descent = font.getmetrics()
    height = ascent + descent
    row_h = height + 4          # espacio para el contorno (2 px abajo)
    width = 256 if len(chars) > 16 else 128
    glyphs, x, y = [], 1, 0
    for ch in chars:
        l, t, r, b = font.getbbox(ch) if ch.strip() else (0, 0, 0, 0)
        w = max(r - l + 1, 1)
        if x + w + 2 > width:
            x, y = 1, y + row_h
        glyphs.append((ch, x, y, w, l, font.getlength(ch)))
        x += w + 4
    img = Image.new('RGBA', (width, pot(y + row_h)), (255, 255, 255, 0))
    d = ImageDraw.Draw(img)
    for ch, gx, gy, w, l, adv in glyphs:
        if ch.strip():
            d.text((gx - l, gy + 1), ch, font=font, fill=(255, 255, 255, 255))
    write_t3x(img, os.path.join(romfs, 'tex', name + '.t3x'), work_dir, mipmaps=False)
    # contorno de UI.Text ya dibujado (desplazamientos x -1..1, y -1..2): una sola pasada en la 3DS
    from PIL import ImageChops
    alpha = img.getchannel('A')
    outline = Image.new('L', img.size, 0)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1, 2):
            outline = ImageChops.lighter(outline, ImageChops.offset(alpha, dx, dy))
    shadow = Image.new('RGBA', img.size, (0, 0, 0, 0))
    shadow.putalpha(outline)
    write_t3x(shadow, os.path.join(romfs, 'tex', name + '_outline.t3x'), work_dir, mipmaps=False)
    # cabecera: cantidad, ancho, alto, alto de línea; glifo: código, x, y, ancho, avance
    out = struct.pack('<4I', len(glyphs), img.width, img.height, row_h)
    for ch, gx, gy, w, l, adv in glyphs:
        out += struct.pack('<I4f', ord(ch), gx, gy, w, adv)
    with open(os.path.join(romfs, name + '.bin'), 'wb') as f:
        f.write(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    content, romfs = sys.argv[1], sys.argv[2]
    map_name = sys.argv[3] if len(sys.argv) > 3 else '1'
    work = os.path.join(os.path.dirname(os.path.abspath(romfs)), 'build', 'convert')
    os.makedirs(work, exist_ok=True)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import skinned
    bank = TextureBank(find_textures(content), romfs, work)
    # el nivel 1 y sus 10 niveles extra (cassettes)
    maps = [map_name] if len(sys.argv) > 3 else ['1'] + ['1-%d' % i for i in range(1, 11)]
    for m in maps:
        convert_level(content, romfs, m, work, bank)
    # las texturas de los modelos van después de las del nivel (el nivel guarda sus índices)
    skinned.convert_skinned(content, 'player', bank, romfs)
    for name in ('strawberry', 'spring_board', 'refill_gem', 'refill_gem_double', 'feather', 'flag_on', 'flag_off',
                 'granny', 'theo', 'badeline', 'sign', 'logo', 'tape_1', 'tape_2', 'coin'):
        skinned.convert_skinned(content, name, bank, romfs)
    for name in ('circle', 'gradient', 'splash', 'icon_strawberry', 'icon_stopwatch', 'icon_skull', 'ring',
                 'particle-star', 'interact', 'bubble', 'dust_0', 'dust_1', 'dust_2', 'dust_3', 'dust_4',
                 'shard_0', 'shard_1', 'shard_2', 'wood_shard_0', 'wood_shard_1', 'wood_shard_2'):
        bank._add('spr_' + name, os.path.join(content, 'Sprites', name + '.png'))
    # pantalla de título y mapa (Titlescreen.cs / Overworld.cs)
    ui = {
        'ui_overlay': ('Textures', 'Overworld/overlay'), 'ui_vignette': ('Textures', 'Overworld/vignette'),
        'ui_complete': ('Textures', 'Overworld/strawberry'), 'ui_postcard': ('Textures', 'Postcards/ForsakenCity'),
        'ui_postcard_back': ('Textures', 'Postcards/back'),
        'ui_confirm': ('Sprites', 'Controls/Nintendo Switch/confirm'), 'ui_cancel': ('Sprites', 'Controls/Nintendo Switch/cancel'),
    }
    for key, (folder, rel) in ui.items():
        src = os.path.join(content, folder, rel + '.png')
        img = Image.open(src).convert('RGBA')
        # a potencia de 2 (se estira; al dibujar se usa el aspecto original)
        w = 512 if img.width > 256 else pot(img.width)
        h = 512 if img.height > 256 else pot(img.height)
        if key in ('ui_overlay', 'ui_vignette'):
            w, h = 512, 256
        write_t3x(img.resize((w, h), Image.BOX), os.path.join(romfs, 'tex', key + '.t3x'), work, mipmaps=False)
        with open(os.path.join(romfs, 'tex', key + '.size'), 'w') as f:
            f.write(f'{img.width} {img.height}\n')
    convert_font(content, romfs, work)
    convert_font(content, romfs, work, size=11, chars=''.join(chr(c) for c in range(32, 127)), name='font_ui')
    convert_dialog(content, romfs, work)


if __name__ == '__main__':
    main()
